/*
  bt_usb.c - Bluetooth through a USB adapter on the BL616

  The onboard BL616 has a radio, but no antenna on the Tang Nano 20K. A
  plain USB Bluetooth adapter on the hub does the radio instead: it speaks
  HCI over USB, and CherryUSB's Bluetooth class carries that. When the class
  finds an adapter it calls usbh_bluetooth_run(); receiving is left to the
  application, which has to start the class's two receive threads.

  Stage 1, this file: talk to the adapter. It resets the controller, asks
  for its version and its address, and logs every packet that comes back,
  so that the whole way from the companion to the radio chip can be seen on
  the console. A keyboard needs a Bluetooth host stack on top of this.
*/

#include <stdio.h>
#include <string.h>

#include "usbh_core.h"
#include "usbh_bluetooth.h"

#include "../debug.h"

static const char *hci_type(uint8_t t) {
  switch(t) {
  case USB_BLUETOOTH_HCI_EVT: return "event";
  case USB_BLUETOOTH_HCI_ACL: return "acl";
  default:                    return "other";
  }
}

// the answer to a command: event 0x0e "command complete"
static void hci_command_complete(const uint8_t *p, uint32_t len) {
  if(len < 7) return;
  uint16_t opcode = p[4] | (p[5] << 8);
  uint8_t status = p[6];

  if(opcode == 0x0c03)
    debugf("BT  reset: status %02x", status);
  else if(opcode == 0x1001 && len >= 15)
    // HCI and LMP version, manufacturer, LMP subversion: what chip it is
    debugf("BT  version: status %02x, HCI %u rev %04x, LMP %u sub %04x, manufacturer %04x",
           status, p[7], p[8] | (p[9] << 8), p[10], p[13] | (p[14] << 8),
           p[11] | (p[12] << 8));
  else if(opcode == 0x1009 && len >= 13)
    debugf("BT  address: status %02x, %02X:%02X:%02X:%02X:%02X:%02X",
           status, p[12], p[11], p[10], p[9], p[8], p[7]);
  else
    debugf("BT  command %04x complete, status %02x", opcode, status);
}

// called by the class's receive threads, data[0] is the packet type
void usbh_bluetooth_hci_read_callback(uint8_t *data, uint32_t len) {
  if(len >= 2 && data[0] == USB_BLUETOOTH_HCI_EVT && data[1] == 0x0e) {
    hci_command_complete(data, len);
    return;
  }

  char hex[3 * 16 + 1];
  uint32_t n = len < 16 ? len : 16;
  for(uint32_t i = 0; i < n; i++)
    sprintf(hex + 3 * i, "%02x ", data[i]);
  hex[3 * n] = '\0';
  debugf("BT  %s, %lu bytes: %s%s", hci_type(data[0]), (unsigned long)len, hex,
         len > n ? "..." : "");
}

static int hci_command(const char *what, uint16_t opcode) {
  uint8_t cmd[3] = { opcode & 0xff, opcode >> 8, 0 };
  int ret = usbh_bluetooth_hci_write(USB_BLUETOOTH_HCI_CMD, cmd, sizeof(cmd));
  if(ret < 0) debugf("BT  %s: write failed, %d", what, ret);
  return ret;
}

static void bt_probe_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV) {
  (void)CONFIG_USB_OSAL_THREAD_GET_ARGV;

  // let the enumeration finish before the first control transfer
  usb_osal_msleep(200);
  hci_command("reset", 0x0c03);
  usb_osal_msleep(500);
  hci_command("read version", 0x1001);
  usb_osal_msleep(200);
  hci_command("read address", 0x1009);

  usb_osal_thread_delete(NULL);
}

void usbh_bluetooth_run(struct usbh_bluetooth *bluetooth_class) {
  struct usbh_hubport *hport = bluetooth_class->hport;
  debugf("BT  adapter %04x:%04x found", hport->device_desc.idVendor,
         hport->device_desc.idProduct);

  usb_osal_thread_create("bt_evt", 2048, CONFIG_USBHOST_PSC_PRIO,
                         usbh_bluetooth_hci_evt_rx_thread, NULL);
  usb_osal_thread_create("bt_acl", 2048, CONFIG_USBHOST_PSC_PRIO,
                         usbh_bluetooth_hci_acl_rx_thread, NULL);
  usb_osal_thread_create("bt_probe", 2048, CONFIG_USBHOST_PSC_PRIO - 1,
                         bt_probe_thread, NULL);
}

void usbh_bluetooth_stop(struct usbh_bluetooth *bluetooth_class) {
  (void)bluetooth_class;
  debugf("BT  adapter gone");
}
