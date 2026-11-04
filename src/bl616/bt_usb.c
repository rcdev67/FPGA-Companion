/*
  bt_usb.c - Bluetooth through a USB adapter on the BL616

  The onboard BL616 has a radio, but no antenna on the Tang Nano 20K. A
  plain USB Bluetooth adapter on the hub does the radio instead: it speaks
  HCI over USB, and CherryUSB's Bluetooth class carries that. When the class
  finds an adapter it calls usbh_bluetooth_run(); receiving is left to the
  application, which has to start the class's two receive threads.

  This file is the HCI transport BTstack runs on, and the task BTstack runs
  in. Received packets arrive in usbh_bluetooth_hci_read_callback(), in the
  class's receive threads; they are queued and handed to BTstack on its own
  task, the only one BTstack may be called from. Sending is synchronous: the
  write returns when the adapter has the packet, and BTstack is then told,
  from its own task, that it may send the next one.

  The first adapter starts BTstack, which then stays. An adapter pulled and
  plugged in again powers the stack down and up. bt_hid.c is the HID host
  on top, taken from the Raspberry Pi Pico port.
*/

#include <stdio.h>
#include <string.h>

#include "usbh_core.h"
#include "usbh_bluetooth.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// the parts of BTstack used here; btstack.h would pull in every profile
// and codec, including ones whose third-party sources are not built
#include "btstack_config.h"
#include "btstack_defines.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_freertos.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_transport.h"
#include "classic/btstack_link_key_db.h"
#include "btstack_tlv.h"
#include "ble/le_device_db_tlv.h"
#include "hal_time_ms.h"

#include "../debug.h"
#include "../bluetooth.h"
#include "../sdc.h"

// ------------------------------------------------------------- receiving --
// One item per received packet: the H4 type byte, then the packet.
typedef struct {
  uint16_t len;
  uint8_t data[1 + 4 + HCI_ACL_PAYLOAD_SIZE];
} bt_packet_t;

// Three, each the size of the largest packet: a full queue loses nothing,
// the receive threads wait and the adapter holds on to what it has. Every
// item is a kilobyte of heap, which Low Energy's stack needs more.
#define BT_RX_QUEUE_LENGTH 3

static QueueHandle_t bt_rx_queue;
static bt_packet_t bt_rx_out;           // taken from the queue, on BTstack's task

// The event thread only delivers events, the ACL thread only ACL data, so
// one staging item each and the two never share.
static bt_packet_t bt_rx_in[2];

static void (*bt_packet_handler)(uint8_t type, uint8_t *packet, uint16_t size);
static btstack_data_source_t bt_data_source;
static volatile bool bt_packet_sent;    // set after a write, cleared on BTstack's task
static volatile bool bt_adapter;        // an adapter is there to talk to

const btstack_link_key_db_t *bt_keys_instance(void);   // bt_keys.c
const btstack_tlv_t *bt_tlv_instance(void);            // bt_tlv.c

// bt_rtk.c: a Realtek adapter's patch is loaded before BTstack takes over
bool bt_rtk_active(void);
void bt_rtk_event(const uint8_t *data, uint32_t len);
void bt_rtk_setup(uint16_t vid, uint16_t pid);

void usbh_bluetooth_hci_read_callback(uint8_t *data, uint32_t len) {
  if(!len) return;
  if(bt_rtk_active()) {
    bt_rtk_event(data, len);
    return;
  }
  if(!bt_rx_queue) return;

  bt_packet_t *p = &bt_rx_in[data[0] == USB_BLUETOOTH_HCI_ACL];
  if(len > sizeof(p->data)) {
    bt_debugf("packet of %lu bytes dropped, too long", (unsigned long)len);
    return;
  }
  p->len = len;
  memcpy(p->data, data, len);
  xQueueSend(bt_rx_queue, p, portMAX_DELAY);
  btstack_run_loop_freertos_trigger();
}

// polled by BTstack's run loop on every round
static void bt_transport_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t type) {
  (void)ds;
  if(type != DATA_SOURCE_CALLBACK_POLL || !bt_packet_handler) return;

  if(bt_packet_sent) {
    static const uint8_t sent[] = { HCI_EVENT_TRANSPORT_PACKET_SENT, 0 };
    bt_packet_sent = false;
    bt_packet_handler(HCI_EVENT_PACKET, (uint8_t *)sent, sizeof(sent));
  }

  while(xQueueReceive(bt_rx_queue, &bt_rx_out, 0) == pdTRUE) {
    uint8_t type = bt_rx_out.data[0] == USB_BLUETOOTH_HCI_ACL ? HCI_ACL_DATA_PACKET : HCI_EVENT_PACKET;
    bt_packet_handler(type, bt_rx_out.data + 1, bt_rx_out.len - 1);
  }
}

// ------------------------------------------------------------- transport --
static void bt_transport_init(const void *config) {
  (void)config;
}

static int bt_transport_open(void) {
  btstack_run_loop_set_data_source_handler(&bt_data_source, &bt_transport_process);
  btstack_run_loop_enable_data_source_callbacks(&bt_data_source, DATA_SOURCE_CALLBACK_POLL);
  btstack_run_loop_add_data_source(&bt_data_source);
  return 0;
}

static int bt_transport_close(void) {
  btstack_run_loop_disable_data_source_callbacks(&bt_data_source, DATA_SOURCE_CALLBACK_POLL);
  btstack_run_loop_remove_data_source(&bt_data_source);
  return 0;
}

static void bt_transport_register_packet_handler(void (*handler)(uint8_t type, uint8_t *packet, uint16_t size)) {
  bt_packet_handler = handler;
}

static int bt_transport_can_send_now(uint8_t type) {
  (void)type;
  return bt_adapter && !bt_packet_sent;
}

static int bt_transport_send_packet(uint8_t type, uint8_t *packet, int size) {
  int ret;
  switch(type) {
  case HCI_COMMAND_DATA_PACKET:
    ret = usbh_bluetooth_hci_write(USB_BLUETOOTH_HCI_CMD, packet, size);
    break;
  case HCI_ACL_DATA_PACKET:
    ret = usbh_bluetooth_hci_write(USB_BLUETOOTH_HCI_ACL, packet, size);
    break;
  default:
    return -1;
  }
  if(ret < 0) bt_debugf("send of type %u failed, %d", type, ret);

  // tell BTstack from its own task, on the next round, that it may go on
  bt_packet_sent = true;
  btstack_run_loop_freertos_trigger();
  return 0;
}

static const hci_transport_t bt_transport = {
  /* name */                    "CherryUSB",
  /* init */                    &bt_transport_init,
  /* open */                    &bt_transport_open,
  /* close */                   &bt_transport_close,
  /* register_packet_handler */ &bt_transport_register_packet_handler,
  /* can_send_packet_now */     &bt_transport_can_send_now,
  /* send_packet */             &bt_transport_send_packet,
  /* set_baudrate */            NULL,
  /* reset_link */              NULL,
  /* set_sco_config */          NULL,
};

// --------------------------------------------------------------- logging --
static void bt_log_reset(void) { }

// Every packet, shortened, when BT_LOG_PACKETS is 1: for bringing a new
// adapter up. It was how the stalled inquiry and the Realtek patch were found.
#define BT_LOG_PACKETS 0

static void bt_log_packet(uint8_t type, uint8_t in, uint8_t *packet, uint16_t len) {
  if(!BT_LOG_PACKETS) return;
  char hex[3 * 12 + 1];
  uint16_t n = len < 12 ? len : 12;
  for(uint16_t i = 0; i < n; i++)
    sprintf(hex + 3 * i, "%02x ", packet[i]);
  hex[3 * n] = '\0';
  printf("\033[0;35mHCI %s %s %3u: %s%s\033[0m\r\n", in ? "<-" : "->",
         type == HCI_COMMAND_DATA_PACKET ? "cmd" : type == HCI_EVENT_PACKET ? "evt" :
         type == HCI_ACL_DATA_PACKET ? "acl" : "???", len, hex, len > n ? "..." : "");
}

static void bt_log_message(int level, const char *format, va_list argptr) {
  printf(level == HCI_DUMP_LOG_LEVEL_ERROR ? "\033[1;31mBTS: " : "\033[0;36mBTS: ");
  vprintf(format, argptr);
  printf("\033[0m\r\n");
}

static const hci_dump_t bt_log = {
  .reset = bt_log_reset,
  .log_packet = bt_log_packet,
  .log_message = bt_log_message,
};

uint32_t hal_time_ms(void) {
  return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// ------------------------------------------------------------ the task --
static TaskHandle_t bt_task_handle;

// 8 KB. Measured with both keyboards connecting and one dropping out: 2.6 KB
// used; pairing a Low Energy device, micro-ecc's maths included, worked with
// as much. The heap is short, every connection takes some of it.
#define BT_TASK_STACK 2048   // words

static void bt_task(void *arg) {
  (void)arg;

  btstack_memory_init();
  btstack_run_loop_init(btstack_run_loop_freertos_get_instance());
  hci_dump_init(&bt_log);
  hci_init(&bt_transport, NULL);
  // pairings are kept on the SD card: classic ones by bt_keys.c, Low
  // Energy bondings by bt_tlv.c
  hci_set_link_key_db(bt_keys_instance());
  btstack_tlv_set_instance(bt_tlv_instance(), NULL);
  le_device_db_tlv_configure(bt_tlv_instance(), NULL);

  bluetooth_init();   // bt_hid.c: L2CAP, HID hosts, power on
  bluetooth_run();    // never returns
}

static void bt_power(void *on) {
  hci_power_control(on ? HCI_POWER_ON : HCI_POWER_OFF);
}

static btstack_context_callback_registration_t bt_power_on  = { .callback = &bt_power, .context = (void *)1 };
static btstack_context_callback_registration_t bt_power_off = { .callback = &bt_power, .context = NULL };

// Prepares a freshly plugged adapter -- the Realtek patch -- and only then
// hands it to BTstack: starts the stack the first time, powers it up after.
static uint16_t bt_vid, bt_pid;

static void bt_setup_task(void *arg) {
  (void)arg;
  bt_rtk_setup(bt_vid, bt_pid);
  bt_adapter = true;

  if(!bt_task_handle) {
    // The pairings are on the SD card. A paired keyboard calls within a
    // second, and the Atari ST's FPGA answers only after 2.4 s, the card
    // comes after that: asked for a key before, the companion stopped. So
    // the stack starts with the card, or after ten seconds without one.
    for(int i = 0; i < 100 && !sdc_is_initialized(); i++) {
      if(i == 0) bt_debugf("waiting for the SD card before starting");
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    bt_rx_queue = xQueueCreate(BT_RX_QUEUE_LENGTH, sizeof(bt_packet_t));
    if(xTaskCreate(bt_task, "bluetooth", BT_TASK_STACK, NULL, 10, &bt_task_handle) != pdPASS)
      bt_debugf("no memory for the Bluetooth task");
  } else
    btstack_run_loop_execute_on_main_thread(&bt_power_on);
  vTaskDelete(NULL);
}

void usbh_bluetooth_run(struct usbh_bluetooth *bluetooth_class) {
  struct usbh_hubport *hport = bluetooth_class->hport;
  bt_vid = hport->device_desc.idVendor;
  bt_pid = hport->device_desc.idProduct;
  bt_debugf("adapter %04x:%04x found", bt_vid, bt_pid);

  usb_osal_thread_create("bt_evt", 2048, CONFIG_USBHOST_PSC_PRIO,
                         usbh_bluetooth_hci_evt_rx_thread, NULL);
  usb_osal_thread_create("bt_acl", 2048, CONFIG_USBHOST_PSC_PRIO,
                         usbh_bluetooth_hci_acl_rx_thread, NULL);

  // not from here: this runs inside the USB enumeration, which the
  // adapter's control transfers would have to wait for
  xTaskCreate(bt_setup_task, "bt setup", 1024, NULL, 10, NULL);   // 4 KB
}

bool bt_usb_started(void) {
  return bt_task_handle != NULL;
}

void usbh_bluetooth_stop(struct usbh_bluetooth *bluetooth_class) {
  (void)bluetooth_class;
  bt_debugf("adapter gone");
  bt_adapter = false;
  if(bt_task_handle)
    btstack_run_loop_execute_on_main_thread(&bt_power_off);
}
