/*
  btstack_config.h - BTstack on the BL616, with a USB Bluetooth adapter

  Classic Bluetooth for keyboards like the Logitech K380, and Bluetooth Low
  Energy for those that speak only that -- the ROG Azoth X in its Bluetooth
  mode, game controllers like the Xbox Series one. bt_hid.c is the HID host
  for both: classic HID, and HID over GATT through BTstack's hids_host.
*/

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// port: FreeRTOS, time from its tick, memory from its heap
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_FREERTOS_TASK_NOTIFICATIONS
#define HAVE_MALLOC

// features
#define ENABLE_CLASSIC
#define ENABLE_BLE
#define ENABLE_LE_CENTRAL
// LE Secure Connections where the device offers them, legacy pairing for
// the others. The P-256 maths is done here, by micro-ecc: asked for its key,
// the RTL8761BU never answered, and took every connection down with it.
#define ENABLE_LE_SECURE_CONNECTIONS
#define ENABLE_MICRO_ECC_P256
#define ENABLE_LOG_ERROR
// #define ENABLE_LOG_INFO    // BTstack's own account of every step
#define ENABLE_PRINTF_HEXDUMP

// The SDK's WiFi library has an sm_init() of its own, its station
// management; BTstack's Security Manager gets another name.
#define sm_init btstack_sm_init

// LE bondings, in the TLV store on the SD card (bt_tlv.c)
#define NVM_NUM_DEVICE_DB_ENTRIES 8

// An ACL packet from the adapter has to fit CherryUSB's receive buffer,
// CONFIG_USBHOST_BLUETOOTH_RX_SIZE in usb_config.h, with its header and
// type byte; one to it has to fit CONFIG_USBHOST_BLUETOOTH_TX_SIZE.
#define HCI_ACL_PAYLOAD_SIZE (1021 + 4)
// room before a packet: BTstack needs 6 for long GATT reads; 8 keeps the
// alignment
#define HCI_INCOMING_PRE_BUFFER_SIZE 8
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#endif // BTSTACK_CONFIG_H
