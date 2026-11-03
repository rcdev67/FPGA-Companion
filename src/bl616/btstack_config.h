/*
  btstack_config.h - BTstack on the BL616, with a USB Bluetooth adapter

  Classic Bluetooth only for now: that is what keyboards like the Logitech
  K380 speak, and what the HID host in bt_hid.c handles. Bluetooth Low
  Energy keyboards need ENABLE_BLE and a HID-over-GATT client on top.
*/

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// port: FreeRTOS, time from its tick, memory from its heap
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_FREERTOS_TASK_NOTIFICATIONS
#define HAVE_MALLOC

// features
#define ENABLE_CLASSIC
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP

// An ACL packet from the adapter has to fit CherryUSB's receive buffer,
// CONFIG_USBHOST_BLUETOOTH_RX_SIZE in usb_config.h, with its header and
// type byte; one to it has to fit CONFIG_USBHOST_BLUETOOTH_TX_SIZE.
#define HCI_ACL_PAYLOAD_SIZE (1021 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 4
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#endif // BTSTACK_CONFIG_H
