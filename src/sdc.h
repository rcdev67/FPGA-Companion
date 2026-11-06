#ifndef SDC_H
#define SDC_H

#include <stdbool.h>
#include "config.h"
#include <ff.h>

// fatfs mounts the card under /sd
#define CARD_MOUNTPOINT "/sd"

// the file selector slot of "From USB" in the companion's menu, after the
// drives, the images and the cores, see menu.c
#define SDC_COPY_SLOT (MAX_DRIVES+MAX_IMAGES+MAX_CORES)

typedef struct sdc_dir {
  char *name;
  unsigned long len;
  int is_dir;
  struct sdc_dir *next;
} sdc_dir_entry_t;

int sdc_init(void);
int sdc_image_open(int drive, char *name);
sdc_dir_entry_t *sdc_readdir(int drive, char *name, const char *exts);
int sdc_handle_event(void);
int  sdc_get_last_error(void);   // 0 = none, see sdc.c
bool sdc_is_initialized(void);
void sdc_lock(void);
void sdc_unlock(void);
char *sdc_get_image_name(int drive);
char *sdc_get_cwd(int drive);
void sdc_set_cwd(int drive, char *path);
void sdc_set_default(int drive, const char *name);
void sdc_mount_defaults(void);
bool sdc_image_upload_in_progress(void);
extern uint32_t sdc_sector_requests;   // sectors the core has asked for
// images the core has written, for a backup on the server, see sdc.c:
// the next one quiet for quiet_ms (its path, a stamp; -1 if none), and how
// it went: 1 backed up, 0 failed (it stays to go), -1 never
int sdc_save_next(char *path, size_t size, uint32_t quiet_ms, uint32_t *stamp);
void sdc_save_done(int slot, uint32_t stamp, int how);
bool sdc_check_for_pending_image_uploads(void);

#endif // SDC_H
