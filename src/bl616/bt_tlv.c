/*
  bt_tlv.c - BTstack's tag-length-value store, on the SD card

  BTstack keeps the bondings of Bluetooth Low Energy devices through a TLV
  store: its le_device_db writes one tag per bonded device, and bt_hid.c one
  more with the LE HID devices to reconnect to. This one holds the tags in
  RAM and writes all of them to bluetooth_le.bin in the root of the SD card
  on every change. Delete that file to forget every LE pairing; the classic
  ones are in bluetooth.txt, see bt_keys.c.

  File: "BTLV", then per tag its number (4 bytes), length (2), data.
*/

#include <stdio.h>
#include <string.h>

#include <ff.h>

#include "btstack_config.h"
#include "btstack_tlv.h"

#include "../debug.h"
#include "../sdc.h"

#define BT_TLV_FILE  CARD_MOUNTPOINT "/bluetooth_le.bin"
#define BT_TLV_MAX   20
#define BT_TLV_DATA  96

static struct {
  uint32_t tag;
  uint16_t len;
  uint8_t data[BT_TLV_DATA];
} bt_tlv[BT_TLV_MAX];
static int bt_tlv_num;
static bool bt_tlv_loaded;

static void bt_tlv_load(void) {
  if(bt_tlv_loaded || !sdc_is_initialized()) return;
  bt_tlv_num = 0;

  FIL fil;
  sdc_lock();
  FRESULT res = f_open(&fil, BT_TLV_FILE, FA_OPEN_EXISTING | FA_READ);
  if(res == FR_OK) {
    UINT n;
    char magic[4];
    if(f_read(&fil, magic, 4, &n) == FR_OK && n == 4 && memcmp(magic, "BTLV", 4) == 0) {
      while(bt_tlv_num < BT_TLV_MAX) {
        uint8_t head[6];
        if(f_read(&fil, head, 6, &n) != FR_OK || n != 6) break;
        uint32_t tag = head[0] | (head[1] << 8) | (head[2] << 16) | ((uint32_t)head[3] << 24);
        uint16_t len = head[4] | (head[5] << 8);
        if(len > BT_TLV_DATA) break;
        if(f_read(&fil, bt_tlv[bt_tlv_num].data, len, &n) != FR_OK || n != len) break;
        bt_tlv[bt_tlv_num].tag = tag;
        bt_tlv[bt_tlv_num].len = len;
        bt_tlv_num++;
      }
    }
    f_close(&fil);
    bt_tlv_loaded = true;
  } else
    // no file yet: nothing bonded; a card not ready: tried again next time
    bt_tlv_loaded = (res == FR_NO_FILE);
  sdc_unlock();
}

static void bt_tlv_save(void) {
  if(!sdc_is_initialized()) return;
  FIL fil;
  sdc_lock();
  if(f_open(&fil, BT_TLV_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    UINT n;
    f_write(&fil, "BTLV", 4, &n);
    for(int i = 0; i < bt_tlv_num; i++) {
      uint8_t head[6] = {
        bt_tlv[i].tag & 0xff, (bt_tlv[i].tag >> 8) & 0xff,
        (bt_tlv[i].tag >> 16) & 0xff, bt_tlv[i].tag >> 24,
        bt_tlv[i].len & 0xff, bt_tlv[i].len >> 8 };
      f_write(&fil, head, 6, &n);
      f_write(&fil, bt_tlv[i].data, bt_tlv[i].len, &n);
    }
    if(f_close(&fil) != FR_OK) bt_debugf("writing %s failed", BT_TLV_FILE);
  } else
    bt_debugf("cannot write %s, LE pairings are kept until power off", BT_TLV_FILE);
  sdc_unlock();
}

static int bt_tlv_find(uint32_t tag) {
  for(int i = 0; i < bt_tlv_num; i++)
    if(bt_tlv[i].tag == tag) return i;
  return -1;
}

static int tlv_get_tag(void *context, uint32_t tag, uint8_t *buffer, uint32_t buffer_size) {
  (void)context;
  bt_tlv_load();
  int i = bt_tlv_find(tag);
  if(i < 0) return 0;
  uint32_t n = bt_tlv[i].len < buffer_size ? bt_tlv[i].len : buffer_size;
  memcpy(buffer, bt_tlv[i].data, n);
  return bt_tlv[i].len;
}

static int tlv_store_tag(void *context, uint32_t tag, const uint8_t *data, uint32_t data_size) {
  (void)context;
  bt_tlv_load();
  if(data_size > BT_TLV_DATA) {
    bt_debugf("TLV tag %08lx too long, %lu bytes", (unsigned long)tag, (unsigned long)data_size);
    return 1;
  }
  int i = bt_tlv_find(tag);
  if(i < 0) {
    if(bt_tlv_num == BT_TLV_MAX) {
      bt_debugf("TLV store full");
      return 1;
    }
    i = bt_tlv_num++;
  } else if(bt_tlv[i].len == data_size && memcmp(bt_tlv[i].data, data, data_size) == 0)
    return 0;   // unchanged, no write to the card
  bt_tlv[i].tag = tag;
  bt_tlv[i].len = data_size;
  memcpy(bt_tlv[i].data, data, data_size);
  bt_tlv_save();
  return 0;
}

static void tlv_delete_tag(void *context, uint32_t tag) {
  (void)context;
  bt_tlv_load();
  int i = bt_tlv_find(tag);
  if(i < 0) return;
  memmove(&bt_tlv[i], &bt_tlv[i + 1], sizeof(bt_tlv[0]) * (bt_tlv_num - i - 1));
  bt_tlv_num--;
  bt_tlv_save();
}

static const btstack_tlv_t bt_tlv_impl = {
  &tlv_get_tag,
  &tlv_store_tag,
  &tlv_delete_tag,
};

const btstack_tlv_t *bt_tlv_instance(void) {
  return &bt_tlv_impl;
}
