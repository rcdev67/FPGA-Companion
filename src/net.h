/*
  net.h

  Network access for the companion itself through a Hayes modem (an ESP32
  running Zimodem or ESP-AT) on port 1 of the core's port protocol. Used to
  pull files from a web server onto the SD card.
*/

#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

#define NET_MAX_ENTRIES   32
#define NET_NAME_LEN      48      // the name as shown and as written to the card
#define NET_HREF_LEN      96      // the name as the server wants it in the URL
#define NET_SERVER_LEN    64
#define NET_PATH_LEN      128     // the folder the list is showing

// states shown by the OSD
#define NET_STATE_IDLE     0   // nothing done yet, no list
#define NET_STATE_BUSY     1   // fetching the list or a file
#define NET_STATE_LIST     2   // list of files available
#define NET_STATE_ERROR    3   // last request failed, see netdl_message()
#define NET_STATE_DONE     4   // last download finished

void netdl_init(void);

// bytes that arrived from the modem, called from the core's interrupt handler
void netdl_port_bytes(const unsigned char *data, int len);

// server "host:port" from the ini file
void netdl_set_server(const char *server);
const char *netdl_get_server(void);
void netdl_set_wifi(const char *net_and_password);   // "MyNet,MyPassword" from the ini
const char *netdl_get_wifi(void);
void netdl_set_timezone(const char *tz);   // a Zimodem time zone code from the ini, "CET", "CEST", "UTC" ...
const char *netdl_get_timezone(void);
const char *netdl_link_text(void);
void netdl_set_clock_wait(int s);
// play=: every so many seconds ask stserve for a game to play, 0 = never
void netdl_set_play(int s);
int netdl_get_play(void);
void netdl_set_save(bool on);       // ini save=: written disk images go back to stserve
bool netdl_get_save(void);
int netdl_get_clock_wait(void);
bool netdl_hold_start(void (*release)(void));
const char *netdl_get_ip(void);

// requests from the menu, handled in the net task
void netdl_request_list(void);
// a message over the modem's status display, "Title|text"; waits up to
// wait_ms for it to be sent, true if it was
bool netdl_display_message(const char *msg, int wait_ms);
// the same, sent with the first contact to the modem after start-up
void netdl_boot_message(const char *msg);
void netdl_request_download(int index);

// state for the menu to draw
int netdl_state(void);
const char *netdl_message(void);
int netdl_entry_count(void);
const char *netdl_entry_name(int index);
// sent from the catalogue: "> " to the card, "USB> " to the stick
#define NET_MARK_NONE   0
#define NET_MARK_CARD   1
#define NET_MARK_STICK  2
int netdl_entry_mark(int index);
// where a file without a mark goes: false the SD card, true the USB stick
void netdl_set_to_stick(bool on);
bool netdl_to_stick(void);
uint32_t netdl_bytes_done(void);
uint32_t netdl_bytes_total(void);
const char *netdl_tail(void);   // the last characters exchanged with the modem
const char *netdl_fetch_to(void);      // "to <host>:<port>" of the last fetch, "" before
const char *netdl_fetch_result(void);  // its outcome: "stalled -78dBm", "fetched 142 KB -63dBm"

#endif // NET_H
