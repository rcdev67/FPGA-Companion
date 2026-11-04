/*
  bt_hid.c

  Bluetooth HID host for the BL616, on a USB Bluetooth adapter (bt_usb.c).

  This is the Raspberry Pi Pico port's rp2040/bluetooth.c, with what the
  BL616 needs changed: BTstack is started by bt_usb.c once an adapter is
  there, the OSD's scan request reaches BTstack on its own task, the
  keyboard is paired without a PIN where it allows that, and the first
  scan starts by itself when the stack is up.

  Next to classic Bluetooth, Low Energy: keyboards and controllers that
  speak only that, like the ROG Azoth X in its Bluetooth mode, are found by
  their advertising, paired, and read through BTstack's HID over GATT host.

  Based on:
  https://github.com/bluekitchen/btstack/tree/master/example/spp_streamer_client.c
  https://github.com/bluekitchen/btstack/tree/master/example/hid_host_demo.c
  https://github.com/bluekitchen/btstack/tree/master/example/hog_host_demo.c
*/

#include <string.h>

#include "../debug.h"
#include "../menu.h"
#include "../hid.h"
#include "../hidparser.h"
#include "../bluetooth.h"

// the parts of BTstack used here, see bt_usb.c
#include "btstack_config.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "classic/hid_host.h"
#include "btstack_hid_parser.h"
#include "ad_parser.h"
#include "bluetooth_data_types.h"
#include "bluetooth_gatt.h"
#include "ble/att_db_util.h"
#include "ble/att_server.h"
#include "ble/gatt_client.h"
#include "ble/le_device_db.h"
#include "ble/sm.h"
#include "ble/gatt-service/hids_host.h"

// the first scan, right after the stack is up: 30 x 1.28 s, about 38 s
#define FIRST_SCAN_UNITS 30

#define MAX_ATTRIBUTE_VALUE_SIZE 300

// Four: a keyboard, a mouse and two controllers, as much as a computer of
// the time had; more goes on USB. Each slot is about a kilobyte of RAM,
// and the RAM here is the heap's as well. Each connection takes some
// 3 KB of heap: measured with two USB devices on the hub and three
// Bluetooth ones connected, 10 KB were left.
#define MAX_BT_HID_DEVICES  4
#define MAX_BT_HID_REPORTS  4  // up to four different resports per device supported

typedef enum {
    DEV_UNUSED,
    DEV_DETECTED,
    DEV_CONNECTING
} bt_device_state_t;

static struct bt_hid_device_S {
  bt_device_state_t dev_state;
  bd_addr_t bd_addr;
  uint16_t hid_host_cid;
  hid_state_t state[MAX_BT_HID_REPORTS];
  hid_report_t rep[MAX_BT_HID_REPORTS];  
  // Low Energy only
  bool le;
  bd_addr_type_t addr_type;
  hci_con_handle_t con_handle;
  uint16_t hids_cid;
  uint8_t service[MAX_BT_HID_REPORTS];   // the HID service each report is from
  bool unmatched_logged;
} bt_hid_device[MAX_BT_HID_DEVICES];

static btstack_packet_callback_registration_t hci_event_callback_registration;

// SDP
static uint8_t hid_descriptor_storage[MAX_ATTRIBUTE_VALUE_SIZE];

//static hid_protocol_mode_t hid_host_report_mode = HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT;
static hid_protocol_mode_t hid_host_report_mode = HID_PROTOCOL_MODE_REPORT;

// The first scan, whenever the stack has come up: at power on, and again
// each time the adapter is plugged in. It starts from a timer, a moment
// after the "working" event, rather than from inside BTstack's handling of
// it.
//
// BTstack keeps an inquiry it believes running across a power down: pull the
// adapter during a scan, plug it in again, and gap_inquiry_start() refuses
// with "command disallowed" -- seen on the board, nothing was ever sent. Then
// the stale inquiry is cancelled and the start tried again a second later.
static btstack_timer_source_t first_scan_timer;
static int first_scan_tries;

int bt_keys_count(void);   // bt_keys.c
void mcu_hw_print_heaps(void);   // mcu_hw.c
void mcu_hw_print_stacks(void);  // mcu_hw.c
static int le_bonds_count(void);
static void le_reconnect(void);
static void le_scan_start(uint32_t ms);

static void first_scan(btstack_timer_source_t *ts) {
  if(!first_scan_tries)
    le_reconnect();
  int paired = bt_keys_count() + le_bonds_count();
  if(paired) {
    // a paired keyboard connects by itself; a scan would only be in its way
    bt_debugf("%d device%s paired, waiting for %s; Scan Bluetooth in the OSD pairs another",
              paired, paired == 1 ? "" : "s", paired == 1 ? "it" : "them");
    return;
  }
  int ret = gap_inquiry_start(FIRST_SCAN_UNITS);
  if(ret == ERROR_CODE_SUCCESS) {
    bt_debugf("scanning for %d seconds", FIRST_SCAN_UNITS * 128 / 100);
    le_scan_start(FIRST_SCAN_UNITS * 1280);
    return;
  }
  if(++first_scan_tries > 3) {
    bt_debugf("scan not started, error 0x%02x; Low Energy only", ret);
    le_scan_start(FIRST_SCAN_UNITS * 1280);
    return;
  }
  gap_inquiry_stop();
  btstack_run_loop_set_timer(ts, 1000);
  btstack_run_loop_add_timer(ts);
}

// ------------------------------------------------------- key timing --
// A keyboard in its power saving mode collects reports and sends them in
// bursts: on the board a key was reported held for 1 to 12 ms, its press and
// release arriving together. The C64 scans its keyboard every 16.7 ms, so
// such a key fell between two scans, or went into one scan with the next:
// keys went missing or came twice. So every keyboard state is held for at
// least KEY_STATE_MIN_MS before the next one is applied, in order. That only
// slows anything down beyond about 30 keys a second.
#define KEY_STATE_MIN_MS 30
#define KEY_QUEUE_LEN    16

static struct {
  struct bt_hid_device_S *dev;
  int rep;
  uint16_t len;
  uint8_t data[24];   // the ROG Azoth X: report id and 16 bytes
} key_queue[KEY_QUEUE_LEN];
static int key_queue_head, key_queue_count;
static uint32_t key_last_ms;
static btstack_timer_source_t key_timer;
static bool key_timer_armed;

static void le_kbd_apply(struct bt_hid_device_S *dev, int rep, const uint8_t *data, uint16_t len);

static void key_apply(struct bt_hid_device_S *dev, int rep, const uint8_t *data, uint16_t len) {
  if(dev->le)
    le_kbd_apply(dev, rep, data, len);
  else
    hid_parse(&dev->rep[rep], &dev->state[rep], data, len);
  key_last_ms = btstack_run_loop_get_time_ms();
}

static void key_arm(uint32_t ms);

static void key_timer_handler(btstack_timer_source_t *ts) {
  (void)ts;
  key_timer_armed = false;
  if(!key_queue_count) return;
  int i = key_queue_head;
  key_apply(key_queue[i].dev, key_queue[i].rep, key_queue[i].data, key_queue[i].len);
  key_queue_head = (key_queue_head + 1) % KEY_QUEUE_LEN;
  key_queue_count--;
  if(key_queue_count) key_arm(KEY_STATE_MIN_MS);
}

static void key_arm(uint32_t ms) {
  btstack_run_loop_remove_timer(&key_timer);   // BTstack asserts on adding it twice
  btstack_run_loop_set_timer_handler(&key_timer, &key_timer_handler);
  btstack_run_loop_set_timer(&key_timer, ms);
  btstack_run_loop_add_timer(&key_timer);
  key_timer_armed = true;
}

// a report for one of the device's parsed reports
static void bt_hid_input(struct bt_hid_device_S *dev, int rep, const uint8_t *data, uint16_t len) {
  if(dev->rep[rep].type != REPORT_TYPE_KEYBOARD || len > sizeof(key_queue[0].data)) {
    hid_parse(&dev->rep[rep], &dev->state[rep], data, len);
    return;
  }

  uint32_t since = btstack_run_loop_get_time_ms() - key_last_ms;
  if(!key_queue_count && since >= KEY_STATE_MIN_MS) {
    key_apply(dev, rep, data, len);
    return;
  }

  if(key_queue_count == KEY_QUEUE_LEN) {
    // a full queue is a keyboard gone mad; apply the oldest now
    key_timer_handler(NULL);
  }
  int i = (key_queue_head + key_queue_count) % KEY_QUEUE_LEN;
  key_queue[i].dev = dev;
  key_queue[i].rep = rep;
  key_queue[i].len = len;
  memcpy(key_queue[i].data, data, len);
  key_queue_count++;
  if(!key_timer_armed)
    key_arm(since >= KEY_STATE_MIN_MS ? 0 : KEY_STATE_MIN_MS - since);
}

// The connection is gone: apply what is still queued, then let go of every
// key, or the C64 would keep the last one pressed.
static void bt_hid_release(struct bt_hid_device_S *dev) {
  while(key_queue_count && key_queue[key_queue_head].dev == dev)
    key_timer_handler(NULL);

  static const uint8_t none[32];
  for(int i = 0; i < MAX_BT_HID_REPORTS; i++)
    if(dev->rep[i].type == REPORT_TYPE_KEYBOARD)
      kbd_parse_usage_state(&dev->state[i].kbd, 0, none);
}

static struct bt_hid_device_S *get_dev_by_hid_cid(uint16_t cid) {
  for(int i=0;i<MAX_BT_HID_DEVICES;i++)
    if(bt_hid_device[i].dev_state != DEV_UNUSED && !bt_hid_device[i].le)
      if(bt_hid_device[i].hid_host_cid == cid)
	return &bt_hid_device[i];
  
  return NULL;
}

// ------------------------------------------------------- Low Energy --
// A Low Energy device is not found by an inquiry but by its advertising:
// a scan runs alongside every inquiry. A device that advertises the HID
// service, or a HID appearance, is connected, paired with bonding, so it is
// known the next time, and read through hids_host, as in hog_host_demo.c.
//
// Bonded devices are connected to again whenever they advertise: they are
// on the adapter's white list. BTstack stops connecting from it at every
// connection made or failed, so le_reconnect() starts it again after each.
// To connect a newly found device directly, the white list is put aside
// first and the connect sent a moment later, once the adapter has let go.

#define LE_CONNECT_TIMEOUT_MS 10000

static btstack_packet_callback_registration_t sm_event_callback_registration;
static uint8_t hids_descriptor_storage[1000];
static btstack_timer_source_t le_scan_timer, le_connect_timer;
static bool le_scanning;
static struct bt_hid_device_S *le_pending;   // found by the scan, being connected
static bool le_connect_sent;
static int le_connect_tries;

static int le_bonds_count(void) {
  int n = 0;
  for(int i = 0; i < le_device_db_max_count(); i++) {
    int addr_type;
    bd_addr_t addr;
    le_device_db_info(i, &addr_type, addr, NULL);
    if(addr_type != BD_ADDR_TYPE_UNKNOWN) n++;
  }
  return n;
}

// A bonded device the heap had no room for. It used to be let go and taken
// back at once, several times a second for as long as it was switched on:
// the radio was busy with that, and the classic keyboard waited a minute
// for its turn. It now waits 30 s before it is taken again.
#define LE_NO_MEMORY_PAUSE_MS 30000
static bd_addr_t le_no_memory_addr;
static bool le_no_memory;
static btstack_timer_source_t le_no_memory_timer;

static void le_reconnect(void) {
  if(le_pending) return;   // a new one is being connected
  for(int i = 0; i < le_device_db_max_count(); i++) {
    int addr_type;
    bd_addr_t addr;
    le_device_db_info(i, &addr_type, addr, NULL);
    if(addr_type == BD_ADDR_TYPE_UNKNOWN) continue;
    if(le_no_memory && bd_addr_cmp(addr, le_no_memory_addr) == 0) continue;
    gap_auto_connection_start((bd_addr_type_t)addr_type, addr);
  }
}

static btstack_timer_source_t bt_report_timer;

static void bt_report(btstack_timer_source_t *ts) {
  static int n;
  if(n++ == 0) bt_debugf("a minute after the start:");
  else         bt_debugf("%d minutes after the start:", 1 + 10 * (n - 1));
  mcu_hw_print_heaps();
  mcu_hw_print_stacks();
  // and every ten minutes after, to catch what a download or the menu used
  btstack_run_loop_set_timer(ts, 600000);
  btstack_run_loop_add_timer(ts);
}

static void le_no_memory_over(btstack_timer_source_t *ts) {
  (void)ts;
  le_no_memory = false;
  le_reconnect();
}

static void le_no_memory_pause(struct bt_hid_device_S *dev) {
  bt_debugf("no memory for %s as well; it is taken again in %d s",
            bd_addr_to_str(dev->bd_addr), LE_NO_MEMORY_PAUSE_MS / 1000);
  mcu_hw_print_heaps();
  gap_auto_connection_stop(dev->addr_type, dev->bd_addr);
  memcpy(le_no_memory_addr, dev->bd_addr, sizeof(bd_addr_t));
  le_no_memory = true;
  btstack_run_loop_remove_timer(&le_no_memory_timer);
  btstack_run_loop_set_timer_handler(&le_no_memory_timer, &le_no_memory_over);
  btstack_run_loop_set_timer(&le_no_memory_timer, LE_NO_MEMORY_PAUSE_MS);
  btstack_run_loop_add_timer(&le_no_memory_timer);
}

static void le_scan_stop(btstack_timer_source_t *ts) {
  (void)ts;
  if(!le_scanning) return;
  gap_stop_scan();
  le_scanning = false;
}

static void le_scan_start(uint32_t ms) {
  // active: a scan response often carries what the advertising leaves out
  gap_set_scan_parameters(1, 48, 48);
  gap_start_scan();
  le_scanning = true;
  btstack_run_loop_remove_timer(&le_scan_timer);
  btstack_run_loop_set_timer_handler(&le_scan_timer, &le_scan_stop);
  btstack_run_loop_set_timer(&le_scan_timer, ms);
  btstack_run_loop_add_timer(&le_scan_timer);
}

static bool le_adv_is_hid(const uint8_t *packet) {
  const uint8_t *ad = gap_event_advertising_report_get_data(packet);
  uint8_t len = gap_event_advertising_report_get_data_length(packet);
  if(ad_data_contains_uuid16(len, ad, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE))
    return true;
  // some give only their appearance; 0x03c0 to 0x03ff are the HID ones
  ad_context_t c;
  for(ad_iterator_init(&c, len, ad); ad_iterator_has_more(&c); ad_iterator_next(&c))
    if(ad_iterator_get_data_type(&c) == BLUETOOTH_DATA_TYPE_APPEARANCE &&
       ad_iterator_get_data_len(&c) == 2 &&
       (little_endian_read_16(ad_iterator_get_data(&c), 0) >> 6) == 0x0f)
      return true;
  return false;
}

static struct bt_hid_device_S *le_dev_by_handle(hci_con_handle_t handle) {
  for(int i = 0; i < MAX_BT_HID_DEVICES; i++)
    if(bt_hid_device[i].dev_state != DEV_UNUSED && bt_hid_device[i].le &&
       bt_hid_device[i].con_handle == handle)
      return &bt_hid_device[i];
  return NULL;
}

static struct bt_hid_device_S *le_dev_by_hids_cid(uint16_t cid) {
  for(int i = 0; i < MAX_BT_HID_DEVICES; i++)
    if(bt_hid_device[i].dev_state != DEV_UNUSED && bt_hid_device[i].le &&
       bt_hid_device[i].hids_cid == cid)
      return &bt_hid_device[i];
  return NULL;
}

static struct bt_hid_device_S *le_dev_alloc(const bd_addr_t addr, bd_addr_type_t addr_type) {
  for(int i = 0; i < MAX_BT_HID_DEVICES; i++) {
    struct bt_hid_device_S *dev = &bt_hid_device[i];
    if(dev->dev_state != DEV_UNUSED) continue;
    dev->dev_state = DEV_CONNECTING;
    dev->le = true;
    memcpy(dev->bd_addr, addr, 6);
    dev->addr_type = addr_type;
    dev->con_handle = HCI_CON_HANDLE_INVALID;
    dev->hid_host_cid = 0;
    dev->hids_cid = 0;
    dev->unmatched_logged = false;
    memset(dev->state, 0, sizeof(dev->state));   // no keys held
    for(int r = 0; r < MAX_BT_HID_REPORTS; r++)
      dev->rep[r].type = REPORT_TYPE_NONE;
    return dev;
  }
  return NULL;
}

static void le_dev_free(struct bt_hid_device_S *dev) {
  bt_hid_release(dev);
  for(int r = 0; r < MAX_BT_HID_REPORTS; r++) {
    if(dev->rep[r].type == REPORT_TYPE_JOYSTICK)
      hid_release_joystick(dev->state[r].joystick.js_index);
    dev->rep[r].type = REPORT_TYPE_NONE;
  }
  if(dev == le_pending) {
    le_pending = NULL;
    btstack_run_loop_remove_timer(&le_connect_timer);
  }
  dev->dev_state = DEV_UNUSED;
  dev->le = false;
  dev->con_handle = HCI_CON_HANDLE_INVALID;
  dev->hids_cid = 0;
}

static void le_give_up(void) {
  if(le_pending) le_dev_free(le_pending);
  le_reconnect();
}

static void le_connect_timeout(btstack_timer_source_t *ts) {
  (void)ts;
  bt_debugf("no answer from %s", le_pending ? bd_addr_to_str(le_pending->bd_addr) : "it");
  gap_connect_cancel();
  le_give_up();
}

static void le_connect_pending(btstack_timer_source_t *ts) {
  if(!le_pending) return;
  uint8_t status = gap_connect(le_pending->bd_addr, le_pending->addr_type);
  if(status == ERROR_CODE_SUCCESS) {
    le_connect_sent = true;
    btstack_run_loop_set_timer_handler(ts, &le_connect_timeout);
    btstack_run_loop_set_timer(ts, LE_CONNECT_TIMEOUT_MS);
    btstack_run_loop_add_timer(ts);
    return;
  }
  if(++le_connect_tries < 10) {
    btstack_run_loop_set_timer(ts, 200);
    btstack_run_loop_add_timer(ts);
    return;
  }
  if(status == BTSTACK_MEMORY_ALLOC_FAILED)
    bt_debugf("cannot connect: no memory left for another connection");
  else
    bt_debugf("cannot connect, error 0x%02x", status);
  le_give_up();
}

static void le_advertising_report(const uint8_t *packet) {
  if(!le_scanning || le_pending) return;
  // only the connectable ones, and the scan responses to them
  uint8_t type = gap_event_advertising_report_get_advertising_event_type(packet);
  if(type == 2 || type == 3) return;
  if(!le_adv_is_hid(packet)) return;

  bd_addr_t addr;
  gap_event_advertising_report_get_address(packet, addr);
  for(int i = 0; i < MAX_BT_HID_DEVICES; i++)
    if(bt_hid_device[i].dev_state != DEV_UNUSED &&
       bd_addr_cmp(bt_hid_device[i].bd_addr, addr) == 0)
      return;   // connected already

  struct bt_hid_device_S *dev = le_dev_alloc(addr,
      (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet));
  if(!dev) return;
  bt_debugf("Low Energy HID device found: %s", bd_addr_to_str(addr));

  le_pending = dev;
  le_connect_sent = false;
  le_connect_tries = 0;
  btstack_run_loop_remove_timer(&le_scan_timer);
  le_scan_stop(NULL);
  gap_inquiry_stop();
  gap_auto_connection_stop_all();
  btstack_run_loop_remove_timer(&le_connect_timer);
  btstack_run_loop_set_timer_handler(&le_connect_timer, &le_connect_pending);
  btstack_run_loop_set_timer(&le_connect_timer, 200);
  btstack_run_loop_add_timer(&le_connect_timer);
}

static void le_connection_complete(const uint8_t *packet) {
  uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
  bd_addr_t addr;
  gap_subevent_le_connection_complete_get_peer_address(packet, addr);

  if(status != ERROR_CODE_SUCCESS) {
    // also the white list put aside for a new device: nothing to do then
    if(le_pending && le_connect_sent) {
      bt_debugf("connecting %s failed, status 0x%02x", bd_addr_to_str(le_pending->bd_addr), status);
      le_give_up();
    } else
      le_reconnect();
    return;
  }

  hci_con_handle_t handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
  struct bt_hid_device_S *dev = NULL;
  if(le_pending && le_connect_sent && bd_addr_cmp(le_pending->bd_addr, addr) == 0) {
    dev = le_pending;
    le_pending = NULL;
    btstack_run_loop_remove_timer(&le_connect_timer);
  } else {
    // a bonded one, from the white list
    dev = le_dev_alloc(addr, (bd_addr_type_t)gap_subevent_le_connection_complete_get_peer_address_type(packet));
    if(!dev) {
      bt_debugf("no room for %s", bd_addr_to_str(addr));
      gap_disconnect(handle);
      return;
    }
  }
  dev->con_handle = handle;
  bt_debugf("connected to %s, securing the connection", bd_addr_to_str(addr));
  // pairs a new device; one bonded already only encrypts with its key
  sm_request_pairing(handle);
  le_reconnect();
}

static void le_disconnected(hci_con_handle_t handle) {
  struct bt_hid_device_S *dev = le_dev_by_handle(handle);
  if(!dev) return;
  bt_debugf("%s disconnected", bd_addr_to_str(dev->bd_addr));
  bool had_reports = dev->rep[0].type != REPORT_TYPE_NONE;
  le_dev_free(dev);
  if(had_reports)
    menu_notify(MENU_EVENT_BLUETOOTH_DISCONNECTED);
  le_reconnect();
}

static void hids_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void le_hids_connect(struct bt_hid_device_S *dev) {
  if(dev->hids_cid) return;   // pairing and encryption both end here
  uint8_t status = hids_host_connect(dev->con_handle, &hids_packet_handler,
                                     HID_PROTOCOL_MODE_REPORT, &dev->hids_cid);
  if(status != ERROR_CODE_SUCCESS) {
    bt_debugf("HID service not read, status 0x%02x", status);
    if(status == BTSTACK_MEMORY_ALLOC_FAILED) le_no_memory_pause(dev);
    gap_disconnect(dev->con_handle);
  }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
  UNUSED(channel);
  UNUSED(size);
  if(packet_type != HCI_EVENT_PACKET) return;

  struct bt_hid_device_S *dev;
  uint8_t status;

  switch(hci_event_packet_get_type(packet)) {
  case SM_EVENT_JUST_WORKS_REQUEST:
    sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
    break;

  case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
    // nothing here to show the number on: accepted, as "just works"
    sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
    break;

  case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
    bt_debugf("Passkey to type on the keyboard: %06u, then Return",
              (unsigned)sm_event_passkey_display_number_get_passkey(packet));
    break;

  case SM_EVENT_PAIRING_COMPLETE:
    dev = le_dev_by_handle(sm_event_pairing_complete_get_handle(packet));
    if(!dev) break;
    status = sm_event_pairing_complete_get_status(packet);
    if(status == ERROR_CODE_SUCCESS) {
      bt_debugf("paired, reading the HID service");
      le_hids_connect(dev);
    } else {
      bt_debugf("pairing failed, status 0x%02x, reason %u", status,
                sm_event_pairing_complete_get_reason(packet));
      gap_disconnect(dev->con_handle);
    }
    break;

  case SM_EVENT_REENCRYPTION_COMPLETE:
    dev = le_dev_by_handle(sm_event_reencryption_complete_get_handle(packet));
    if(!dev) break;
    status = sm_event_reencryption_complete_get_status(packet);
    if(status == ERROR_CODE_SUCCESS) {
      bt_debugf("encrypted with the stored key");
      le_hids_connect(dev);
    } else {
      // the device has forgotten the pairing, paired with something else
      bt_debugf("stored key refused, status 0x%02x; Scan Bluetooth pairs it again", status);
      if(status == ERROR_CODE_PIN_OR_KEY_MISSING)
        gap_delete_bonding(dev->addr_type, dev->bd_addr);
      gap_disconnect(dev->con_handle);
    }
    break;

  default:
    break;
  }
}

static void le_parse_descriptors(struct bt_hid_device_S *dev, int services) {
  int r = 0;
  for(int s = 0; s < services && r < MAX_BT_HID_REPORTS; s++) {
    const uint8_t *desc = hids_host_descriptor_storage_get_descriptor_data(dev->hids_cid, s);
    uint16_t len = hids_host_descriptor_storage_get_descriptor_len(dev->hids_cid, s);
    bt_debugf("HID service %d: descriptor, %d bytes", s, len);
    while(r < MAX_BT_HID_REPORTS && len) {
      uint16_t rbytes = 0;
      if(!parse_report_descriptor(desc, len, &dev->rep[r], &rbytes) || !rbytes || rbytes > len) {
        dev->rep[r].type = REPORT_TYPE_NONE;
        break;
      }
      dev->service[r] = s;
      if(dev->rep[r].type == REPORT_TYPE_JOYSTICK)
        dev->state[r].joystick.js_index = hid_allocate_joystick();
      bt_debugf("  report %d: type %d, id %d, %d bytes", r, dev->rep[r].type,
                dev->rep[r].report_id_present ? dev->rep[r].report_id : -1,
                dev->rep[r].report_size);
      len -= rbytes;
      desc += rbytes;
      r++;
    }
  }
}

// A Low Energy keyboard's report is read by its own descriptor, with
// BTstack's HID parser: kbd_parse() knows only the 8 byte boot layout, and
// the ROG Azoth X sends 16 bytes, a bit for each key and the modifiers last.
// Whatever the layout, the keys end up as the bit field hid.c works with.
static void le_kbd_apply(struct bt_hid_device_S *dev, int rep, const uint8_t *data, uint16_t len) {
  btstack_hid_parser_t parser;
  btstack_hid_parser_init(&parser,
      hids_host_descriptor_storage_get_descriptor_data(dev->hids_cid, dev->service[rep]),
      hids_host_descriptor_storage_get_descriptor_len(dev->hids_cid, dev->service[rep]),
      HID_REPORT_TYPE_INPUT, data, len);

  uint8_t modifiers = 0;
  uint8_t keys[32] = { 0 };
  while(btstack_hid_parser_has_more(&parser)) {
    uint16_t page, usage;
    int32_t value;
    btstack_hid_parser_get_field(&parser, &page, &usage, &value);
    if(page != HID_USAGE_PAGE_KEYBOARD || !value) continue;
    if(usage >= 0xe0 && usage <= 0xe7)
      modifiers |= (uint8_t)(1U << (usage - 0xe0));
    else if(usage >= 1 && usage <= 3)
      return;   // too many keys at once: the state stays as it was
    else if(usage >= 4 && usage <= 0xff)
      keys[usage / 8] |= (uint8_t)(1U << (usage % 8));
  }
  kbd_parse_usage_state(&dev->state[rep].kbd, modifiers, keys);
}

static void le_report(struct bt_hid_device_S *dev, uint8_t service, const uint8_t *data, uint16_t len) {
  // hids_host puts the report id first, 0 for a report without one
  if(len < 2) return;
  for(int i = 0; i < MAX_BT_HID_REPORTS; i++) {
    if(dev->rep[i].type == REPORT_TYPE_NONE || dev->service[i] != service)
      continue;
    if(!dev->rep[i].report_id_present) {
      bt_hid_input(dev, i, data + 1, len - 1);
      return;
    }
    if(data[0] == dev->rep[i].report_id && len - 1 == dev->rep[i].report_size) {
      bt_hid_input(dev, i, data, len);
      return;
    }
  }
  if(!dev->unmatched_logged) {
    // once per connection, enough to see what a new device sends
    bt_debugf("report %d of %d bytes from service %d not used", data[0], len - 1, service);
    dev->unmatched_logged = true;
  }
}

static void hids_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
  UNUSED(packet_type);
  UNUSED(channel);
  UNUSED(size);
  if(hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

  struct bt_hid_device_S *dev;
  switch(hci_event_gattservice_meta_get_subevent_code(packet)) {
  case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
    dev = le_dev_by_hids_cid(gattservice_subevent_hid_service_connected_get_hids_cid(packet));
    if(!dev) break;
    uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
    if(status != ERROR_CODE_SUCCESS) {
      bt_debugf("HID service not read, status 0x%02x", status);
      gap_disconnect(dev->con_handle);
      break;
    }
    le_parse_descriptors(dev, gattservice_subevent_hid_service_connected_get_num_instances(packet));
    if(dev->rep[0].type == REPORT_TYPE_NONE) {
      bt_debugf("no usable report");
      gap_disconnect(dev->con_handle);
    } else {
      bt_debugf("HID over GATT ready");
      menu_notify(MENU_EVENT_BLUETOOTH_CONNECTED);
    }
  } break;

  case GATTSERVICE_SUBEVENT_HID_REPORT:
    dev = le_dev_by_hids_cid(gattservice_subevent_hid_report_get_hids_cid(packet));
    if(dev)
      le_report(dev, gattservice_subevent_hid_report_get_service_index(packet),
                gattservice_subevent_hid_report_get_report(packet),
                gattservice_subevent_hid_report_get_report_len(packet));
    break;

  default:
    break;
  }
}

// A device may read the host's own GATT database, as phones and computers
// have one; a request left unanswered ends the connection after 30 s.
static uint8_t le_device_name[] = "MiSTle FPGA Companion";

static void le_init(void) {
  sm_init();
  sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
  sm_event_callback_registration.callback = &sm_packet_handler;
  sm_add_event_handler(&sm_event_callback_registration);

  // Looking out for bonded devices runs all the time, so only 30 ms in
  // every 200, not BTstack's half the time, which leaves the radio to
  // classic Bluetooth. A connection is dropped after 2 s without a word
  // rather than 0.72 s, for the same reason.
  gap_set_connection_parameters(0x0140, 0x0030, 0x0008, 0x0018, 4, 0x00c8, 0, 0);

  // hids_host before gatt_client, so it gets a disconnection first. The
  // other way round gatt_client frees its context for the connection,
  // hids_host's clean-up then asks for it and gets a new one, and once the
  // connection is gone that one wants to send for ever: BTstack recursed
  // until the stack overflowed, each time the ROG Azoth X was switched off.
  hids_host_init(hids_descriptor_storage, sizeof(hids_descriptor_storage));
  gatt_client_init();

  att_db_util_init();
  att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS);
  att_db_util_add_characteristic_uuid16(ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME,
      ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE,
      le_device_name, sizeof(le_device_name) - 1);
  att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ATTRIBUTE);
  att_server_init(att_db_util_get_address(), NULL, NULL);
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
  UNUSED(channel);
  UNUSED(size);

  //bt_debugf("packet_handler(%d, %d, %d)", packet_type, channel, size);
  //  if(size) hexdump(packet, size);
  
  bd_addr_t event_addr;
  uint32_t  class_of_device;
  uint8_t   status;
  
  switch (packet_type) {
  case HCI_EVENT_PACKET:
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
      if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
	bt_debugf("BTStack up and working");
	mcu_hw_print_heaps();   // what the connections have to share
	// and again a minute later, with the devices in, and the stacks too
	btstack_run_loop_remove_timer(&bt_report_timer);
	btstack_run_loop_set_timer_handler(&bt_report_timer, &bt_report);
	btstack_run_loop_set_timer(&bt_report_timer, 60000);
	btstack_run_loop_add_timer(&bt_report_timer);
	le_no_memory = false;
	first_scan_tries = 0;
	// what was connected over Low Energy before the adapter went away
	for(int i=0;i<MAX_BT_HID_DEVICES;i++)
	  if(bt_hid_device[i].dev_state != DEV_UNUSED && bt_hid_device[i].le)
	    le_dev_free(&bt_hid_device[i]);
	le_pending = NULL;
	le_scanning = false;
	btstack_run_loop_remove_timer(&le_scan_timer);
	btstack_run_loop_remove_timer(&le_connect_timer);
	btstack_run_loop_remove_timer(&first_scan_timer);   // still pending after a quick replug
	btstack_run_loop_set_timer_handler(&first_scan_timer, &first_scan);
	btstack_run_loop_set_timer(&first_scan_timer, 100);
	btstack_run_loop_add_timer(&first_scan_timer);
      }
      break;

    case GAP_EVENT_INQUIRY_RESULT:
      // get class of device, mask Limited Discoverable Mode
      class_of_device = gap_event_inquiry_result_get_class_of_device(packet) & 0xdfff;
      gap_event_inquiry_result_get_bd_addr(packet, event_addr);

      // https://bluetooth-pentest.narod.ru/software/bluetooth_class_of_device-service_generator.html      
      // COD 504 - Peripheral; Joystick      
      // COD 508 - Peripheral; Gamepad      
      // COD 540 - Peripheral; Keyboard
      // COD 580 - Peripheral; Pointing Device
      if((class_of_device == 0x504) || (class_of_device == 0x508) ||
	 (class_of_device == 0x540) || (class_of_device == 0x580)) {
	bt_debugf("%s class device found: %s",
		  (class_of_device == 0x504)?"Joystick":
		  (class_of_device == 0x508)?"Gamepad":
		  (class_of_device == 0x540)?"Keyboard":
		  "Mouse", bd_addr_to_str(event_addr));

	// One already connected answers a scan too. Connecting it again failed
	// and the clean-up dropped the working connection; seen on the board.
	for(int i=0;i<MAX_BT_HID_DEVICES;i++)
	  if(bt_hid_device[i].dev_state != DEV_UNUSED &&
	     bd_addr_cmp(bt_hid_device[i].bd_addr, event_addr) == 0) {
	    bt_debugf("%s is connected already", bd_addr_to_str(event_addr));
	    return;
	  }

	// search a free hid slot
	for(int i=0;i<MAX_BT_HID_DEVICES;i++) {
	  if(bt_hid_device[i].dev_state == DEV_UNUSED) {
	    memcpy(bt_hid_device[i].bd_addr, event_addr, 6);
	    bt_hid_device[i].dev_state = DEV_DETECTED;
	    bt_hid_device[i].le = false;

	    gap_inquiry_stop();
	    return;
	  }
	}
      } else
	bt_debugf("Unsupported device found: %s with COD: 0x%06x",
		  bd_addr_to_str(event_addr), (int) class_of_device);
      break;
      
    case GAP_EVENT_INQUIRY_COMPLETE:
      bt_debugf("GAP_EVENT_INQUIRY_COMPLETE");
      
      // check if one entry contains a freshly detected device
      for(int i=0;i<MAX_BT_HID_DEVICES;i++) {
	if(bt_hid_device[i].dev_state == DEV_DETECTED) {
	  
	  bt_debugf("Start to connect and query for HID service: %s",
		    bd_addr_to_str(bt_hid_device[i].bd_addr));
	  
	  status = hid_host_connect(bt_hid_device[i].bd_addr,
				    hid_host_report_mode,
				    &bt_hid_device[i].hid_host_cid);
	  if (status != ERROR_CODE_SUCCESS) {
	    // TODO: this sometimes reports a failure "ERROR_CODE_COMMAND_DISALLOWED" and
	    // is actually connected, anyway.	    
	    bt_debugf("HID host connect failed, status 0x%02x.", status);
	    bt_hid_device[i].dev_state = DEV_UNUSED;
	    // nothing to disconnect: no connection was made, and the cid in the
	    // slot is stale
	  } else {
	    bt_debugf("HID host connected, cid 0x%x.", bt_hid_device[i].hid_host_cid);
	    bt_hid_device[i].dev_state = DEV_CONNECTING;
	  }
	  
	  return;
	}
	break;
      }
      break;
      
    case GAP_EVENT_ADVERTISING_REPORT:
      le_advertising_report(packet);
      break;

    case HCI_EVENT_META_GAP:
      if(hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
	le_connection_complete(packet);
      break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
      le_disconnected(hci_event_disconnection_complete_get_connection_handle(packet));
      break;

    case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
      // the keyboard wants this typed on it, followed by Return
      bt_debugf("Passkey to type on the keyboard: %06u, then Return",
		(unsigned)hci_event_user_passkey_notification_get_numeric_value(packet));
      break;

    case HCI_EVENT_PIN_CODE_REQUEST:
      // inform about pin code request
      bt_debugf("Pin code request - using '123456'");
      hci_event_pin_code_request_get_bd_addr(packet, event_addr);
      gap_pin_code_response(event_addr, "123456");
      menu_notify(MENU_EVENT_BLUETOOTH_PIN_CODE_REQUEST);
      break;

    case HCI_EVENT_HID_META:
      // bt_debugf("HCI_EVENT_HID_META(0x%02x)", hci_event_hid_meta_get_subevent_code(packet));
      
      switch (hci_event_hid_meta_get_subevent_code(packet)) {
      case HID_SUBEVENT_SNIFF_SUBRATING_PARAMS:
	bt_debugf("HID_SUBEVENT_SNIFF_SUBRATING_PARAMS");
	break;
	
      case HID_SUBEVENT_INCOMING_CONNECTION: {
	// There is an incoming connection: we can accept it or decline it.
	// The hid_host_report_mode in the hid_host_accept_connection function 
	// allows the application to request a protocol mode. 
	// For available protocol modes, see hid_protocol_mode_t in btstack_hid.h file. 
	uint16_t hid_cid = hid_subevent_incoming_connection_get_hid_cid(packet);

	struct bt_hid_device_S *dev = NULL;

	// find a free entry
	for(int i=0;i<MAX_BT_HID_DEVICES&&!dev;i++) {
	  if(bt_hid_device[i].dev_state == DEV_UNUSED) {
	    dev = &bt_hid_device[i];
	    dev->dev_state = DEV_CONNECTING;   // not DEV_DETECTED, which the end of a scan connects
	    dev->le = false;
	    dev->hid_host_cid = hid_cid;
	    hid_subevent_incoming_connection_get_address(packet, dev->bd_addr);
	  }
	}
	
	if(!dev) {
	  bt_debugf("HID_SUBEVENT_INCOMING_CONNECTION: No matching device entry");	  
	  hid_host_decline_connection(hid_cid);
	  return;
	}

	// Stop any inquiry in progress
	gap_inquiry_stop();
	
	bt_debugf("Accepting incoming connection from %s", bd_addr_to_str(dev->bd_addr));
	hid_host_accept_connection(hid_cid, hid_host_report_mode);
      } break;
	
      case HID_SUBEVENT_CONNECTION_OPENED: {
	struct bt_hid_device_S *dev = get_dev_by_hid_cid(hid_subevent_connection_opened_get_hid_cid(packet));

	if(!dev)
	  bt_debugf("HID_SUBEVENT_CONNECTION_OPENED: No matching device entry");
	else {	
	  // The status field of this event indicates if the control and interrupt
	  // connections were opened successfully.
	  status = hid_subevent_connection_opened_get_status(packet);
	  if (status != ERROR_CODE_SUCCESS) {
	    bt_debugf("Connection failed, status 0x%02x", status);
	    dev->hid_host_cid = 0;
	    return;
	  }

	  dev->hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
	  bt_debugf("HID Host connected.");
	}
      } break;

      case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
	struct bt_hid_device_S *dev = get_dev_by_hid_cid(hid_subevent_descriptor_available_get_hid_cid(packet));

	if(!dev)
	  bt_debugf("HID_SUBEVENT_DESCRIPTOR_AVAILABLE: No matching device entry");
	else {	
	  bt_debugf("HID_SUBEVENT_DESCRIPTOR_AVAILABLE");
	  // This event follows a HID_SUBEVENT_CONNECTION_OPENED event. 
	  // For incoming connections, i.e. HID Device initiating the connection,
	  // the HID_SUBEVENT_DESCRIPTOR_AVAILABLE is delayed, and some HID  
	  // reports may be received via HID_SUBEVENT_REPORT event. It is up to 
	  // the application if these reports should be buffered or ignored until 
	  // the HID descriptor is available.
	  status = hid_subevent_descriptor_available_get_status(packet);
	  if(status == ERROR_CODE_SUCCESS) {
	    const uint8_t *hid_descriptor = hid_descriptor_storage_get_descriptor_data(dev->hid_host_cid);
	    uint16_t hid_descriptor_len = hid_descriptor_storage_get_descriptor_len(dev->hid_host_cid);
	    bt_debugf("hid descriptor, %d bytes", hid_descriptor_len);

	    // Bluetooth devices have a tendency to report mutliple report types. This happens less often with
	    // USB devices as these can have multiple interfaces and can expose different capabilities that way.

	    // clear all reports
	    for(int i=0;i<MAX_BT_HID_REPORTS;i++)
	      dev->rep[i].type = REPORT_TYPE_NONE;

	    // parse all report descriptors
	    for(int i=0;(i<MAX_BT_HID_REPORTS) && hid_descriptor_len;i++)  {
	      uint16_t rbytes;
	      if(parse_report_descriptor(hid_descriptor, hid_descriptor_len, &dev->rep[i], &rbytes)) {
		if(dev->rep[i].type == REPORT_TYPE_JOYSTICK)
		  dev->state[i].joystick.js_index = hid_allocate_joystick();
		
		// report descriptors successfully parsed, try to parse further descriptors
		hid_descriptor_len -= rbytes;
		hid_descriptor += rbytes;

	      } else
		hid_descriptor_len = 0;
	    }
	      
	    // check if any usable report has been found
	    if(dev->rep[0].type == REPORT_TYPE_NONE) {	      
	      // report parsing failed, reject device
	      hid_host_disconnect(dev->hid_host_cid);
	    } else		
	      menu_notify(MENU_EVENT_BLUETOOTH_CONNECTED);

	  } else {
	    fatal_debugf("Cannot handle input report, HID Descriptor is not available, status 0x%02x", status);
	    hid_host_disconnect(dev->hid_host_cid);
	  }
	}
      } break;
	
      case HID_SUBEVENT_REPORT: {
	struct bt_hid_device_S *dev = get_dev_by_hid_cid(hid_subevent_report_get_hid_cid(packet));

	if(!dev)
	  bt_debugf("HID_SUBEVENT_REPORT: No matching device entry");
	else {
	  uint8_t const* data = hid_subevent_report_get_report(packet);
	  uint16_t len = hid_subevent_report_get_report_len(packet);

	  if(data[0] == 0xa1) {	   // report type: "input report"
	    data++; len--;         // skip report type
	    
	    // If no report id is included in a the report, then only
	    // one type of report can exist as otherwise the different
	    // reports could not be distinguished.
	    if(!dev->rep[0].report_id_present) {
	      bt_hid_input(dev, 0, data, len);
	    } else {
	      // report id's are present, so check which report description
	      // the current report matches
	      for(int i=0;i<MAX_BT_HID_REPORTS;i++) {
		// check report id if present
		if(dev->rep[i].report_id_present && (len-1 == dev->rep[i].report_size)) {
		  if(data[0] == dev->rep[i].report_id) {
		    bt_hid_input(dev, i, data, len);
		  }
		}
	      }
	    }
	  }
	}
      } break;
	
      case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
	// For incoming connections, the library will set the protocol mode of the
	// HID Device as requested in the call to hid_host_accept_connection. The event 
	// reports the result. For connections initiated by calling hid_host_connect, 
	// this event will occur only if the established report mode is boot mode.
	status = hid_subevent_set_protocol_response_get_handshake_status(packet);
	if (status != HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL){
	  bt_debugf("Error set protocol, status 0x%02x", status);
	  break;
	}
	switch ((hid_protocol_mode_t)hid_subevent_set_protocol_response_get_protocol_mode(packet)){
	case HID_PROTOCOL_MODE_BOOT:
	  bt_debugf("Protocol mode set: BOOT.");
	  break;  
	case HID_PROTOCOL_MODE_REPORT:
	  bt_debugf("Protocol mode set: REPORT.");
	  break;
	default:
	  bt_debugf("Unknown protocol mode.");
	  break; 
	}
	break;
	
      case HID_SUBEVENT_CONNECTION_CLOSED: {
	struct bt_hid_device_S *dev = get_dev_by_hid_cid(hid_subevent_connection_closed_get_hid_cid(packet));

	if(!dev)
	  bt_debugf("HID_SUBEVENT_CONNECTION_CLOSED: No matching device entry");
	else {	
	  bt_debugf("HID Host disconnected.");
	  // The connection was closed.
	  bt_hid_release(dev);

	  // release all joysticks
	  for(int i=0;i<MAX_BT_HID_REPORTS;i++)
	    if(dev->rep[i].type == REPORT_TYPE_JOYSTICK)
	      hid_release_joystick(dev->state[i].joystick.js_index);
	    
	  // mark the list entry as free/unused
	  dev->dev_state = DEV_UNUSED;
	  dev->hid_host_cid = 0;

	  menu_notify(MENU_EVENT_BLUETOOTH_DISCONNECTED);
	}
      } break;
        
      default:
	break;				    
      }
      break;

    default:
      // bt_debugf("HCI_EVENT(0x%02x)", hci_event_packet_get_type(packet));
      break;
    }
    break;
  }
}

// BTstack's log output is set up in bt_usb.c, before the stack starts

void bluetooth_init(void) {
  bt_debugf("init");

  for(int i=0;i<MAX_BT_HID_DEVICES;i++)
    bt_hid_device[i].dev_state = DEV_UNUSED;

  // ============ setup bluetooth =============
  // Initialize L2CAP
  l2cap_init();

  // Low Energy: Security Manager, GATT client, HID over GATT host
  le_init();

  gap_set_local_name("MiSTle FPGA Companion");

  // Initialize HID Host
  hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
  hid_host_register_packet_handler(packet_handler);
  
  // Allow sniff mode requests by HID device and support role switch
  gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
  
  // try to become master on incoming connections
  hci_set_master_slave_policy(HCI_ROLE_MASTER);

  // Secure Simple Pairing without anything to show or type: a keyboard
  // that allows it pairs as "just works". One that insists on a passkey
  // gets HCI_EVENT_USER_PASSKEY_NOTIFICATION, logged below for now.
  gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  gap_ssp_set_auto_accept(1);
  gap_set_bondable_mode(1);

  // register for HCI events
  hci_event_callback_registration.callback = &packet_handler;
  hci_add_event_handler(&hci_event_callback_registration);

  // make discoverable to allow HID device to initiate connection
  gap_discoverable_control(1);

  // A paired classic keyboard calls us when a key is pressed. With the
  // default page scan, 11 ms in every 1.28 s, a K380 took up to a minute of
  // key presses; twice as often, twice as long and interlaced it finds us
  // within its first tries. Costs some 7 % of the radio's time.
  gap_set_page_scan_type(PAGE_SCAN_MODE_INTERLACED);
  gap_set_page_scan_activity(0x0400, 0x0024);

  // turn on!
  hci_power_control(HCI_POWER_ON);
}

void bluetooth_run(void) {
  bt_debugf("entering main loop");
  
  // this loop will never terminate
  btstack_run_loop_execute();
}

// The OSD asks from its own task; BTstack may only be called from its own.
static void bluetooth_scan_on_main(void *context) {
  (void)context;
  bt_debugf("Starting a 13 second scan, classic and Low Energy");
  gap_inquiry_start(10);   // 10 x 1.28 s
  le_scan_start(13000);
}

static btstack_context_callback_registration_t bluetooth_scan_registration = {
  .callback = &bluetooth_scan_on_main
};

bool bt_usb_started(void);   // bt_usb.c

void bluetooth_scan(void) {
  if(!bt_usb_started()) {
    bt_debugf("no Bluetooth adapter, nothing to scan with");
    return;
  }
  btstack_run_loop_execute_on_main_thread(&bluetooth_scan_registration);
}
