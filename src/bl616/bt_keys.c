/*
  bt_keys.c - Bluetooth pairings kept on the SD card

  BTstack asks a "link key db" for the key of a device that connects, and
  hands it the key of a device just paired. This one keeps them in RAM and
  writes every change to bluetooth.txt in the root of the SD card, so a
  keyboard paired once reconnects by itself after every power on -- for the
  C64 and the Atari ST alike, as the file belongs to neither core.

  One line per device, readable, and safe to edit with the companion off:

    F4:73:35:A3:EB:4C 4 df8e5ea2...   address, key type, 16 byte link key

  Deleting a line forgets that device; deleting the file forgets them all.
*/

#include <stdio.h>
#include <string.h>

#include <ff.h>

#include "btstack_config.h"
#include "btstack_util.h"
#include "classic/btstack_link_key_db.h"

#include "../debug.h"
#include "../sdc.h"

#define BT_KEYS_FILE CARD_MOUNTPOINT "/bluetooth.txt"
#define BT_KEYS_MAX  8

static struct {
  bd_addr_t addr;
  link_key_t key;
  link_key_type_t type;
} bt_keys[BT_KEYS_MAX];
static int bt_keys_num;
static bool bt_keys_loaded;

static int hex_nibble(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "F4:73:35:A3:EB:4C 4 <32 hex digits>", false on anything else
static bool bt_keys_parse(const char *s, bd_addr_t addr, link_key_t key, link_key_type_t *type) {
  for(int i = 0; i < 6; i++) {
    int h = hex_nibble(s[0]), l = hex_nibble(s[1]);
    if(h < 0 || l < 0) return false;
    addr[i] = (h << 4) | l;
    s += 2;
    if(i < 5 && *s++ != ':') return false;
  }
  if(*s++ != ' ') return false;
  if(*s < '0' || *s > '9') return false;
  *type = (link_key_type_t)(*s++ - '0');
  if(*s++ != ' ') return false;
  for(int i = 0; i < 16; i++) {
    int h = hex_nibble(s[0]), l = hex_nibble(s[1]);
    if(h < 0 || l < 0) return false;
    key[i] = (h << 4) | l;
    s += 2;
  }
  return true;
}

static void bt_keys_load(void) {
  if(bt_keys_loaded || !sdc_is_initialized()) return;
  bt_keys_num = 0;

  FIL fil;
  sdc_lock();
  FRESULT res = f_open(&fil, BT_KEYS_FILE, FA_OPEN_EXISTING | FA_READ);
  if(res == FR_OK) {
    char line[80];
    while(bt_keys_num < BT_KEYS_MAX && f_gets(line, sizeof(line), &fil)) {
      if(line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
      if(bt_keys_parse(line, bt_keys[bt_keys_num].addr, bt_keys[bt_keys_num].key,
                       &bt_keys[bt_keys_num].type))
        bt_keys_num++;
    }
    f_close(&fil);
    bt_keys_loaded = true;
  } else
    // no file yet is fine, nothing is paired; a card not ready yet is tried
    // again at the next lookup
    bt_keys_loaded = (res == FR_NO_FILE);
  sdc_unlock();

  bt_debugf("%d paired device%s in %s", bt_keys_num, bt_keys_num == 1 ? "" : "s", BT_KEYS_FILE);
}

static void bt_keys_save(void) {
  if(!sdc_is_initialized()) return;
  FIL fil;
  sdc_lock();
  if(f_open(&fil, BT_KEYS_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    f_puts("; Bluetooth pairings, kept by the FPGA companion. One device per line:\n", &fil);
    f_puts("; address, key type, link key. Delete a line to forget that device.\n", &fil);
    for(int i = 0; i < bt_keys_num; i++) {
      char line[80];
      int n = sprintf(line, "%s %u ", bd_addr_to_str(bt_keys[i].addr), (unsigned)bt_keys[i].type);
      for(int j = 0; j < 16; j++) n += sprintf(line + n, "%02x", bt_keys[i].key[j]);
      strcpy(line + n, "\n");
      f_puts(line, &fil);
    }
    if(f_close(&fil) != FR_OK)
      bt_debugf("writing %s failed", BT_KEYS_FILE);
  } else
    bt_debugf("cannot write %s, the pairing is kept until power off", BT_KEYS_FILE);
  sdc_unlock();
}

static int bt_keys_find(bd_addr_t addr) {
  for(int i = 0; i < bt_keys_num; i++)
    if(bd_addr_cmp(bt_keys[i].addr, addr) == 0) return i;
  return -1;
}

int bt_keys_count(void) {
  bt_keys_load();
  return bt_keys_num;
}

// ------------------------------------------------- the BTstack interface --
static void db_open(void) { bt_keys_load(); }
static void db_set_local_bd_addr(bd_addr_t addr) { (void)addr; }
static void db_close(void) { }

static int db_get_link_key(bd_addr_t addr, link_key_t key, link_key_type_t *type) {
  bt_keys_load();
  int i = bt_keys_find(addr);
  if(i < 0) return 0;
  memcpy(key, bt_keys[i].key, sizeof(link_key_t));
  *type = bt_keys[i].type;
  return 1;
}

static void db_put_link_key(bd_addr_t addr, link_key_t key, link_key_type_t type) {
  bt_keys_load();
  int i = bt_keys_find(addr);
  if(i < 0) {
    if(bt_keys_num == BT_KEYS_MAX) {
      // full: forget the oldest
      memmove(&bt_keys[0], &bt_keys[1], sizeof(bt_keys[0]) * (BT_KEYS_MAX - 1));
      bt_keys_num--;
    }
    i = bt_keys_num++;
  }
  memcpy(bt_keys[i].addr, addr, sizeof(bd_addr_t));
  memcpy(bt_keys[i].key, key, sizeof(link_key_t));
  bt_keys[i].type = type;
  bt_debugf("paired %s, saved", bd_addr_to_str(addr));
  bt_keys_save();
}

static void db_delete_link_key(bd_addr_t addr) {
  bt_keys_load();
  int i = bt_keys_find(addr);
  if(i < 0) return;
  memmove(&bt_keys[i], &bt_keys[i + 1], sizeof(bt_keys[0]) * (bt_keys_num - i - 1));
  bt_keys_num--;
  bt_keys_save();
}

static int db_iterator_init(btstack_link_key_iterator_t *it) {
  bt_keys_load();
  it->context = (void *)0;
  return 1;
}

static int db_iterator_get_next(btstack_link_key_iterator_t *it, bd_addr_t addr,
                                link_key_t key, link_key_type_t *type) {
  uintptr_t i = (uintptr_t)it->context;
  if(i >= (uintptr_t)bt_keys_num) return 0;
  memcpy(addr, bt_keys[i].addr, sizeof(bd_addr_t));
  memcpy(key, bt_keys[i].key, sizeof(link_key_t));
  *type = bt_keys[i].type;
  it->context = (void *)(i + 1);
  return 1;
}

static void db_iterator_done(btstack_link_key_iterator_t *it) { (void)it; }

static const btstack_link_key_db_t bt_keys_db = {
  &db_open,
  &db_set_local_bd_addr,
  &db_close,
  &db_get_link_key,
  &db_put_link_key,
  &db_delete_link_key,
  &db_iterator_init,
  &db_iterator_get_next,
  &db_iterator_done,
};

const btstack_link_key_db_t *bt_keys_instance(void) {
  return &bt_keys_db;
}
