/*
  bt_rtk.c - load the firmware patch into a Realtek USB Bluetooth adapter

  A Realtek adapter starts on the bare firmware in its ROM. That answers
  commands but its radio finds nothing -- seen on the board with an RTL8761B:
  a full inquiry, no single device. Windows and Linux load a patch from
  Realtek into the chip's RAM every time it is plugged in; so does this,
  before BTstack takes the adapter over. The patch lives in the companion's
  flash (firmware/rtl_bt, Realtek's file from linux-firmware, unmodified, see
  its licence there), so nothing has to be on the SD card and no network is
  needed.

  This follows Linux's drivers/bluetooth/btrtl.c for the "epatch v1" files,
  and streams the patch from flash piece by piece instead of building it in
  RAM: BTstack's own Realtek support would hold the 44 KB file twice, and the
  BL616 has about 50 KB to spare.

  1. read the local version: LMP subversion 0x8761 means the ROM is running,
     anything else that the patch is already in
  2. read the ROM version, a vendor command
  3. pick the patch for it: chip id = ROM version + 1. Its last four bytes
     are replaced by the file's firmware version, and the config file is
     appended
  4. send that in pieces of 252 bytes, vendor command 0xfc20
*/

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "usbh_core.h"
#include "usbh_bluetooth.h"

#include "../debug.h"

// from the build: firmware/rtl_bt/*.bin as C arrays (CMakeLists.txt)
extern const unsigned char rtl8761bu_fw[];
extern const unsigned int rtl8761bu_fw_len;
extern const unsigned char rtl8761bu_config[];
extern const unsigned int rtl8761bu_config_len;

#define RTK_FRAG_LEN 252

static SemaphoreHandle_t rtk_done;
static uint16_t rtk_wait_opcode;
static uint8_t rtk_reply[32];      // the command complete parameters, from the status on
static volatile bool rtk_active;

bool bt_rtk_active(void) {
  return rtk_active;
}

// while active, every event from the adapter comes here instead of BTstack
void bt_rtk_event(const uint8_t *data, uint32_t len) {
  // data[0] packet type, then event code 0x0e, length, num packets, opcode
  if(len < 7 || data[1] != 0x0e) return;
  uint16_t opcode = data[4] | (data[5] << 8);
  if(opcode != rtk_wait_opcode) return;
  uint32_t n = len - 6;
  if(n > sizeof(rtk_reply)) n = sizeof(rtk_reply);
  memcpy(rtk_reply, data + 6, n);
  xSemaphoreGive(rtk_done);
}

// send a command and wait for its command complete; returns its status
static int rtk_command(uint16_t opcode, const uint8_t *params, uint8_t plen) {
  static uint8_t cmd[3 + 1 + RTK_FRAG_LEN];
  cmd[0] = opcode & 0xff;
  cmd[1] = opcode >> 8;
  cmd[2] = plen;
  if(plen) memcpy(cmd + 3, params, plen);

  rtk_wait_opcode = opcode;
  xSemaphoreTake(rtk_done, 0);
  if(usbh_bluetooth_hci_write(USB_BLUETOOTH_HCI_CMD, cmd, 3 + plen) < 0)
    return -1;
  if(xSemaphoreTake(rtk_done, pdMS_TO_TICKS(2000)) != pdTRUE)
    return -2;
  return rtk_reply[0];
}

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint16_t rtk_lmp_subversion(void) {
  if(rtk_command(0x1001, NULL, 0) != 0) return 0;
  // status, HCI version, HCI revision (2), LMP version, manufacturer (2), LMP subversion (2)
  return le16(rtk_reply + 7);
}

// the byte at position pos of the stream to send: patch, firmware version
// in its last four bytes, config appended
static const uint8_t *rtk_patch;
static uint32_t rtk_patch_len;
static uint8_t rtk_fw_version[4];

static uint8_t rtk_stream_byte(uint32_t pos) {
  if(pos < rtk_patch_len - 4) return rtk_patch[pos];
  if(pos < rtk_patch_len)     return rtk_fw_version[pos - (rtk_patch_len - 4)];
  return rtl8761bu_config[pos - rtk_patch_len];
}

static bool rtk_download(void) {
  const uint8_t *fw = rtl8761bu_fw;
  if(rtl8761bu_fw_len < 14 || memcmp(fw, "Realtech", 8) != 0) {
    bt_debugf("RTK: firmware file is not an epatch v1");
    return false;
  }
  if(le32(fw + rtl8761bu_fw_len - 4) != 0x77fd0451) {
    bt_debugf("RTK: firmware file has no extension signature");
    return false;
  }

  if(rtk_command(0xfc6d, NULL, 0) != 0) {
    bt_debugf("RTK: reading the ROM version failed");
    return false;
  }
  uint8_t rom_version = rtk_reply[1];

  memcpy(rtk_fw_version, fw + 8, 4);
  uint16_t num = le16(fw + 12);
  const uint8_t *chip_ids = fw + 14;
  const uint8_t *lengths  = chip_ids + 2 * num;
  const uint8_t *offsets  = lengths + 2 * num;

  rtk_patch = NULL;
  for(uint16_t i = 0; i < num; i++) {
    if(le16(chip_ids + 2 * i) == rom_version + 1) {
      rtk_patch_len = le16(lengths + 2 * i);
      uint32_t offset = le32(offsets + 4 * i);
      if(offset + rtk_patch_len > rtl8761bu_fw_len || rtk_patch_len < 4) break;
      rtk_patch = fw + offset;
    }
  }
  if(!rtk_patch) {
    bt_debugf("RTK: no patch for ROM version %u in the firmware file", rom_version);
    return false;
  }

  uint32_t total = rtk_patch_len + rtl8761bu_config_len;
  uint32_t frags = total / RTK_FRAG_LEN + 1;
  bt_debugf("RTK: ROM version %u, sending a %lu byte patch in %lu pieces", rom_version,
            (unsigned long)total, (unsigned long)frags);

  static uint8_t frag[1 + RTK_FRAG_LEN];
  uint32_t pos = 0;
  for(uint32_t i = 0; i < frags; i++) {
    uint32_t len = RTK_FRAG_LEN;
    uint8_t index = i > 0x7f ? (i & 0x7f) + 1 : i;
    if(i == frags - 1) {
      index |= 0x80;   // the last piece
      len = total % RTK_FRAG_LEN;
    }
    frag[0] = index;
    for(uint32_t j = 0; j < len; j++)
      frag[1 + j] = rtk_stream_byte(pos + j);
    int status = rtk_command(0xfc20, frag, 1 + len);
    if(status != 0) {
      bt_debugf("RTK: piece %lu of %lu refused, status %d", (unsigned long)i + 1,
                (unsigned long)frags, status);
      return false;
    }
    pos += RTK_FRAG_LEN;
  }
  return true;
}

// Called with the adapter just plugged in and BTstack not yet on it. The
// adapter's events reach bt_rtk_event() meanwhile.
void bt_rtk_setup(uint16_t vid, uint16_t pid) {
  if(vid != 0x0bda || pid != 0x8771) return;   // RTL8761BU, the one patch here

  if(!rtk_done) rtk_done = xSemaphoreCreateBinary();
  rtk_active = true;

  rtk_command(0x0c03, NULL, 0);   // reset
  uint16_t sub = rtk_lmp_subversion();
  if(sub != 0x8761) {
    bt_debugf("RTK: LMP subversion %04x, the patch is in already", sub);
  } else if(rtk_download()) {
    bt_debugf("RTK: patch loaded, LMP subversion now %04x", rtk_lmp_subversion());
  } else
    bt_debugf("RTK: running without the patch, the radio may find nothing");

  rtk_active = false;
}
