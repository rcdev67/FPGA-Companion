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
void netdl_set_clock_wait(int s);
int netdl_get_clock_wait(void);
bool netdl_hold_start(void (*release)(void));
const char *netdl_get_ip(void);

// the Bluetooth controller on the modem (Zimodem built with Bluepad32)
void netdl_request_joy(bool pair);  // ask for the status, or start pairing first
const char *netdl_joy_status(void); // "XBox One", "none", "asking..." and the like
bool netdl_joy_busy(void);     // the modem's address as it last reported it, "" if unknown

// requests from the menu, handled in the net task
void netdl_request_list(void);
void netdl_request_download(int index);

// state for the menu to draw
int netdl_state(void);
const char *netdl_message(void);
int netdl_entry_count(void);
const char *netdl_entry_name(int index);
uint32_t netdl_bytes_done(void);
uint32_t netdl_bytes_total(void);
const char *netdl_tail(void);   // the last characters exchanged with the modem

#endif // NET_H
