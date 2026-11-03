/*
  bt_hid.c

  Bluetooth HID host for the BL616, on a USB Bluetooth adapter (bt_usb.c).

  This is the Raspberry Pi Pico port's rp2040/bluetooth.c, with what the
  BL616 needs changed: BTstack is started by bt_usb.c once an adapter is
  there, the OSD's scan request reaches BTstack on its own task, the
  keyboard is paired without a PIN where it allows that, and the first
  scan starts by itself when the stack is up.

  Based on:
  https://github.com/bluekitchen/btstack/tree/master/example/spp_streamer_client.c
  https://github.com/bluekitchen/btstack/tree/master/example/hid_host_demo.c
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

// the first scan, right after the stack is up: 30 x 1.28 s, about 38 s
#define FIRST_SCAN_UNITS 30

#define MAX_ATTRIBUTE_VALUE_SIZE 300

#define MAX_BT_HID_DEVICES  2
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

static void first_scan(btstack_timer_source_t *ts) {
  int paired = bt_keys_count();
  if(paired) {
    // a paired keyboard connects by itself; a scan would only be in its way
    bt_debugf("%d device%s paired, waiting for %s; Scan Bluetooth in the OSD pairs another",
              paired, paired == 1 ? "" : "s", paired == 1 ? "it" : "them");
    return;
  }
  int ret = gap_inquiry_start(FIRST_SCAN_UNITS);
  if(ret == ERROR_CODE_SUCCESS) {
    bt_debugf("scanning for %d seconds", FIRST_SCAN_UNITS * 128 / 100);
    return;
  }
  if(++first_scan_tries > 3) {
    bt_debugf("scan not started, error 0x%02x", ret);
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
  uint8_t data[16];
} key_queue[KEY_QUEUE_LEN];
static int key_queue_head, key_queue_count;
static uint32_t key_last_ms;
static btstack_timer_source_t key_timer;
static bool key_timer_armed;

static void key_apply(struct bt_hid_device_S *dev, int rep, const uint8_t *data, uint16_t len) {
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

  for(int i = 0; i < MAX_BT_HID_REPORTS; i++) {
    if(dev->rep[i].type != REPORT_TYPE_KEYBOARD) continue;
    uint8_t up[1 + 8] = { 0 };
    uint16_t len = 8;
    uint8_t *data = up + 1;
    if(dev->rep[i].report_id_present) {
      up[0] = dev->rep[i].report_id;
      data = up;
      len = 9;
    }
    hid_parse(&dev->rep[i], &dev->state[i], data, len);
  }
}

static struct bt_hid_device_S *get_dev_by_hid_cid(uint16_t cid) {
  for(int i=0;i<MAX_BT_HID_DEVICES;i++)
    if(bt_hid_device[i].dev_state != DEV_UNUSED)
      if(bt_hid_device[i].hid_host_cid == cid)
	return &bt_hid_device[i];
  
  return NULL;
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
	first_scan_tries = 0;
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
  bt_debugf("Starting a 13 second inquiry scan");
  gap_inquiry_start(10);   // 10 x 1.28 s
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
