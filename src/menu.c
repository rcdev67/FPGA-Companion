/*
  menu.c - MiSTeryNano menu based in u8g2

  This version includes the old static MiSTeryNano type of menu
  as well as the new config driven one.

*/
  
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ff.h>
#include <diskio.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#else
#include <FreeRTOS.h>
#include <timers.h>
#include <task.h>
#include <queue.h>
#endif

#include "sdc.h"
#include "osd.h"
#include "inifile.h"
#include "menu.h"
#include "sysctrl.h"
#include "net.h"
#include "debug.h"
#include "mcu_hw.h"

#ifdef ENABLE_BLUETOOTH
#include "bluetooth.h"
#endif

// this is the u8g2_font_helvR08_te with any trailing
// spaces removed
#include "font_helvR08_te.c"

#if defined(ENABLE_JTAG) && !defined(DIRECT_SDC_SUPPORTED)
// "Change core", further down: a core from the USB stick into the flash
#define CORE_SWITCH_SLOT (MAX_DRIVES+MAX_IMAGES+1)
static void core_switch(const char *dir, const char *file);
static bool core_switch_result(void);
#endif

// some constants for arrangement
// The OSD (currently) is 64 pixel high. To allow for a proper
// box around a text line, it needs to be 12 pixels high. A total
// of five lines is 5*12 = 60 + title seperation line
#define MENU_LINE_Y      13   // y pos of seperator line
#define MENU_ENTRY_H     12   // height of regular menu entries
#define MENU_ENTRY_BASE   9   // font baseline offset


#define MENU_FORM_FSEL           -1

#define MENU_ENTRY_INDEX_ID       0
#define MENU_ENTRY_INDEX_LABEL    1
#define MENU_ENTRY_INDEX_FORM     2
#define MENU_ENTRY_INDEX_OPTIONS  2
#define MENU_ENTRY_INDEX_VARIABLE 3

// menu types
#define MENU_TYPE_MENU          0
#define MENU_TYPE_FILESELECTOR  1
#define MENU_TYPE_CUSTOM        2  // only internally used

typedef struct config_custom_S {
  char *label;

  // Pointer to function the returns the number of active entries in
  // this custom dialog. This is the number of "lines" that can be
  // selected with the up and down keys
  int (*length)(void); 
  
  // Pointer to function that draws the custom contents
  void (*draw)(void);

  // Pointer to function called when an entry (1..length) is selected.
  // May be NULL for dialogs that only display something
  void (*select)(int);

} config_custom_t;

/* new menu state */
typedef struct menu_state {
  int type;
  int selected; 
  int scroll;
  union {
    const config_menu_t *menu;  
    const char *label;             // used for file/image selector, only
    const config_custom_t *custom;  
  };
  
  // file selector related
  sdc_dir_entry_t *dir;

  struct menu_state *prev;
} menu_state_t;

typedef struct {
  char index;
  char **ext;
  config_action_t *action;  // action to be run afterwards

  // string and icon to be used if nothing is selected
  // by default this is "[X] No Disk"
  char *none_str;
  unsigned char *none_icn;
} fsel_state_t;

// menu state is a stack since as the menu is hierarchical
static menu_state_t *menu_state = NULL;
static fsel_state_t fsel_state;

/* =========== handling of variables ============= */
static menu_variable_t *variables = NULL;

menu_variable_t *menu_get_variables(void) {
  return variables;
}

// Also used outside the menu, e.g. by the mouse code to pick up a scale set
// in the ini file, so this is not static anymore.
int menu_variable_get(char id) {
  menu_variable_t *v = variables;

  while(v) {
    if(v->id == id) return v->value;
    v = v->next;
  }
  return 0;  
}

static bool menu_variable_exists(char id) {
  menu_variable_t *v = variables;

  while(v) {
    if(v->id == id) return true;
    v = v->next;
  }
  return false;  
}

static void menu_variable_set(char id, int value) {
  menu_variable_t *v = variables;
  
  while(v) {
    if(v->id == id) {
      if(v->value != value) {
	v->value = value;
	// also set this in the core
	sys_set_val(id, value);
      }
    }
    v = v->next;
  }
}

static void menu_setup_variable(char id, int value) {
  // menu_debugf("setup variable '%c' = %d", id, value);

  // allocate new entry
  menu_variable_t *variable = pvPortMalloc(sizeof(menu_variable_t));
  variable->id = id;
  variable->value = value;
  variable->next = NULL;

  if(!variables)
    variables = variable;
  else {
    menu_variable_t *v = variables;
    while(v->next) v = v->next;
    v->next = variable;
  }

  // also set the value in the core
  sys_set_val(id, value);
}

static void menu_setup_menu_variables(const config_menu_t *menu) {
  config_menu_entry_t *me = menu->entries;
  while(me) {
    if(me->type == CONFIG_MENU_ENTRY_MENU)
      menu_setup_menu_variables(me->menu);
    
    if(me->type == CONFIG_MENU_ENTRY_LIST) {
      // setup variable ...
      menu_setup_variable(me->list->id, me->list->def);
    }
    
    if(me->type == CONFIG_MENU_ENTRY_TOGGLE) {
      // setup variable ...
      menu_setup_variable(me->toggle->id, me->toggle->def);
    }
    
    if(me->type == CONFIG_MENU_ENTRY_RANGE) {
      // setup variable ...
      menu_setup_variable(me->range->id, me->range->def);
    }
    
    me = me->next;
  }
}

static void menu_setup_variables(void) {
  // variables occur in two places:
  // in the set command used in actions
  // in menu items (currently only in lists as buttons use actions)

  // Only the variables from menu items are actually stored permanently.
  // Action variables are sent into the core and processed there.
  
  // search through menu tree for lists
  menu_setup_menu_variables(cfg->menu);  
}


void menu_set_value(unsigned char id, int8_t value) {
  // this is called when reading the ini file
  // We allow values in the ini file which are actually not in the
  // menu at all. This can be used for "static" changes which are made
  // once by manually manipulating the ini file e.g. with a text
  // editor

  if(!menu_variable_exists(id)) {
    // previous companion versions were writing the action related variables
    // into the ini file as well. This mainly affected the 'R' variable often
    // used for reset. We ignore this particular variable by now as this was
    // not intentinal. This should be removed one day.
    if(id == 'R') {
      menu_debugf("suppressing 'R' reset variable");
      return;
    }
    menu_setup_variable(id, value);
  } else
    menu_variable_set(id, value);
}

// various 8x8 icons
static const unsigned char icn_right_bits[]  = { 0x00,0x04,0x0c,0x1c,0x3c,0x1c,0x0c,0x04 };
static const unsigned char icn_left_bits[]   = { 0x00,0x20,0x30,0x38,0x3c,0x38,0x30,0x20 };
static const unsigned char icn_floppy_bits[] = { 0xff,0x81,0x83,0x81,0xbd,0xad,0x6d,0x3f };
static const unsigned char icn_empty_bits[] =  { 0xc3,0xe7,0x7e,0x3c,0x3c,0x7e,0xe7,0xc3 };
static const unsigned char icn_on_bits[] =     { 0x3c,0x42,0x99,0xbd,0xbd,0x99,0x42,0x3c };
static const unsigned char icn_off_bits[] =    { 0x3c,0x42,0x81,0x81,0x81,0x81,0x42,0x3c };

void u8g2_DrawStrT(u8g2_t *u8g2, u8g2_uint_t x, u8g2_uint_t y, const char *s) {
  // get length of string
  int n = 0;
  while(s[n] && s[n] != ';' && s[n] != ',' && s[n] != '|') n++;

  // create a 0 terminated copy in the stack
  char buffer[n+1];
  strncpy(buffer, s, n);
  buffer[n] = '\0';
  
  u8g2_DrawStr(u8g2, x, y, buffer);
}

#define FS_ICON_WIDTH 10
static int fs_scroll_cur = -1;

static int dir_len(sdc_dir_entry_t *d) {
  int len = 0;
  while(d) {
    d = d->next;
    len++;
  }
  return len;
}

static sdc_dir_entry_t *dir_entry(sdc_dir_entry_t *d, int n) {
  while(n && d) {
    d = d->next;
    n--;
  }
  return d;
}

static void menu_fs_scroll_entry(void) {
  // no scrolling
  if(fs_scroll_cur < 0) return;
  
  // don't scroll anything else
  if(menu_state->type != MENU_TYPE_FILESELECTOR) return;
  
  int row = menu_state->selected - 1;
  int y =  MENU_LINE_Y + MENU_ENTRY_H * (row-menu_state->scroll+1);
  int width = u8g2_GetDisplayWidth(&u8g2);

  // jump to the row'th entry in the dir listing
  int swid = u8g2_GetStrWidth(&u8g2, dir_entry(menu_state->dir, row)->name) + 1;

  // fill the area where the scrolling entry would show
  u8g2_SetClipWindow(&u8g2, FS_ICON_WIDTH, y-MENU_ENTRY_BASE, width, y+MENU_ENTRY_H-MENU_ENTRY_BASE);  
  u8g2_DrawBox(&u8g2, FS_ICON_WIDTH, y-MENU_ENTRY_BASE, width-FS_ICON_WIDTH, MENU_ENTRY_H);
  u8g2_SetDrawColor(&u8g2, 0);

  int scroll = fs_scroll_cur++ - 25;   // 25 means 1 sec delay
  if(fs_scroll_cur > swid-width+FS_ICON_WIDTH+50) fs_scroll_cur = 0;
  if(scroll < 0) scroll = 0;
  if(scroll > swid-width+FS_ICON_WIDTH) scroll = swid-width+FS_ICON_WIDTH;
  
  u8g2_DrawStr(&u8g2, FS_ICON_WIDTH-scroll, y, dir_entry(menu_state->dir, row)->name);
  
  // restore previous draw mode
  u8g2_SetDrawColor(&u8g2, 1);
  u8g2_SetMaxClipWindow(&u8g2);
  u8g2_SendBuffer(&u8g2);
}

void menu_timer_enable(bool on);

static void menu_fs_draw_entry(int row, sdc_dir_entry_t *entry) {      
  static const unsigned char folder_icon[] = { 0x70,0x8e,0xff,0x81,0x81,0x81,0x81,0x7e };
  static const unsigned char up_icon[] =     { 0x04,0x0e,0x1f,0x0e,0xfe,0xfe,0xfe,0x00 };
  static const unsigned char empty_icon[] =  { 0xc3,0xe7,0x7e,0x3c,0x3c,0x7e,0xe7,0xc3 };

  int y =  MENU_LINE_Y + MENU_ENTRY_H * (row+1);
  int width = u8g2_GetDisplayWidth(&u8g2);
  
  if(entry->name[0] == '/') {
    if(fsel_state.none_str) u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, fsel_state.none_str);
    else                    u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, "No Disk");
  } else {
    char str[strlen(entry->name)+1];
    strcpy(str, entry->name);
  
    // properly ellipsize string
    int dotlen = u8g2_GetStrWidth(&u8g2, "...");
    if(u8g2_GetStrWidth(&u8g2, str) > width-FS_ICON_WIDTH) {
      // the entry is too long to fit the menu.    
      // check if this is the selected file and then enable scrolling
      if(row == menu_state->selected - menu_state->scroll - 1)
	fs_scroll_cur = 0;
      
      // enable timer, to allow animations
      menu_timer_enable(true);
      
      while(u8g2_GetStrWidth(&u8g2, str) > width-FS_ICON_WIDTH-dotlen) str[strlen(str)-1] = 0;
      if(strlen(str) < sizeof(str)-4) strcat(str, "...");
    }
  
    u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, str);
  }
  
  // draw folder icon in front of directories
  if(entry->is_dir)
    u8g2_DrawXBM(&u8g2, 1, y-8, 8, 8,
		 (entry->name[0] == '/')?(fsel_state.none_icn?fsel_state.none_icn:empty_icon):
		 strcmp(entry->name, "..")?folder_icon:
		 up_icon);

  if(menu_state->selected == row+menu_state->scroll+1)
    u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
}

static void menu_push(void) {
  menu_debugf("menu_push()");

  // allocate a new menu entry
  menu_state_t *state = pvPortMalloc(sizeof(menu_state_t));

  // insert the new entry at begin of chain
  state->prev = menu_state;
  menu_state = state;
}

static int menu_len(const config_menu_t *menu) {
  int entries = 0;
  config_menu_entry_t *me = menu->entries;
  while(me) {
    entries++;
    me = me->next;
  }
  return entries;
}

static int menu_count_entries(void) {
  int entries = 0;

  if(menu_state->type == MENU_TYPE_MENU)
    entries = menu_len(menu_state->menu);
  else if(menu_state->type == MENU_TYPE_FILESELECTOR)
    entries = dir_len(menu_state->dir);
  else if(menu_state->type == MENU_TYPE_CUSTOM)
    if(menu_state->custom->length)
      entries = menu_state->custom->length();
    
  return entries+1;  // title is also an entry
}

static bool menu_is_root(const config_menu_t *menu) {
  return menu == cfg->menu;
}

static int menu_entry_is_usable(void) {
  // not root menu? Then all entries are usable. Root menu
  // title is only usable if no hid keyboard or gamepad is present
  if(!menu_is_root(menu_state->menu) || !mcu_hw_hid_present()) return 1;

  // in root menu only the title is unusable
  return menu_state->selected != 0;
}

static config_menu_entry_t *menu_get_selected_entry(void) {
  // first check if there's a menu at all  
  if(menu_state->type != MENU_TYPE_MENU)
    return NULL;
  
  config_menu_entry_t *entry = menu_state->menu->entries;
  for(int i=0;i<menu_state->selected - 1;i++) entry=entry->next;
  return entry;
}
  
static void menu_entry_go(int step) {

  // the up/down events change the a range value in edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    int value = menu_variable_get(entry->range->id);

    value -= step;
    if(value > entry->range->max) value = entry->range->max;
    if(value < entry->range->min) value = entry->range->min;          

    menu_variable_set(entry->range->id, value);
    return;
  }							  
  
  int entries = menu_count_entries();
  do {
    menu_state->selected += step;
    
    // single step wraps top/bottom, paging does not
    if(abs(step) == 1) {    
      if(menu_state->selected < 0) menu_state->selected = entries + menu_state->selected;
      if(menu_state->selected >= entries) menu_state->selected = menu_state->selected - entries;
    } else {
      // limit to top/bottom. Afterwards step 1 in opposite
      // direction to skip unusable entries
      if(menu_state->selected < 1) { menu_state->selected = 1; step = 1; }	
      if(menu_state->selected >= entries) { menu_state->selected = entries - 1; step = -1; }
    }

    // scrolling needed?
    if(step > 0) {
      if(entries <= 5)                            menu_state->scroll = 0;
      else {
	if(menu_state->selected <= 3)             menu_state->scroll = 0;
	else if(menu_state->selected < entries-2) menu_state->scroll = menu_state->selected - 3;
	else                                      menu_state->scroll = entries-5;
      }
    }

    if(step < 0) {
      if(entries <= 5)                            menu_state->scroll = 0;
      else {
	if(menu_state->selected <= 2)             menu_state->scroll = 0;
	else if(menu_state->selected < entries-3) menu_state->scroll = menu_state->selected - 2;
	else                                      menu_state->scroll = entries-5;
      }
    }    
  } while(!menu_entry_is_usable());
}

static void menu_draw_title(const char *s, bool arrow, bool selected) {
  int x = 1;

  // draw left arrow for submenus
  if(arrow) {
    u8g2_DrawXBM(&u8g2, 0, 1, 8, 8, icn_left_bits);    
    x = 8;
  } else if(!mcu_hw_hid_present()) {
    // without keyboard or joystick connected, the menu runs in
    // "single button" mode
  
    // no arrow? Then this is the root menu. In that case
    // a cross is drawn in "single button mode"
    u8g2_DrawXBM(&u8g2, 0, 1, 8, 8, icn_empty_bits);    
    x = 10;
  }
  
  // draw title in bold and seperator line
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  u8g2_DrawStr(&u8g2, x, MENU_ENTRY_BASE, s);
  u8g2_DrawHLine(&u8g2, 0, MENU_LINE_Y, u8g2_GetDisplayWidth(&u8g2));

  if(selected)
    u8g2_DrawButtonFrame(&u8g2, 0, MENU_ENTRY_BASE, U8G2_BTN_INV,
	 u8g2_GetDisplayWidth(&u8g2), 1, 1);
  
  // draw the rest with normal font
  u8g2_SetFont(&u8g2, font_helvR08_te);
}

static char *menuentry_get_label(config_menu_entry_t *entry) {
  if(entry->type == CONFIG_MENU_ENTRY_MENU)
    return entry->menu->label;
  if(entry->type == CONFIG_MENU_ENTRY_FILESELECTOR)
    return entry->fsel->label;
  if(entry->type == CONFIG_MENU_ENTRY_LIST)
    return entry->list->label;
  if(entry->type == CONFIG_MENU_ENTRY_BUTTON)
    return entry->button->label;
  if(entry->type == CONFIG_MENU_ENTRY_IMAGE)
    return entry->image->label;
  if(entry->type == CONFIG_MENU_ENTRY_TOGGLE)
    return entry->toggle->label;
  if(entry->type == CONFIG_MENU_ENTRY_RANGE)
    return entry->range->label;
  
  return NULL;
}

static char *menu_get_listentry(config_menu_entry_t *entry, int value) {
  if(!entry || entry->type != CONFIG_MENU_ENTRY_LIST) return NULL;

  config_listentry_t *le = entry->list->listentries;
  while(le) {
    if(le->value == value)
      return le->label;
    le = le->next;
  }

  return NULL;
}

static void menu_draw_entry(config_menu_entry_t *entry, int row, bool selected) {
  menu_debugf("row %d: %s '%s'", row,
	      config_menuentry_get_type_str(entry),
	      menuentry_get_label(entry));

  // all menu entries use some kind of label
  char *s = menuentry_get_label(entry);
  int ypos = MENU_LINE_Y+MENU_ENTRY_H + MENU_ENTRY_H * row;
  int width = u8g2_GetDisplayWidth(&u8g2);
  
  // all menu entries are a plain text
  u8g2_DrawStr(&u8g2, 1, ypos, s);
    
  // prepare highlight
  int hl_w = width;

  // handle second string for list entries
  if(entry->type == CONFIG_MENU_ENTRY_LIST) {
    // get matching variable
    int value = menu_variable_get(entry->list->id);
    char *str = menu_get_listentry(entry, value);

    if(str) {
      // right align entry
      int sw = u8g2_GetStrWidth(&u8g2, str) + 1;
      if(sw > width/2) sw = width/2;
      u8g2_DrawStr(&u8g2, width-sw, ypos, str);
    }
		  
    hl_w = width/2;
  }
  
  // some entries have a small icon to the right    
  else if(entry->type == CONFIG_MENU_ENTRY_MENU)
    u8g2_DrawXBM(&u8g2, hl_w-8, ypos-8, 8, 8, icn_right_bits);

  else if(entry->type == CONFIG_MENU_ENTRY_FILESELECTOR) {
    // icon depends if floppy is inserted
    u8g2_DrawXBM(&u8g2, hl_w-MENU_ENTRY_BASE, ypos-8, 8, 8,
		 sdc_get_image_name(entry->fsel->index)?icn_floppy_bits:icn_empty_bits);
  }
  
  else if(entry->type == CONFIG_MENU_ENTRY_TOGGLE) 
    u8g2_DrawXBM(&u8g2, hl_w-MENU_ENTRY_BASE, ypos-8, 8, 8,
		 menu_variable_get(entry->toggle->id)?icn_on_bits:icn_off_bits);
  
  if(entry->type == CONFIG_MENU_ENTRY_IMAGE) {
    const unsigned char *icon = sdc_get_image_name(entry->image->index+MAX_DRIVES)?icn_floppy_bits:
      entry->image->none_icn?entry->image->none_icn:
      icn_empty_bits;
    
    u8g2_DrawXBM(&u8g2, hl_w-MENU_ENTRY_BASE, ypos-8, 8, 8, icon);
  }
    
  else if(entry->type == CONFIG_MENU_ENTRY_RANGE) {
    char str[5];  // max length 5 for "-100\0"
    
    // draw range value
    int value = menu_variable_get(entry->range->id);
    sprintf(str, "%d", value);

    // right align entry
    int sw = u8g2_GetStrWidth(&u8g2, str) + 1;
    if(sw > width/2) sw = width/2;
    u8g2_DrawStr(&u8g2, width-sw, ypos, str);

    // in "normal" menu mode, the entire range is highlighted when
    // selected. In "edit" mode only the value is highlighted
    if(entry->range->edit) {    
      // only highlight the value
      hl_w = u8g2_GetStrWidth(&u8g2, str) + 1;      
    }
  }
    
  if(selected)
    u8g2_DrawButtonFrame(&u8g2, width-hl_w, ypos, U8G2_BTN_INV, hl_w, 1, 1);
}

// Draws msg centered in lines no wider than the display, from y_in down, and
// returns the y below the last line. With y_in 0 it only measures. A '\n' ends
// a line, the rest breaks at spaces, and a word wider than the display is split.
static int menu_wrap_text(int y_in, const char *msg) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  char line[80];
  int y = y_in;

  u8g2_SetFont(&u8g2, font_helvR08_te);
  if(!*msg) return y + 11;   // an empty message still takes one line

  while(*msg) {
    size_t take = 0;   // characters that fit for sure
    size_t gap  = 0;   // last space among them, 0 = none
    size_t i;

    while(*msg == ' ') msg++;                        // a line never starts with a space
    if(*msg == '\n') { y += 11; msg++; continue; }   // an empty line
    if(!*msg) break;

    // take characters until the line is too wide, the buffer is full or the text ends
    for(i = 0; msg[i] && msg[i] != '\n' && i < sizeof(line) - 1; i++) {
      line[i]     = msg[i];
      line[i + 1] = '\0';
      if(u8g2_GetStrWidth(&u8g2, line) > width) break;
      if(msg[i] == ' ') gap = i;
      take = i + 1;
    }

    // stopped inside a word with a space before it: break at the space instead.
    // Stopped at a space: everything before it fits, the line ends right there.
    if(msg[i] && msg[i] != '\n' && msg[i] != ' ' && gap) take = gap;
    if(take == 0) take = 1;   // even one character is too wide, it goes out anyway
    while(take > 1 && msg[take - 1] == ' ') take--;   // spaces at the end would shift the centering

    line[take] = '\0';
    if(y_in) u8g2_DrawStr(&u8g2, (width - u8g2_GetStrWidth(&u8g2, line)) / 2, y, line);
    y += 11;

    // the next line starts after the '\n' that ended this one
    msg += take;
    while(*msg == ' ') msg++;
    if(*msg == '\n') msg++;
  }

  return y;
}

// draw a dialog box
static bool dialog_opened_osd = false;
static TimerHandle_t dialog_disappear_timer = NULL;  
static char network_ipaddr[16];
static bool network_was_connected = false;

static bool menu_dialog_is_open(void) {
  if(!dialog_disappear_timer) return false;  // no timer -> no dialog
  return xTimerIsTimerActive(dialog_disappear_timer) != pdFALSE;
}

static void menu_dialog_timeout(__attribute__((unused)) TimerHandle_t arg) {
  menu_debugf("Close dialog");
  if(dialog_opened_osd) osd_enable(OSD_INVISIBLE);    
  else                  menu_do(0);
}

static void menu_dialog_close(void) {
  if(!dialog_disappear_timer) return;
  xTimerStop(dialog_disappear_timer, 0);
  menu_dialog_timeout(NULL);
}
  
static void menu_draw_dialog_for(const char *title, const char *msg,
                                  TickType_t duration) {
  // check if osd is already visible
  if(!menu_dialog_is_open()) {
    dialog_opened_osd = !osd_is_visible();
    if(dialog_opened_osd) osd_enable(OSD_VISIBLE);
  }

  if(!dialog_disappear_timer)
    dialog_disappear_timer = xTimerCreate("Dialog timer", duration, pdFALSE,
					   NULL, menu_dialog_timeout);
  else
    xTimerChangePeriod(dialog_disappear_timer, duration, 0);
  xTimerStart(dialog_disappear_timer, 0);
  
  u8g2_ClearBuffer(&u8g2);

  // MENU_LINE_Y is the height of the title incl line
  int y = (64 - MENU_LINE_Y - menu_wrap_text(0, msg))/2;
  
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  
  int width = u8g2_GetDisplayWidth(&u8g2);
  int swid = u8g2_GetStrWidth(&u8g2, title);
 
  // draw title in bold and seperator line
  u8g2_DrawStr(&u8g2, (width-swid)/2, y+MENU_ENTRY_BASE, title);
  u8g2_DrawHLine(&u8g2, (width-swid)/2, y+MENU_ENTRY_H, swid);

  u8g2_SetFont(&u8g2, font_helvR08_te);

  menu_wrap_text(y+23, msg);
  
  u8g2_SendBuffer(&u8g2);
}

void menu_draw_dialog(const char *title, const char *msg) {
  menu_draw_dialog_for(title, msg, pdMS_TO_TICKS(2000));
}

static void menu_draw(const config_menu_t *menu, int selected, int scroll) {
  u8g2_ClearBuffer(&u8g2);
 
  // =============== draw a regular menu =================
  menu_debugf("drawing '%s'", menu->label);  
    
  // draw the title. The root menu tells which TOS is running and, with a
  // modem on port 1, its network address
  if(menu_is_root(menu)) {
    char title[24];
    unsigned short tos = sys_get_tos_version();
    if(tos) snprintf(title, sizeof(title), "TOS %x.%02x", tos >> 8, tos & 0xff);
    else    snprintf(title, sizeof(title), "%s", menu->label);
    menu_draw_title(title, false, selected == 0);

    const char *ip = netdl_get_ip();
    if(ip[0]) {
      u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
      if(selected == 0) u8g2_SetDrawColor(&u8g2, 0);
      int w = u8g2_GetStrWidth(&u8g2, ip);
      u8g2_DrawStr(&u8g2, u8g2_GetDisplayWidth(&u8g2) - w - 1, MENU_ENTRY_BASE - 1, ip);
      u8g2_SetDrawColor(&u8g2, 1);
      u8g2_SetFont(&u8g2, font_helvR08_te);
    }
  } else
    menu_draw_title(menu->label, true, selected == 0);

  config_menu_entry_t *entry = menu->entries;
  for(int i=0;i<scroll;i++) entry=entry->next;  // skip first "scroll" entries
  for(int i=0;i<4 && entry;i++,entry=entry->next)           // then draw up to four entries
    menu_draw_entry(entry, i, selected == scroll+i+1);    
  
  u8g2_SendBuffer(&u8g2);
}

static void menu_fsel_draw(const char *label, sdc_dir_entry_t *dir, int selected, int scroll) {
  u8g2_ClearBuffer(&u8g2);

  // =============== draw a fileselector =================    
  menu_debugf("drawing '%s'", label);
  
  menu_draw_title(label, true, selected == 0);
  menu_timer_enable(false);
  fs_scroll_cur = -1;

  // draw up to four entries
  for(int i=0;i<4 && i<dir_len(dir)-scroll;i++) {            
    menu_debugf("file %s", dir_entry(dir, i+scroll)->name);
    menu_fs_draw_entry(i, dir_entry(dir, i+scroll));
  }
  u8g2_SendBuffer(&u8g2);
}

static void menu_custom_draw(config_custom_t *custom, int selected, int scroll) {
  u8g2_ClearBuffer(&u8g2);

  // =============== draw a custom dialog =================    
  menu_debugf("drawing custom '%s' (%d)", custom->label, selected);

  // title is a regular entry
  menu_draw_title(custom->label, true, selected == 0);

  if(custom->draw)
    custom->draw();
  
  u8g2_SendBuffer(&u8g2);
}

void menu_goto(const config_menu_t *menu) {
  menu_push();
  
  // prepare menu state ...
  menu_state->menu = menu;
  menu_state->selected = 1;
  menu_state->scroll = 0;
  menu_state->type = MENU_TYPE_MENU;
}

static void menu_file_selector_open(config_menu_entry_t *entry) {
  menu_debugf("menu_file_selector_open()");

  // the file selector can either be opened from disk image file
  // selectors or from rom image selectors  
  if(entry->type == CONFIG_MENU_ENTRY_IMAGE) {
    menu_debugf("opening rom image selector");
    fsel_state.ext = entry->image->ext;
    fsel_state.index = entry->image->index + MAX_DRIVES;  // the images are stored after the drives
    fsel_state.none_str = entry->image->none_str;
    fsel_state.none_icn = entry->image->none_icn;
    fsel_state.action = entry->image->action;
  } else {
    menu_debugf("opening drive image selector");
    fsel_state.ext = entry->fsel->ext;
    fsel_state.index = entry->fsel->index;
    fsel_state.none_str = (entry->fsel->index == SDC_COPY_SLOT) ? "Back" : NULL;
#if defined(ENABLE_JTAG) && !defined(DIRECT_SDC_SUPPORTED)
    if(entry->fsel->index == CORE_SWITCH_SLOT) fsel_state.none_str = "Back";
#endif
    fsel_state.none_icn = NULL;
    fsel_state.action = entry->fsel->action;
  }
    
  // The file selector usually works on the sd card as that is what
  // the cores are reading data from. But cores may be loaded from
  // USB as well. The file selectors default path will begin with
  // "/usb" in that case

  // Initialize current working directory if needed
  if(!sdc_get_cwd(fsel_state.index)) {
    bool is_usb = strncasecmp(entry->fsel->def, "/usb", 4) == 0;
    
    // check if USB is to be used but isn't present
    if(is_usb && (disk_status(1) != RES_OK)) {
      menu_draw_dialog("USB Error", "No USB mass storage device connected!");
      return;
    }

    sdc_set_cwd(fsel_state.index, is_usb?"/usb":"/sd");
  }

  // IMAGE and file selectors are both drawn by the fileselector routines
  menu_push();
  menu_state->label = (entry->type == CONFIG_MENU_ENTRY_IMAGE)?entry->image->label:entry->fsel->label;
  menu_state->selected = 1;
  menu_state->scroll = 0;
  menu_state->type = MENU_TYPE_FILESELECTOR;
  
  // scan file system
  menu_state->dir = sdc_readdir(fsel_state.index, NULL, (void*)fsel_state.ext);
  // try to jump to current file. Get the current image name and path
  char *name = sdc_get_image_name(fsel_state.index);
  if(name) {
    debugf("trying to jump to %s", name);
    // try to find name in file list
    for(int i=0;i<dir_len(menu_state->dir);i++) {
      if(strcmp(dir_entry(menu_state->dir, i)->name, name) == 0) {
	debugf("found preset entry %d", i);
	
	// file found, adjust entry and offset
	menu_state->selected = i+1;
	
	if(dir_len(menu_state->dir) > 4 && menu_state->selected > 3) {
	  debugf("more than 4 files and selected is > 3");
	  if(menu_state->selected < dir_len(menu_state->dir)-1) menu_state->scroll = menu_state->selected - 3;
	  else                                                  menu_state->scroll = dir_len(menu_state->dir)-4;
	}
      }
    }
  }  
}

// all other entries in step down
static void menu_pop(void) {
  menu_debugf("menu_pop()");

  // "pop"ing the root menu means to hide it. This will only
  // happen in single button mode with the roots title
  if(!menu_state->prev) {
    osd_enable(OSD_INVISIBLE);
    return;
  }

  // neither should this as we never really close the
  // root menu
  if(menu_state->menu == cfg->menu) {
    vPortFree(menu_state);
    menu_state = NULL;
    return;
  }

  // de-chain first entry
  menu_state_t *m = menu_state;
  menu_state = menu_state->prev;
  vPortFree(m);
}

static void usb_copy(const char *dir, const char *name);

static void menu_fileselector_select(sdc_dir_entry_t *entry) {
  int drive = fsel_state.index;
  menu_debugf("%s %d: file selected '%s'", drive>=MAX_DRIVES?"IMG":"DRV",
	      drive>=MAX_DRIVES?drive-MAX_DRIVES:drive, entry->name);
    
  // stop any scroll timer that might be running
  menu_timer_enable(false);
    
  if(entry->is_dir) {
    if(entry->name[0] == '/') {
      // User selected the "No Disk" entry
      // return to parent form
      menu_pop();
      // Eject; "From USB" has nothing to eject
      if(drive != SDC_COPY_SLOT
#if defined(ENABLE_JTAG) && !defined(DIRECT_SDC_SUPPORTED)
         && drive != CORE_SWITCH_SLOT
#endif
        ) sdc_image_open(drive, NULL);
    } else {	
      // check if we are going up one dir and try to select the
      // directory we are coming from
      char *prev = NULL; 
      if(strcmp(entry->name, "..") == 0) {
	prev = strrchr(sdc_get_cwd(drive), '/');
	if(prev) prev++;
      }

      menu_state->selected = 1;   // start by highlighting '..'
      menu_state->scroll = 0;
      menu_state->dir = sdc_readdir(drive, entry->name, (void*)fsel_state.ext);	
      
      // prev is still valid, since sdc_readdir doesn't free the old string when going
      // up one directory. Instead it just terminates it in the middle	
      if(prev) {
	menu_debugf("up to %s", prev);
	
	// try to find previous dir entry in current dir	  
	for(int i=0;i<dir_len(menu_state->dir);i++) {
	  if(dir_entry(menu_state->dir, i)->is_dir && strcmp(dir_entry(menu_state->dir, i)->name, prev) == 0) {
	    // file found, adjust entry and offset
	    menu_state->selected = i+1;

	    if(dir_len(menu_state->dir) > 4 && menu_state->selected > 3) {
	      if(menu_state->selected < dir_len(menu_state->dir) - 1) menu_state->scroll = menu_state->selected - 3;
	      else                                                    menu_state->scroll = menu_state->selected - 5;
	    }
	  }
	}
      }
    }
  } else if(drive == SDC_COPY_SLOT) {
    // "From USB": onto the card, then back to the menu
    menu_pop();
    usb_copy(sdc_get_cwd(drive), entry->name);
#if defined(ENABLE_JTAG) && !defined(DIRECT_SDC_SUPPORTED)
  } else if(drive == CORE_SWITCH_SLOT) {
    // "Change core": into the flash for good, see core_switch()
    menu_pop();
    core_switch(sdc_get_cwd(drive), entry->name);
#endif
  } else {
    // request insertion of this image
    sdc_image_open(drive, entry->name);
    
    // return to parent form
    menu_pop();

    // check if this is a drive image selection and run action if yes. Image selectors
    // work differently and do an IRQ driven transfer in the background. The action is there executed
    // ocne the transfer itself is done.
    if(drive<MAX_DRIVES && fsel_state.action)
      sys_run_action(fsel_state.action);
  }
}

void menu_run_current_image_action(void) {
  // Note: The download may also have been triggered when loading config from
  // ini file. We may need to find the right action in that case as well.
  // The same is true for mounted disk images
  
  if(!menu_state) return;

  // the current menu state will be a open file selector if the image transfer
  // has been started through the OSD by the user
  if(menu_state->type != MENU_TYPE_FILESELECTOR) {
    menu_debugf("finished download was ini file triggered during boot");

    if(!sdc_check_for_pending_image_uploads()) {
      menu_debugf("no more image uploads pending");

      sys_run_action_by_name("ready");
    }
      
    return;
  }

  // fsel index should be >= MAX_DRIVES and < MAX_DRIVES+MAX_IMAGES as it
  // should point to an image selector
  if(fsel_state.index < MAX_DRIVES || fsel_state.index >= MAX_DRIVES+MAX_IMAGES) return;

  // run the file selector action if one is present
  if(fsel_state.action)
    sys_run_action(fsel_state.action);

}

// user has pressed esc to go back one level
static void menu_back(void) {
  // when in range edit mode, back leaves edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    entry->range->edit = false;
    return;
  }							  

  // stop doing the scroll timer
  menu_timer_enable(false);

  // are we in the root menu?
  if(menu_state->menu == cfg->menu)
    osd_enable(OSD_INVISIBLE);
  else {
    // are we in fileselector?
    if(menu_state->type == MENU_TYPE_FILESELECTOR) {
      // search for ".." in current dir
      sdc_dir_entry_t *entry = NULL;
      for(int i=0;i<dir_len(menu_state->dir);i++)
	if(!strcmp(dir_entry(menu_state->dir, i)->name, ".."))
	  entry = dir_entry(menu_state->dir, i);
      
      // if there was one, go up. Else quit the file selector
      if(entry) menu_fileselector_select(entry);
      else      menu_pop();
    } else
      menu_pop();
  }
}

// user has selected a menu entry
static void menu_select(void) {
  // when in range edit mode, select leaves edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    entry->range->edit = false;
    return;
  }							  

  // if the title was selected, then goto parent form
  if(menu_state->selected == 0) {
    menu_pop();
    return;
  }

  // in fileselector
  if(menu_state->type == MENU_TYPE_FILESELECTOR) {
    menu_fileselector_select(dir_entry(menu_state->dir, menu_state->selected-1));
    return;
  }

  // in a custom dialog: the dialog decides what selecting means. The
  // config entry lookup below must not run here, custom dialogs have none
  if(menu_state->type == MENU_TYPE_CUSTOM) {
    if(menu_state->custom->select)
      menu_state->custom->select(menu_state->selected);
    return;
  }
  
  menu_debugf("Selected: %s '%s'", config_menuentry_get_type_str(entry), menuentry_get_label(entry));

  switch(entry->type) {
  case CONFIG_MENU_ENTRY_FILESELECTOR:
    // user has choosen a file selector
    menu_file_selector_open(entry);
    break;
    
  case CONFIG_MENU_ENTRY_MENU:
    menu_goto(entry->menu);
    break;

  case CONFIG_MENU_ENTRY_LIST: {
    // user has choosen a selection list: go to the next entry in list
    // order and back to the first one after the last. The values need not
    // be 0..n-1, e.g. DIP switch settings like 3, 2, 1
    config_listentry_t *le = entry->list->listentries;
    int value = menu_variable_get(entry->list->id);
    while(le && le->value != value) le = le->next;
    le = (le && le->next) ? le->next : entry->list->listentries;
    if(le) menu_variable_set(entry->list->id, le->value);

    // check if there's an action connected to changing this
    // list. This e.g. happens when changing system settings is
    // meant to trigger a (cold) boot
    if(entry->list->action)
      sys_run_action(entry->list->action);

  } break;

  case CONFIG_MENU_ENTRY_BUTTON:
    if(entry->button->action)
      sys_run_action(entry->button->action);
    break;
	
  case CONFIG_MENU_ENTRY_IMAGE:
    // user has choosen an image selector
    menu_file_selector_open(entry);
    break;
    
  case CONFIG_MENU_ENTRY_TOGGLE:
    menu_variable_set(entry->toggle->id, !menu_variable_get(entry->toggle->id));
    if(entry->toggle->action)
      sys_run_action(entry->toggle->action);
    break;

  case CONFIG_MENU_ENTRY_RANGE:
    // selecting a range entry enables changing its value
    entry->range->edit = true;
    break;
    
  default:
    menu_debugf("unknown %s", config_menuentry_get_type_str(entry));    
  }
}

// timer implementing key repeat
static TimerHandle_t menu_key_repeat_timer = NULL;
static int menu_key_last_event = -1;

// menu_notify() drops an event when the queue is full. A dropped release
// left the key repeating for good: the OSD ran up or down by itself until
// another key was let go, once for more than four minutes. So a release
// that did not fit is remembered here and stops the repeat all the same.
static volatile bool menu_release_lost = false;
static volatile unsigned long menu_events_dropped = 0;

static void menu_key_repeat(__attribute__((unused)) TimerHandle_t arg) {
  if(menu_release_lost) {
    menu_release_lost = false;
    menu_key_last_event = -1;
    return;
  }
  if(menu_key_last_event >= 0) {

    if(menu_key_last_event == MENU_EVENT_UP)     menu_entry_go(-1);
    if(menu_key_last_event == MENU_EVENT_DOWN)   menu_entry_go( 1);

    if(menu_key_last_event == MENU_EVENT_PGUP)   menu_entry_go(-4);
    if(menu_key_last_event == MENU_EVENT_PGDOWN) menu_entry_go( 4);

    if(menu_state->type == MENU_TYPE_MENU)
      menu_draw(menu_state->menu, menu_state->selected, menu_state->scroll);
    else if(menu_state->type == MENU_TYPE_FILESELECTOR)
      menu_fsel_draw(menu_state->label, menu_state->dir, menu_state->selected, menu_state->scroll);
    else if(menu_state->type == MENU_TYPE_CUSTOM)
      menu_custom_draw(menu_state->custom, menu_state->selected, menu_state->scroll);
  
    xTimerChangePeriod( menu_key_repeat_timer, pdMS_TO_TICKS(100), 0);
    xTimerStart( menu_key_repeat_timer, 0 );
  }
}
  
void menu_stop_repeat(void) {
  xTimerStop(menu_key_repeat_timer, 0);
  menu_key_last_event = -1;
}

void menu_do(int event) {
  //
  if(menu_dialog_is_open()) {
    // if the dialog is open, then any key event will close it
    if((event >= MENU_EVENT_UP) && (event <= MENU_EVENT_BACK))
      menu_dialog_close();

    return;
  }

  // -1 is a timer event used to scroll the current file name if it's to long
  // for the OSD
  if(event < 0) {
    if(cfg) {
      if(menu_state->type == MENU_TYPE_FILESELECTOR)
	menu_fs_scroll_entry();
    }
      
    return;
  }
  
  menu_debugf("do %d", event);
  
  if(event)  {
    if(event == MENU_EVENT_TOGGLE) {
      if(!osd_is_visible())
	osd_enable(OSD_VISIBLE);
      else {
	menu_timer_enable(false);
	osd_enable(OSD_INVISIBLE);
	return;  // return now to prevent OSD from being drawn, again
      }
    }

    // a key release event just stops any repeat timer
    if(event == MENU_EVENT_KEY_RELEASE) {
      menu_stop_repeat();
      return;
    }

    // UP/DOWN PGUP and PGDOWN have a repeat
    if(event == MENU_EVENT_UP || event == MENU_EVENT_DOWN ||
       event == MENU_EVENT_PGUP || event == MENU_EVENT_PGDOWN) {
      
      if(menu_key_repeat_timer) {
	menu_release_lost = false;   // a new press; an old lost release is over
	menu_key_last_event = event;
	xTimerChangePeriod( menu_key_repeat_timer, pdMS_TO_TICKS(500), 0);
	xTimerStart( menu_key_repeat_timer, 0 );
      }
    }
    
    if(event == MENU_EVENT_UP)     menu_entry_go(-1);
    if(event == MENU_EVENT_DOWN)   menu_entry_go( 1);

    if(event == MENU_EVENT_PGUP)   menu_entry_go(-4);
    if(event == MENU_EVENT_PGDOWN) menu_entry_go( 4);

    if(event == MENU_EVENT_SELECT) menu_select();
    if(event == MENU_EVENT_BACK)   menu_back();
  }

  // if no dialog is open, then draw menu/fsel
  if(!menu_dialog_is_open()) {  
    if(menu_state->type == MENU_TYPE_MENU)
      menu_draw(menu_state->menu, menu_state->selected, menu_state->scroll);
    else if(menu_state->type == MENU_TYPE_FILESELECTOR)
      menu_fsel_draw(menu_state->label, menu_state->dir, menu_state->selected, menu_state->scroll);  
    else if(menu_state->type == MENU_TYPE_CUSTOM)
      menu_custom_draw(menu_state->custom, menu_state->selected, menu_state->scroll);
  }
}

TimerHandle_t menu_timer_handle;
// queue to forward key press events from USB to MENU
QueueHandle_t menu_queue = NULL;

void menu_timer_enable(bool on) {
  if(on) xTimerStart(menu_timer_handle, 0);
  else   xTimerStop(menu_timer_handle, 0);
}

// a 25Hz timer that can be activated by the menu whenever animations
// are displayed and which should be updated constantly
// The 25 Hz tick for scrolling names. It shares the queue with the keys,
// and whenever the OSD took longer than 40 ms to draw, ticks piled up until
// the queue was full and keys were dropped -- a release among them, see
// menu_release_lost. Now one tick at most waits in the queue.
static volatile bool menu_tick_pending = false;

static void menu_timer(__attribute__((unused)) TimerHandle_t pxTimer) {
  static long msg = -1;
  if(menu_tick_pending) return;
  menu_tick_pending = true;
  if(xQueueSendToBack(menu_queue, &msg,  ( TickType_t ) 0) != pdTRUE)
    menu_tick_pending = false;
}

static const config_menu_t system_menu_main;

// check if menu is the system menu or a submenu of it
static bool menu_is_systemmenu(void) {
  menu_state_t *ms = menu_state;
  
  while(ms) {
    if((ms->type == CONFIG_MENU_ENTRY_MENU) &&
       (ms->menu == &system_menu_main))
      return true;

    ms = ms->prev;
  }
  return false;
}  

static bool net_dialog_jump(uint8_t code);

// Delete key on a drive entry in the menu or inside its file selector: eject the
// drive and return to the menu, which then shows the empty icon. Only real
// drives, not rom image slots and not "From USB".
// (taken from upstream, MiSTle-Dev/FPGA-Companion 9a62d8c and a35ba9e)
static void menu_eject_drive(void) {
  if(!menu_state || !osd_is_visible()) return;

  int drive = -1;
  if(menu_state->type == MENU_TYPE_FILESELECTOR) {
    drive = fsel_state.index;
  } else if(menu_state->type == MENU_TYPE_MENU && menu_state->selected > 0) {
    config_menu_entry_t *entry = menu_get_selected_entry();
    if(entry && entry->type == CONFIG_MENU_ENTRY_FILESELECTOR) drive = entry->fsel->index;
  }

  if(drive < 0 || drive >= MAX_DRIVES || !sdc_get_image_name(drive)) return;

  menu_debugf("DRV %d: eject", drive);
  menu_timer_enable(false);
  if(menu_state->type == MENU_TYPE_FILESELECTOR) menu_pop();
  sdc_image_open(drive, NULL);
  menu_do(MENU_EVENT_NONE);   // redraw
}

static void menu_handle_latin1(uint8_t code) {
  // this function receives lagtin1 encoded keyboard events and
  // is meant to be used for complex text input in custom dialogs
  if(code > 31) menu_debugf("menu_handle_latin1(%d/%c)", code, code);
  else          menu_debugf("menu_handle_latin1(%d)", code);

  // in the download list a letter goes to the first entry starting with it
  if(net_dialog_jump(code)) menu_do(MENU_EVENT_NONE);
  else if(code == 127) menu_eject_drive();   // the Delete key on a drive
}

static void net_dialog_update(void);

static void menu_task(__attribute__((unused)) void *parms) {
  menu_debugf("task running");

  // wait for user events
  while(1) {
    // receive events from usb    
    long cmd;
    xQueueReceive(menu_queue, &cmd, 0xffffffffUL);
    if(cmd == -1) menu_tick_pending = false;
    else          menu_debugf("command %04lx", cmd);   // not 25 lines a second for the tick
    if(menu_events_dropped) {
      menu_debugf("%lu events dropped, the queue was full", menu_events_dropped);
      menu_events_dropped = 0;
    }

    if(cmd == MENU_EVENT_SYSTEM) {
      menu_debugf("system menu requested");
      menu_dialog_close();
      
      // open osd if it's not open, yet
      if(!osd_is_visible()) osd_enable(OSD_VISIBLE);

      // check if we are already in system menu
      if(!menu_is_systemmenu()) {
	menu_goto(&system_menu_main);
	menu_do(0);   // (re)draw menu
      } else
	menu_debugf("Already in system menu");
    } else

    // catch some non-user controlled events here

#ifdef ENABLE_BLUETOOTH
    if(cmd == MENU_EVENT_BLUETOOTH_CONNECTED) {      
      menu_draw_dialog("Bluetooth", "A device has been connected!");
    } else if(cmd == MENU_EVENT_BLUETOOTH_DISCONNECTED) {
      menu_draw_dialog("Bluetooth", "A device has been disconnected!");
    } else if(cmd == MENU_EVENT_BLUETOOTH_SCAN) {
      // as long as the scan runs, 10 x 1.28 s; menu_draw_dialog() is 2 s
      menu_draw_dialog_for("Bluetooth", "Searching for devices for 13 seconds ...",
                           pdMS_TO_TICKS(13000));
    } else if(cmd == MENU_EVENT_BLUETOOTH_PIN_CODE_REQUEST) {
      menu_draw_dialog("Bluetooth", "Please enter pin '123456'");
    } else
#endif

    if(cmd == MENU_EVENT_NET_UPDATE) {
      net_dialog_update();
    } else
    if(cmd == MENU_EVENT_NETWORK_GOT_IP) {
      char message[32];
      snprintf(message, sizeof(message), "IP: %s", network_ipaddr);
      menu_draw_dialog_for("Network", message, pdMS_TO_TICKS(5000));
    } else if(cmd == MENU_EVENT_NETWORK_DISCONNECTED) {
      menu_draw_dialog_for("Network", "disconnected", pdMS_TO_TICKS(3000));
    } else
  if(cmd == MENU_EVENT_USB_MOUNTED) {
      menu_debugf("USB mount event");

      vTaskDelay(100);

      static FATFS usb_fs;
      FRESULT fres;
      if ( (fres = f_mount(&usb_fs, "/usb", 1)) != FR_OK ) menu_debugf("/usb mount failed: %d", fres);
      else {
	menu_debugf("/usb mounted");
#if defined(ENABLE_JTAG) && !defined(DIRECT_SDC_SUPPORTED)
	// the result of a core change before the restart, if any
	if(!core_switch_result())
#endif
	menu_draw_dialog_for("USB", "The USB stick is ready: Shift+F12, From USB", pdMS_TO_TICKS(3000));
      }
    } else if(cmd == MENU_EVENT_USB_UMOUNTED) {
      if (f_unmount("/usb") != FR_OK )	menu_debugf("/usb unmount failed");
      else	                        menu_debugf("/usb unmounted");
      menu_draw_dialog("USB", "A USB mass storage device has been removed!");
    }

    // A stick takes a moment to answer, and one that never does would
    // otherwise leave nothing on the screen at all.
    else if(cmd == MENU_EVENT_USB_DETECTED)
      menu_draw_dialog_for("USB", "USB stick found, reading it ...", pdMS_TO_TICKS(8000));
    else if(cmd == MENU_EVENT_USB_FAILED)
      menu_draw_dialog_for("USB", "The USB stick does not answer. Plug it in again, or try another one.",
			   pdMS_TO_TICKS(5000));

    else if((cmd & 0xffffff00) == MENU_EVENT_KEY_LATIN1)
      menu_handle_latin1(cmd & 0xff);

    else
      menu_do(cmd);
  }
}

TaskHandle_t menu_handle = NULL;

void menu_init(void) {
  menu_debugf("Initializing");

  // check if a config was loaded. If no, use the legacy menu
  if(!cfg) {  
    menu_debugf("Warning: No core config found. Is this an old legacy core?");
    return;
  }

  // a config was loaded, use that
  menu_debugf("Using configured menu");

  menu_debugf("Setting up variables");
  menu_setup_variables();
    
  menu_debugf("Processing init action");
  sys_run_action_by_name("init");

  menu_goto(cfg->menu);    

  // create a one shot timer for key repeat
  menu_key_repeat_timer = xTimerCreate( "Key repeat timer", pdMS_TO_TICKS(500), pdFALSE,
					NULL, menu_key_repeat);
    
  // switch MCU controlled leds off
  sys_set_leds(0x00);
  
  // create a 25 Hz timer that frequently wakes the OSD thread
  // allowing for animations
  menu_timer_handle = xTimerCreate("Menu scroll timer", pdMS_TO_TICKS(40), pdTRUE,
				   NULL, menu_timer);
  
  // message queue from USB to OSD
  menu_queue = xQueueCreate(32, sizeof( long ) );   // 4 bytes each
  
  // start a thread for the on screen display    
#ifndef MENU_TASK_STACK
#define MENU_TASK_STACK 4096
#endif
  xTaskCreate(menu_task, (char *)"menu_task", MENU_TASK_STACK, NULL, configMAX_PRIORITIES-3, &menu_handle);

  // At this point, the USB may already be ready. But since the
  // menu task wasn't ready by now, it never had a chance to be mounted properly
  if(mcu_hw_usb_msc_present()) {
    menu_debugf("triggering delayed USB init");
    menu_notify(MENU_EVENT_USB_MOUNTED);
  }
}

// queue an event for the menu task
void menu_notify(unsigned long msg) {
  if(menu_queue) 
  {
    if(xQueueSendToBackFromISR(menu_queue, &msg,  ( TickType_t ) 0) != pdTRUE) {
      menu_events_dropped++;
      if(msg == MENU_EVENT_KEY_RELEASE) menu_release_lost = true;
    }
  } else
    menu_debugf("menu_notify(): queue/menu task not ready!");
}

void menu_notify_ip(const char *ipaddr) {
  snprintf(network_ipaddr, sizeof(network_ipaddr), "%s", ipaddr);
  network_was_connected = true;
  menu_notify(MENU_EVENT_NETWORK_GOT_IP);
}

void menu_notify_network_disconnected(void) {
  // only report a disconnect if we had reported an ip address before
  if(!network_was_connected) return;
  network_was_connected = false;
  menu_notify(MENU_EVENT_NETWORK_DISCONNECTED);
}

void menu_joystick_state(unsigned char state) {
  static unsigned char prev_state = 0;

  if(state != prev_state) {
    static unsigned long msg;
    menu_debugf("Joystick state change to %02x", state);

    msg = 0;
    if(state & 0x08) msg = MENU_EVENT_UP;      
    if(state & 0x04) msg = MENU_EVENT_DOWN;      
    if(state & 0x02) msg = MENU_EVENT_BACK;      
    if(state & 0x10) msg = MENU_EVENT_SELECT;
    if(state & 0x20) msg = MENU_EVENT_BACK;
    if(!msg) msg = MENU_EVENT_KEY_RELEASE;
    menu_notify(msg);

    prev_state = state;
  }
}

// ===========  "single button" menu control ================

// The menu is only controlled through the single OSD hw button
// whenever no keyboard or gamepad is connected.

// three events are supported:
// <200ms: next menu entry
// >200ms and < 3s: select menu entry
// >3s: up one menu level

static TickType_t menu_button_last_event = 0;
static TimerHandle_t menu_button_timer = NULL;

static void menu_button_timer_handler(__attribute__((unused)) TimerHandle_t arg) {
  menu_debugf("button timer event");

  menu_notify(MENU_EVENT_BACK);
  menu_notify(MENU_EVENT_KEY_RELEASE);
  menu_button_last_event = 0;
}

void menu_button_state(unsigned char state) {
  menu_debugf("Button state %02x", state);

  if(mcu_hw_hid_present()) {
    if(state & 0x01) {
      // with keyboard and/or gamepad detected implement the same behaviour as
      // previous versions had
      menu_notify(MENU_EVENT_TOGGLE);
    }
  } else {
    static unsigned char prev_state = 0;

    if(state != prev_state) {
      menu_debugf("Button state change to %02x", state);

      // button has been pressed?
      if(state & 1) {
	// show the osd if it's closed
	if(!osd_is_visible())
	  menu_notify(MENU_EVENT_TOGGLE);
	else {	
	  menu_button_last_event = xTaskGetTickCount();
	  
	  if(!menu_button_timer)
	    menu_button_timer = xTimerCreate( "Button timer", pdMS_TO_TICKS(3000), pdFALSE,
					      NULL, menu_button_timer_handler);
	  
	  xTimerStart(menu_button_timer, 0);
	}
      } else if(menu_button_last_event) {
	xTimerStop(menu_button_timer, 0);

	// button action depends on press duration
	TickType_t len = xTaskGetTickCount() - menu_button_last_event;
	if(len > pdMS_TO_TICKS(200)) {
	  menu_notify(MENU_EVENT_SELECT);
	  menu_notify(MENU_EVENT_KEY_RELEASE);
	} else {
	  menu_notify(MENU_EVENT_DOWN);
	  menu_notify(MENU_EVENT_KEY_RELEASE);
	}
	menu_button_last_event = 0;
      }
	
      prev_state = state;
    }
  }
}

/* ================= system menu =================== */

/*  For simplicity and flexibility, the system menu is
    not strictly bound to use the menu structures. Instead
    it can use programmatically designed dialogs.

    Its about box is an example for this.
*/

static int about_length(void) {
  // return the number of selectable items inside the about dialog.
  return 0;
}

static void about_draw(void) {
  u8g2_DrawStr(&u8g2, 20, MENU_LINE_Y + 1*MENU_ENTRY_H, "FPGA Companion");
  u8g2_DrawStr(&u8g2, 5, MENU_LINE_Y + 2*MENU_ENTRY_H, "(c) 2026 the MiSTle Team");
  u8g2_DrawStr(&u8g2, 0, MENU_LINE_Y + 3*MENU_ENTRY_H, "http://github.com/mistle-dev");
}

const static config_custom_t about = {
  .label = "About",
  .length = about_length,
  .draw = about_draw
};

static void about_func(void) {
  menu_debugf("About");

  // push a custom menu
  menu_push();
  menu_state->type = MENU_TYPE_CUSTOM;
  menu_state->custom = &about;
  menu_state->selected = 0;
  menu_state->scroll = 0;
}

static const config_action_command_t about_exec = {
  .code = CONFIG_ACTION_COMMAND_EXEC,
  .exec = about_func
};

static const config_action_t about_action = {
  .name = "about",
  .commands = (config_action_command_t*)&about_exec
};

static const config_button_t about_btn = {
  .label = "About",
  .action = (config_action_t*)&about_action
};


// the system menu is hard coded as it doesn't need to
// be modified by the running core

#ifdef ENABLE_BLUETOOTH

void btscan(void) {
  menu_notify(MENU_EVENT_BLUETOOTH_SCAN);
  bluetooth_scan();
}

static const config_action_command_t bluetooth_exec = {
  .code = CONFIG_ACTION_COMMAND_EXEC,
  .exec = btscan
};

static const config_action_t bluetooth_action = {
  .name = "btscan",
  .commands = (config_action_command_t*)&bluetooth_exec
};

static const config_button_t bluetooth_btn = {
  .label = "Scan Bluetooth",
  .action = (config_action_t*)&bluetooth_action
};

static const config_menu_entry_t system_menu_bluetooth = {
  .type = CONFIG_MENU_ENTRY_BUTTON,
  .button = (config_button_t*)&bluetooth_btn
};
#endif

// the usb core file selector
static char *core_exts[] = { "fs", "bin", NULL }; 

#if MAX_CORES < 2
#error "Setup at least 2 cores in config.h"
#endif

#ifdef DIRECT_SDC_SUPPORTED
static const config_fsel_t sdc_core_fsel = {
  .index = MAX_DRIVES+MAX_IMAGES+1,
  .label = "Load Core from SD",
  .def = "/sd/core.bin",
  .ext = (char**)core_exts
};

// second entry in main system menu
static const config_menu_entry_t system_menu_sdc_core_fsel = {
  .type = CONFIG_MENU_ENTRY_FILESELECTOR,
  .fsel = (config_fsel_t*)&sdc_core_fsel,
#ifdef ENABLE_BLUETOOTH
  .next = (config_menu_entry_t*)&system_menu_bluetooth
#endif
};
#endif

static const config_fsel_t usb_core_fsel = {
  .index = MAX_DRIVES+MAX_IMAGES+0,
  .label = "Load Core from USB",
  .def = "/usb/core.bin",
  .ext = (char**)core_exts
};

// a message that stays a while, for what start-up found
void menu_draw_dialog_long(const char *title, const char *msg) {
  menu_draw_dialog_for(title, msg, pdMS_TO_TICKS(30000));
}

#if defined(ENABLE_JTAG) && !defined(DIRECT_SDC_SUPPORTED)
/* ---- "Change core": another machine from the USB stick, for good ----
   A core file on the stick goes into the flash, so it is the machine from
   now on, also after switching off, until another one is chosen. In this
   order, so that nothing is touched unless all of it can be done:
   1. the file is there; the modem's display and the OSD say what is about
      to happen and that the screen goes dark
   2. the bus to the FPGA is held: no part of the companion talks to an
      empty chip (that hung the first feasibility test)
   3. through JTAG: the SRAM erased, the core written into the flash below
      0x100000, every page read back (gowin_flash_core()); the ROMs above
      are not touched, only looked at
   4. the core from the same file into the SRAM: it runs at once. A reload
      from the flash through JTAG does not work on the Tang Nano 20K, not
      even with Gowin's own programmer
   5. the result on the card, then a clean restart of the companion for the
      new core's menu; the OSD and the display show the result (main.c)
   The slot is the one of "Load Core from SD", which this board has not. */
#include "gowin.h"

// the machine's name from its core file: "/usb/c64nano.fs" -> "C64"
static void core_machine(const char *path, char *out, size_t size) {
  static const char *known[][2] = {
    { "c64nano", "C64" }, { "atarist", "Atari ST" }, { "nanomig", "Amiga" } };
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  const char *dot = strrchr(b, '.');
  size_t n = dot ? (size_t)(dot - b) : strlen(b);
  for(unsigned i = 0; i < sizeof(known)/sizeof(known[0]); i++)
    if(strlen(known[i][0]) == n && !strncasecmp(b, known[i][0], n)) {
      snprintf(out, size, "%s", known[i][1]);
      return;
    }
  snprintf(out, size, "%.*s", (int)n, b);
}

// The result goes to the USB stick, before the new core is loaded: the
// stick hangs on the companion itself, the SD card only through a core,
// and a freshly loaded core has not set the card up -- writing there
// right after the change went nowhere. After the restart the companion
// finds it on the stick, see core_switch_result().
#define CORE_SWITCH_RESULT "/usb/CORESW.NEW"

static void core_switch_note(const char *line) {
  FIL f;
  if(f_open(&f, CORE_SWITCH_RESULT, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
    f_puts(line, &f);
    f_puts("\r\n", &f);
    f_close(&f);
  }
}

// Once the stick is mounted after a restart: a result waiting on it is
// shown in the OSD and on the modem's display, and copied to the card.
static bool core_switch_result(void) {
  FIL f;
  char line[170];
  if(f_open(&f, CORE_SWITCH_RESULT, FA_OPEN_EXISTING | FA_READ) != FR_OK) return false;
  if(!f_gets(line, sizeof(line), &f)) line[0] = 0;
  f_close(&f);
  f_unlink(CORE_SWITCH_RESULT);
  char *e = strpbrk(line, "\r\n");
  if(e) *e = 0;
  if(!line[0]) return false;
  menu_debugf("change core, the result: %s", line);

  sdc_lock();
  if(f_open(&f, CARD_MOUNTPOINT "/CORESW.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
    f_puts(line, &f);
    f_puts("\r\n", &f);
    f_close(&f);
  }
  sdc_unlock();

  char m[190];
  snprintf(m, sizeof(m), "30:Core changed|%s", line);
  if(!netdl_display_message(m, 0)) netdl_boot_message(m);
  menu_draw_dialog_long("Change core", line);
  return true;
}

static void core_switch(const char *dir, const char *file) {
  static char result[160];
  char path[128], machine[24], msg[120];
  FILINFO fno;

  snprintf(path, sizeof(path), "%s/%s", dir, file);
  core_machine(path, machine, sizeof(machine));
  menu_debugf("change core to %s (%s)", path, machine);

  // 1. the file, and what is about to happen
  sdc_lock();
  FRESULT r = f_stat(path, &fno);
  sdc_unlock();
  if(r != FR_OK) {
    menu_draw_dialog_long("Change core", "The file is not on the stick. Nothing was done.");
    return;
  }
  snprintf(msg, sizeof(msg), "Switching|to %s|Screen dark for|about a minute.|Do not switch off.", machine);
  netdl_display_message(msg, 4000);
  snprintf(msg, sizeof(msg), "%s is written for good. The screen goes dark for about a minute. Do not switch off.", machine);
  menu_draw_dialog_for("Change core", msg, pdMS_TO_TICKS(4000));
  vTaskDelay(pdMS_TO_TICKS(3500));

  // 2. to 4., with the bus held; no sdc_lock() in here, a task holding it
  // may be waiting for the bus
  mcu_hw_spi_lock();
  uint32_t written;
  bool tos, cbm;
  const char *err = gowin_flash_core(path, &written, &tos, &cbm);

  // what the new machine still lacks, if anything
  const char *lacks = "";
  if(!strcmp(machine, "Atari ST") && !tos) lacks = " No TOS in the flash yet, see the ST guide.";
  if(!strcmp(machine, "C64") && !cbm)      lacks = " No 1541 ROMs in the flash yet, see the C64 guide.";
  if(!err)
    snprintf(result, sizeof(result), "%s written (%lu KB), from now on at every start.%s [%s]",
             machine, (unsigned long)(written / 1024), lacks, gowin_flash_diag());
  else
    snprintf(result, sizeof(result), "%s NOT written: %s. It runs until power off.", machine, err);
  menu_debugf("change core: %s", result);
  core_switch_note(result);          // before the upload, which unmounts the stick

  mcu_hw_upload_core(path);
  mcu_hw_spi_unlock();
  bool running = sys_wait4fpga();
  menu_debugf("change core: %s", running ? "running" : "NOT running");

  // 5. a clean restart for the new core's menu; the result waits on the stick
  vTaskDelay(pdMS_TO_TICKS(500));
  mcu_hw_reset();
}

static char *core_switch_exts[] = { "fs", "bin", NULL };

static const config_fsel_t core_switch_fsel = {
  .index = CORE_SWITCH_SLOT,
  .label = "Change core",
  .def = "/usb",
  .ext = (char**)core_switch_exts
};

static const config_menu_entry_t system_menu_core_switch = {
  .type = CONFIG_MENU_ENTRY_FILESELECTOR,
  .fsel = (config_fsel_t*)&core_switch_fsel,
#ifdef ENABLE_BLUETOOTH
  .next = (config_menu_entry_t*)&system_menu_bluetooth
#endif
};
#endif

// second entry in main system menu
static const config_menu_entry_t system_menu_usb_core_fsel = {
  .type = CONFIG_MENU_ENTRY_FILESELECTOR,
  .fsel = (config_fsel_t*)&usb_core_fsel,
#if defined(ENABLE_JTAG) && !defined(DIRECT_SDC_SUPPORTED)
  .next = (config_menu_entry_t*)&system_menu_core_switch
#else
#ifdef DIRECT_SDC_SUPPORTED
  .next = (config_menu_entry_t*)&system_menu_sdc_core_fsel
#else
#ifdef ENABLE_BLUETOOTH
  .next = (config_menu_entry_t*)&system_menu_bluetooth
#endif
#endif
#endif
};

/* ---- the download dialog: files from a web server onto the SD card ----
   The companion reaches the server through the modem on the core's port 1,
   see net.c. The dialog shows the server's file list and, while a file is
   loading, a progress bar. */

static const config_custom_t net_dialog;

// With a list, its first line fetches it again: the companion holds one
// list per folder until it is told otherwise, and a server's collection
// grows -- a game sent from stserve's catalogue in the browser shows up at
// the top of the list once it is fetched again.
static int net_dialog_length(void) {
  // "Reload list" and the file list, or the single entry that loads it
  int n = netdl_entry_count();
  return n ? n + 1 : 1;
}

static void net_dialog_draw(void) {
  int width = u8g2_GetDisplayWidth(&u8g2);
  int st = netdl_state();

  if(st == NET_STATE_BUSY) {
    // message, byte count and a bar
    char str[32];
    uint32_t total = netdl_bytes_total(), done = netdl_bytes_done();
    u8g2_DrawStr(&u8g2, 2, MENU_LINE_Y + 1*MENU_ENTRY_H, netdl_message());
    if(total) snprintf(str, sizeof(str), "%lu / %lu KB", (unsigned long)(done/1024), (unsigned long)(total/1024));
    else      snprintf(str, sizeof(str), "%lu KB", (unsigned long)(done/1024));
    u8g2_DrawStr(&u8g2, 2, MENU_LINE_Y + 2*MENU_ENTRY_H, str);
    int y = MENU_LINE_Y + 2*MENU_ENTRY_H + 4;
    u8g2_DrawFrame(&u8g2, 2, y, width-4, 8);
    if(total) u8g2_DrawBox(&u8g2, 3, y+1, (int)(((uint64_t)(width-6) * done) / total), 6);
    return;
  }

  int n = netdl_entry_count();
  if(!n) {
    // no list yet: the entry that fetches it, and what happened last time
    int y = MENU_LINE_Y + 1*MENU_ENTRY_H;
    u8g2_DrawStr(&u8g2, 2, y, "Load file list");
    if(menu_state->selected == 1)
      u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
    // Where we stand with the modem, so that waiting for it does not look
    // like nothing happening: it needs a while after power up, and the
    // address it shows here is the proof that the network is there.
    if(!netdl_get_server()[0])
      u8g2_DrawStr(&u8g2, 2, MENU_LINE_Y + 2*MENU_ENTRY_H, "no server= in ini");
    else if(st == NET_STATE_IDLE)
      u8g2_DrawStr(&u8g2, 2, MENU_LINE_Y + 2*MENU_ENTRY_H, netdl_link_text());
    else
      u8g2_DrawStr(&u8g2, 2, MENU_LINE_Y + 2*MENU_ENTRY_H, netdl_message());

    // Two small lines in words: where the fetch goes, and how the last one
    // ended -- the modem's own reason and its WiFi signal, "stalled -78dBm"
    // or "fetched 142 KB -63dBm". The raw characters on the line used to be
    // here; they are in NETLOG.TXT and the console now.
    {
      char row[26];   // 25 characters of 5 pixels fill the 128
      u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
      if(netdl_fetch_to()[0]) snprintf(row, sizeof(row), "%s", netdl_fetch_to());
      else if(netdl_get_server()[0]) snprintf(row, sizeof(row), "to %s", netdl_get_server());
      else row[0] = 0;
      u8g2_DrawStr(&u8g2, 0, MENU_LINE_Y + 3*MENU_ENTRY_H - 4, row);
      snprintf(row, sizeof(row), "%s", netdl_fetch_result());
      u8g2_DrawStr(&u8g2, 0, MENU_LINE_Y + 3*MENU_ENTRY_H + 5, row);
      u8g2_SetFont(&u8g2, font_helvR08_te);
    }
    return;
  }

  // the sectors the ST's floppy has read, for chasing a drive sound that
  // will not stop: it is here with a list loaded as well, top right corner.
  {
    char row[12];
    snprintf(row, sizeof(row), "s%lu", (unsigned long)sdc_sector_requests);
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
    u8g2_DrawStr(&u8g2, width - 5*strlen(row) - 2, MENU_LINE_Y - 2, row);
    u8g2_SetFont(&u8g2, font_helvR08_te);
  }

  // the list, four lines, scrolled like a menu, "Reload list" first
  for(int i=0;i<4 && i+menu_state->scroll < n+1;i++) {
    int row = i + menu_state->scroll;
    int y = MENU_LINE_Y + (i+1)*MENU_ENTRY_H;
    char label[NET_NAME_LEN + 3];
    if(!row)                             snprintf(label, sizeof(label), "Reload list");
    else if(netdl_entry_marked(row - 1)) snprintf(label, sizeof(label), "> %s", netdl_entry_name(row - 1));
    else                                 snprintf(label, sizeof(label), "%s", netdl_entry_name(row - 1));
    u8g2_DrawStr(&u8g2, 2, y, label);
    if(row == menu_state->selected - 1)
      u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
  }
}

static void net_dialog_select(int selected) {
  if(netdl_state() == NET_STATE_BUSY) return;
  if(!netdl_entry_count() || selected == 1) netdl_request_list();
  else                                      netdl_request_download(selected - 2);
}

// A typed letter or digit selects the first entry starting with it, the
// next one on the next press of the same key. Returns whether it moved.
static bool net_dialog_jump(uint8_t code) {
  if(!menu_state || menu_state->type != MENU_TYPE_CUSTOM || menu_state->custom != &net_dialog)
    return false;
  int n = netdl_entry_count();
  if(!n || netdl_state() == NET_STATE_BUSY) return false;
  if(!((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')))
    return false;
  if(code >= 'a') code -= 'a' - 'A';

  // start after the current entry, so the same key walks through them
  int cur = menu_state->selected - 2;   // index of the selected entry
  for(int k = 1; k <= n; k++) {
    int i = (cur + k + n) % n;
    unsigned char c = (unsigned char)netdl_entry_name(i)[0];
    if(c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if(c != code) continue;
    int row = i + 1;                     // the row on the screen, after "Reload list"
    menu_state->selected = row + 1;
    if(row < menu_state->scroll) menu_state->scroll = row;
    if(row > menu_state->scroll + 3) menu_state->scroll = row - 3;
    return true;
  }
  return false;
}

static const config_custom_t net_dialog = {
  .label = "Download",
  .length = net_dialog_length,
  .draw = net_dialog_draw,
  .select = net_dialog_select
};

static void net_dialog_func(void) {
  menu_push();
  menu_state->type = MENU_TYPE_CUSTOM;
  menu_state->custom = &net_dialog;
  menu_state->selected = 1;
  menu_state->scroll = 0;
}

static const config_action_command_t net_exec = {
  .code = CONFIG_ACTION_COMMAND_EXEC,
  .exec = net_dialog_func
};

static const config_action_t net_action = {
  .name = "download",
  .commands = (config_action_command_t*)&net_exec
};

static const config_button_t net_btn = {
  .label = "Download...",
  .action = (config_action_t*)&net_action
};

// redraw the download dialog if it is on screen
static void net_dialog_update(void) {

  int st = netdl_state();
  if(st == NET_STATE_DONE || st == NET_STATE_ERROR)
    menu_draw_dialog_for("Download", (char*)netdl_message(), pdMS_TO_TICKS(3000));
  else if(osd_is_visible() && !menu_dialog_is_open() && menu_state &&
	  menu_state->type == MENU_TYPE_CUSTOM && menu_state->custom == &net_dialog) {
    // a new folder is a new list: start at its first line
    if(menu_state->selected > net_dialog_length()) {
      menu_state->selected = 1;
      menu_state->scroll = 0;
    }
    menu_do(MENU_EVENT_NONE);
  }
}

/* ---- "From USB": a file from a USB stick onto the SD card ----
   For whoever keeps the games on a stick and has no server. The core's
   floppy reads a disk image straight from the SD card, the companion only
   tells it which sectors of the card hold it -- a disk on the stick is out
   of its reach. So the file is copied into the card's root first, where
   every file selector of every core finds it, as a download lands. */

static char *usb_copy_exts[] = { "*", NULL };

static const config_fsel_t usb_copy_fsel = {
  .index = SDC_COPY_SLOT,
  .label = "From USB",
  .def = "/usb",
  .ext = (char**)usb_copy_exts
};

static const config_menu_entry_t system_menu_usb_copy = {
  .type = CONFIG_MENU_ENTRY_FILESELECTOR,
  .fsel = (config_fsel_t*)&usb_copy_fsel,
  .next = (config_menu_entry_t*)&system_menu_usb_core_fsel
};

static void usb_copy(const char *dir, const char *name) {
  static const char tmp[] = CARD_MOUNTPOINT "/USBCOPY.TMP";
  char src[strlen(dir) + strlen(name) + 2];
  char dst[strlen(CARD_MOUNTPOINT) + strlen(name) + 2];
  snprintf(src, sizeof(src), "%s/%s", dir, name);
  snprintf(dst, sizeof(dst), "%s/%s", CARD_MOUNTPOINT, name);
  menu_debugf("copying %s to %s", src, dst);
  menu_draw_dialog_for("From USB", "Copying to the SD card ...", pdMS_TO_TICKS(60000));

  FIL in, out;
  const char *why = NULL;
  sdc_lock();
  if(f_open(&in, src, FA_OPEN_EXISTING | FA_READ) != FR_OK)
    why = "Cannot read it from the stick";
  else if(f_open(&out, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    f_close(&in);
    why = "Cannot write to the SD card";
  }
  sdc_unlock();

  // a piece at a time, so the core's own card access gets its turns
  if(!why) {
    unsigned char buf[1024];
    UINT n = 0, w = 0;
    do {
      sdc_lock();
      FRESULT r = f_read(&in, buf, sizeof(buf), &n);
      if(r == FR_OK && n) r = f_write(&out, buf, n, &w);
      sdc_unlock();
      if(r != FR_OK || (n && w != n)) { why = "Copying failed"; break; }
    } while(n == sizeof(buf));
    sdc_lock();
    f_close(&in);
    if(f_close(&out) != FR_OK && !why) why = "Copying failed";
    // the old copy goes only once the new one is complete
    if(!why) {
      f_unlink(dst);
      if(f_rename(tmp, dst) != FR_OK) why = "Cannot name it on the card";
    } else
      f_unlink(tmp);
    sdc_unlock();
  }

  if(why) {
    menu_debugf("copy failed: %s", why);
    menu_draw_dialog_for("From USB", why, pdMS_TO_TICKS(3000));
  } else {
    char msg[48 + 32];
    snprintf(msg, sizeof(msg), "%.47s is on the SD card now", name);
    menu_draw_dialog_for("From USB", msg, pdMS_TO_TICKS(3000));
  }
}

// second entry in main system menu
static const config_menu_entry_t system_menu_net = {
  .type = CONFIG_MENU_ENTRY_BUTTON,
  .button = (config_button_t*)&net_btn,
  .next = (config_menu_entry_t*)&system_menu_usb_copy
};

// first entry in main system menu
static const config_menu_entry_t system_menu_about = {
  .type = CONFIG_MENU_ENTRY_BUTTON,
  .button = (config_button_t*)&about_btn,
  .next = (config_menu_entry_t*)&system_menu_net
};

// the main system menu
static const config_menu_t system_menu_main = {
  .label = "Companion",
  .entries = (config_menu_entry_t*)&system_menu_about
};

