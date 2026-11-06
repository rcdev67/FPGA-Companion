/*
  net.c

  The companion's own way onto the network. A Hayes style WiFi modem, the
  ESP32-C3 SuperMini running Zimodem or ESP-AT for instance, hangs off the
  core's second serial port (port 1 of the port protocol, "Serial: Netz" in
  the OSD). This file dials the modem, speaks plain HTTP/1.0 over the raw
  TCP stream and writes what comes back to the SD card.

  The modem is already joined to the WiFi: that is set up once from the ST
  side or from a PC and stored in the modem itself.

  Both firmwares are told apart by their answer to "ATI":
    Zimodem: ATD host:port  -> CONNECT, then a raw stream, +++ / ATH end it
    ESP-AT:  AT+CIPSTART / AT+CIPMODE=1 / AT+CIPSEND -> ">", then the
             passthrough stream, +++ / AT+CIPCLOSE end it
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#else
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <stream_buffer.h>
#endif

#include <ff.h>

#include "net.h"
#include "sysctrl.h"
#include "sdc.h"
#include "menu.h"
#include "config.h"   // for config_find_role() and the core's name
#include "debug.h"
#include "mcu_hw.h"   // for mcu_hw_usb_msc_present()
#include "sysctrl.h"  // for sys_run_action_by_name()
#include "ps2helper.h" // keys typed into the C64, see play_start()

#define NET_PORT          1
#define NET_RX_BUF        2048    // bytes the interrupt handler may queue
#define NET_LINE_LEN      128
#define NET_FILE_CHUNK    1024    // bytes per f_write
#define NET_INDEX_FILE    "index.txt"

#define MODEM_UNKNOWN     0
#define MODEM_ZIMODEM     1
#define MODEM_ESPAT       2

static StreamBufferHandle_t rx_stream;
static QueueHandle_t req_queue;

static char server[NET_SERVER_LEN] = "";
static int state = NET_STATE_IDLE;
static char message[48] = "";
static char names[NET_MAX_ENTRIES][NET_NAME_LEN];   // shown, and the name on the card
static char cwd[NET_PATH_LEN];                      // the folder shown, "" at the top
static char hrefs[NET_MAX_ENTRIES][NET_HREF_LEN];   // as the server wants it in the URL
// A link whose text starts with "> " is marked in the menu: stserve offers a
// game sent from its catalogue in the browser that way, at the top of every
// list. The name on the card is the link's, without the mark. "USB> " is a
// game sent to the stick: it goes there whatever the dialog says.
static uint8_t marked[NET_MAX_ENTRIES];
// where a file without a mark goes: the SD card, or the USB stick
static bool to_stick = false;
static int name_count = 0;
static uint32_t bytes_done = 0, bytes_total = 0;
static int modem = MODEM_UNKNOWN;
static bool wifi_down = false;      // the modem reported no WiFi on the last ATI
static bool in_body = false;        // file data is streaming, keep it out of the tail
// The core's network UART: system value N selects 0=19200 1=38400 2=57600
// 3=115200 4=460800. Transfers run at the fastest one the core's port
// FIFOs allow: the ST's hold 255 bytes, enough for the few milliseconds
// the companion may be busy with the card while a block arrives. The
// C64's hold 15, filled in 0.3 ms at 460800 -- on the board one transfer
// in three was cancelled there, and at 115200, 1.3 ms, three in seven.
// With FIFOs that small the transfer runs at 38400: 3.9 ms for the
// companion to fetch them, a 30 KB program in 8 s.
#define NET_FAST_BAUD   "460800"
#define NET_FAST_N      4
#define NET_SMALL_BAUD  "38400"
#define NET_SMALL_N     1
#define NET_SMALL_FIFO  64        // free bytes of an empty FIFO below this: small

static char wifi_cfg[80];
static bool wifi_tried = false;      // the ini's network is offered once per boot

static bool fast = false;           // the port and the modem run at 115200 for a transfer

// ------------------------------------------------------------ plumbing ----

// the last characters that went over the port, for the OSD while this
// is being brought up: sent commands are marked with '>', line ends
// show as '|', anything unprintable as '.'
#define TAIL_LEN 48
static char tail[TAIL_LEN + 1];
static int tail_fill = 0;

static void tail_add(const unsigned char *data, int len) {
  for(int i=0;i<len;i++) {
    unsigned char c = data[i];
    if(c == '\r' || c == '\n') c = '|';
    else if(c < 32 || c > 126) c = '.';
    if(tail_fill == TAIL_LEN) {
      memmove(tail, tail+1, TAIL_LEN-1);
      tail_fill--;
    }
    tail[tail_fill++] = (char)c;
    tail[tail_fill] = '\0';
  }
}

const char *netdl_tail(void) { return tail; }

void netdl_port_bytes(const unsigned char *data, int len) {
  if(rx_stream && len > 0)
    xStreamBufferSend(rx_stream, data, len, 0);   // never block the com task
  if(state == NET_STATE_BUSY && !in_body)
    tail_add(data, len);   // commands and replies only, not the file body
}

static void net_write(const char *str, int len) {
  if(!in_body) {
    tail_add((const unsigned char*)">", 1);
    tail_add((const unsigned char*)str, len);
  }
  sys_port_write(NET_PORT, (const unsigned char*)str, len);
}

static void net_puts(const char *str) {
  net_write(str, strlen(str));
}

static void net_flush(void) {
  unsigned char b;
  while(xStreamBufferReceive(rx_stream, &b, 1, 0) == 1);
}

// read one byte, false on timeout
static bool net_getc(unsigned char *b, int timeout_ms) {
  return xStreamBufferReceive(rx_stream, b, 1, pdMS_TO_TICKS(timeout_ms)) == 1;
}

// read a line, without CR/LF. Empty lines are skipped. False on timeout.
static bool net_read_line(char *line, int max, int timeout_ms) {
  int len = 0;
  TickType_t start = xTaskGetTickCount();
  while(1) {
    int left = timeout_ms - (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    if(left <= 0) return false;

    unsigned char b;
    if(!net_getc(&b, left)) return false;

    if(b == '\r' || b == '\n') {
      if(len) { line[len] = '\0'; return true; }
    } else if(len < max-1)
      line[len++] = (char)b;
  }
}

// keep a record on the card: the OSD message is gone once the dialog is
// closed, this stays until it is read on a PC
// While the companion asks stserve for a game to play, every few seconds,
// nothing of it goes to NETLOG.TXT, the menu or the dialog's lines.
static bool net_quiet = false;

static void net_notify(void) {
  if(!net_quiet) menu_notify(MENU_EVENT_NET_UPDATE);
}

static void net_log(const char *line) {
  if(net_quiet) return;
  FIL f;
  sdc_lock();
  if(f_open(&f, CARD_MOUNTPOINT "/NETLOG.TXT", FA_OPEN_APPEND | FA_WRITE) == FR_OK) {
    f_puts(line, &f);
    f_puts("\r\n", &f);
    f_close(&f);
  }
  sdc_unlock();
}

static void net_set_message(const char *msg) {
  strncpy(message, msg, sizeof(message)-1);
  message[sizeof(message)-1] = '\0';
  debugf("NET: %s", message);
  net_notify();
}

// send a command and wait for a final reply line: true on "OK" or the
// expected prefix, false on "ERROR", "NO CARRIER" or timeout
static char modem_last[32];        // the modem's last reply line, for NETLOG

static bool modem_cmd(const char *cmd, const char *expect, int timeout_ms) {
  char line[NET_LINE_LEN];

  net_flush();
  net_puts(cmd);
  net_puts("\r");
  strcpy(modem_last, "nothing");

  while(net_read_line(line, sizeof(line), timeout_ms)) {
    debugf("NET: modem '%s'", line);
    if(!strcmp(line, cmd)) continue;        // the modem's echo of the command
    snprintf(modem_last, sizeof(modem_last), "%s", line);
    if(expect && !strncmp(line, expect, strlen(expect))) return true;
    if(!strcmp(line, "OK")) return true;
    // a bare ERROR only: Zimodem's "ERROR ON <ssid>" is a status line
    if(!strcmp(line, "ERROR") || !strncmp(line, "NO CARRIER", 10) ||
       !strcmp(line, "FAIL")) return false;
  }
  return false;
}

// ask the modem who it is: true on a final OK
// The receive path from the modem has been seen to go quiet while the
// modem kept sending, and only a reset of the port FIFOs in the core
// (Serial switched away from Netz and back) brought it back. Record what
// the core reports at that moment, then do the switch from here.
static void net_snapshot(const char *why) {
  unsigned char rx = 0, tx = 0;
  bool port = sys_port_status(NET_PORT, &rx, &tx);
  char line[96];
  snprintf(line, sizeof(line), "STAT %s: port %d rx %u tx %u irqs %lu bytes %lu events %lu src %02x",
           why, port, rx, tx, sys_stats.net_irqs, sys_stats.net_bytes, sys_stats.events, sys_stats.last_src);
  debugf("NET: %s", line);
  net_log(line);
}

/* ---------------------------------------------------------------------------
   Which menu entry routes the core's UART to the companion's own port

   Port 1 only exists while the core routes its serial pins to it. Which menu
   variable does that is the core's business, not ours: the id "E" is the
   serial port in the Atari ST core and the video standard in the C64 core,
   where writing it cold boots the machine. So the core declares the entry with
   role="netport" and we look it up - see config_find_role().

   No role means the running core offers no such port. Then the whole feature
   stays quiet rather than guessing.
   --------------------------------------------------------------------------- */

static bool netport_ok = false;      // the running core declares a netport
static unsigned char netport_id;     // the variable that selects it
static unsigned char netport_val;    // the value that routes it to port 1

// route the core's UART to our port
static void netport_select(void) {
  if(!netport_ok) return;
  sys_set_val(netport_id, netport_val);
}

// and give the user's own setting back
static void netport_restore(void) {
  if(!netport_ok) return;
  int v = menu_variable_get(netport_id);
  sys_set_val(netport_id, (v < 0) ? 0 : v);
}

static void net_port_reset(void) {
  if(!netport_ok) return;
  // Away from our port and back, so the core builds the FIFOs up again. Any
  // other value will do; the user's own setting is the obvious one, and a 0
  // if that happens to be our port already.
  int v = menu_variable_get(netport_id);
  unsigned char other = (v >= 0 && (unsigned char)v != netport_val) ? (unsigned char)v : 0;
  if(other == netport_val) other = netport_val ? 0 : 1;
  sys_set_val(netport_id, other);
  vTaskDelay(pdMS_TO_TICKS(50));
  sys_set_val(netport_id, netport_val);
  vTaskDelay(pdMS_TO_TICKS(50));
  net_flush();
  net_log("port reset");
}

static bool wifi_none = false;      // the modem has no network configured
static char modem_ip[20];           // from the modem's "CONNECTED TO <ssid> (<ip>)"
static char modem_version[16];      // from "Zimodem ESP32 Firmware v4.0.3", for the status
static char tz_cfg[12];             // time zone code for the modem's clock, from the ini
static bool time_set = false;       // the ST's clock has been set from the modem
// What the OSD tells about the line to the modem. Joining a network takes
// the modem the better part of half a minute after power up, and without a
// word about it the download menu looks broken for that while.
#define LINK_ASKING   0             // nothing asked yet
#define LINK_TRYING   1             // asking, or the modem is joining
#define LINK_UP       2             // in its network, address known
#define LINK_NO_WIFI  3             // answers, but is not in a network
#define LINK_NONE     4             // nothing answers on the port
static int link_state = LINK_ASKING;
static bool time_busy = false;      // asking the modem for the time right now
static int  clock_wait = 15;        // ini clockwait=: seconds the ST's start may wait for the clock
static void (*hold_release)(void) = NULL;   // set while the ST's start is held back

// Join the network named in the ini (wifi=Net,Password) and save it in
// the modem, so nobody needs a terminal program on the ST for the setup.
// Offered once per boot when the modem has no network or cannot reach it.
static bool modem_join_from_ini(void) {
  char line[NET_LINE_LEN];
  if(!wifi_cfg[0] || wifi_tried) return false;
  wifi_tried = true;

  net_set_message("Modem joining WiFi from ini...");
  net_flush();
  snprintf(line, sizeof(line), "ATW\"%s\"\r", wifi_cfg);
  net_puts(line);
  bool ok = false;
  while(net_read_line(line, sizeof(line), 30000)) {
    debugf("NET: ATW '%s'", line);
    if(!strcmp(line, "OK")) { ok = true; break; }
    if(!strcmp(line, "ERROR")) break;
  }
  if(ok) {
    net_flush();
    net_puts("AT&W\r");
    ok = modem_cmd("AT", NULL, 3000);   // AT&W answers OK, then the probe does
  }
  net_log(ok ? "wifi from ini: joined and saved" : "wifi from ini: join failed");
  return ok;
}

// Zimodem keeps the time by NTP. Ask for it in a fixed format and set the
// ST's clock with it, the way the companion does with its own NTP on
// boards that have a network. The time zone comes from the ini
// (timezone=CEST); Zimodem knows no daylight saving rules, so it is CET in
// winter and CEST in summer. Called once the modem has joined its network.
static bool modem_time_ask(void);

// The address is held back from the OSD while this runs, see netdl_get_ip():
// TOS looks at its clock chip once, when it starts, so "the address is
// there" has to mean "a reset now picks up the right time".
static bool modem_time_sync(void) {
  if(time_set || modem != MODEM_ZIMODEM) return time_set;
  time_busy = true;
  bool ok = modem_time_ask();
  time_busy = false;
  return ok;
}

// Ask for the time. With check_zone the answer only counts if the modem
// already tells it in my format and in the zone from the ini; *mine says
// whether that was so.
static bool modem_time_read(bool check_zone, bool *mine) {
  char line[NET_LINE_LEN];
  *mine = false;

  net_flush();
  net_puts("AT&T\r");
  while(net_read_line(line, sizeof(line), 2000)) {
    int y, mo, d, h, mi, s;
    char zone[12] = "";
    if(!strcmp(line, "OK") || !strcmp(line, "ERROR")) break;
    int n = sscanf(line, "%d-%d-%d %d:%d:%d %11s", &y, &mo, &d, &h, &mi, &s, zone);
    if(n >= 6) {
      debugf("NET: modem time '%s'", line);
      if(check_zone && (n < 7 || strcasecmp(zone, tz_cfg[0] ? tz_cfg : "utc"))) return false;
      *mine = true;
      if(y < 2024 || y > 2099) {                   // NTP has not answered yet
        net_log("clock: modem has no time yet");
        return false;
      }
      // day of the week, 0 = Sunday (Sakamoto)
      static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
      int yy = (mo < 3) ? y - 1 : y;
      int wday = (yy + yy/4 - yy/100 + yy/400 + t[mo-1] + d) % 7;
      sys_set_time(SYS_TIME_FLAGS_NTP, y - 1900, mo - 1, d + (wday << 5), h, mi, s);
      time_set = true;
      snprintf(line, sizeof(line), "clock set: %04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
      net_log(line);
      return true;
    }
  }
  return false;
}

static bool modem_time_ask(void) {
  char line[NET_LINE_LEN];
  bool mine;

  // The usual case: format and zone are stored in the modem from an earlier
  // run, so the time is right as soon as its first NTP answer is in.
  if(modem_time_read(true, &mine)) return true;
  if(mine) return false;               // no NTP answer yet, the caller asks again

  // First run with this modem, or a new zone in the ini: set format and
  // zone and store them in the modem.
  snprintf(line, sizeof(line), "AT&T\"%s,%%yyyy-%%MM-%%dd %%HH:%%mm:%%ss %%z,\"", tz_cfg);
  if(!modem_cmd(line, NULL, 2000)) { net_log("clock: modem refused time zone or format"); return false; }
  modem_cmd("AT&W", NULL, 3000);
  net_log("clock: time zone and format stored in the modem");

  // Zimodem applies a new time zone only when the next NTP answer comes
  // in, which it asks for right away. Asked too early it still tells the
  // old zone.
  vTaskDelay(pdMS_TO_TICKS(5000));
  return modem_time_read(false, &mine);
}

static void modem_tell_system(void);

static bool modem_ati(void) {
  char line[NET_LINE_LEN];
  wifi_none = false;

  net_flush();
  net_puts("AT\r");
  vTaskDelay(pdMS_TO_TICKS(300));
  net_flush();

  net_puts("ATI\r");
  modem = MODEM_UNKNOWN;
  wifi_down = false;
  while(net_read_line(line, sizeof(line), 3000)) {
    debugf("NET: ATI '%s'", line);
    if(strstr(line, "Zimodem") || strstr(line, "zimodem")) {
      modem = MODEM_ZIMODEM;
      const char *v = strstr(line, "Firmware v");
      if(v) {
        snprintf(modem_version, sizeof(modem_version), "%s", v + 10);
        modem_version[strcspn(modem_version, " \r\n")] = 0;
      }
    }
    if(strstr(line, "AT version") || strstr(line, "SDK version")) modem = MODEM_ESPAT;
    // Zimodem reports its WiFi as "CONNECTED TO <ssid> (<ip>)" or "ERROR ON <ssid>",
    // and "INITIALIZED" when it has no network at all
    if(!strncmp(line, "ERROR ON", 8)) { wifi_down = true; modem_ip[0] = 0; }
    if(!strncmp(line, "CONNECTED TO", 12)) {
      // keep the address in brackets for the OSD
      char *a = strrchr(line, '('), *b = strrchr(line, ')');
      if(a && b && b > a + 1 && (size_t)(b - a - 1) < sizeof(modem_ip)) {
        memcpy(modem_ip, a + 1, b - a - 1);
        modem_ip[b - a - 1] = 0;
      }
    }
    if(!strcmp(line, "INITIALIZED")) { wifi_down = true; wifi_none = true; }
    if(!strcmp(line, "OK")) { modem_tell_system(); return true; }
    if(!strcmp(line, "ERROR")) return false;
  }
  return false;
}

// Wait for the modem to be in its network, asking every two seconds.
// A join takes a few seconds and the modem answers all the while, so
// asking once and giving up turns a link that is about to come up into
// "Modem has no WiFi" - and the next try a minute later works, which is
// exactly the kind of thing that looks like bad luck.
static bool modem_wait_wifi(int seconds) {
  link_state = LINK_TRYING;
  for(int i = 0; i < seconds; i += 2) {
    if(!modem_ati()) return false;        // nothing on the port at all
    if(!wifi_down) return true;           // in
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  return modem_ati() && !wifi_down;
}

// Tell a Zimodem which machine it serves, for its status display: AT+SYS:
// and the core's name from its config xml. Once per power-up, on the first
// answer to ATI, whichever way that comes: the ST asks for the clock at
// start-up, the C64 first at a download. A modem without a display, or
// without the command, answers ERROR, and that is all right.
// a message for the display that waits for the first contact, see
// netdl_boot_message()
static char boot_msg[120];

static void modem_tell_system(void) {
  static bool told = false;
  if(told || modem != MODEM_ZIMODEM || !cfg || !cfg->name) return;
  told = true;
  char line[NET_LINE_LEN];
  snprintf(line, sizeof(line), "AT+SYS:%s", cfg->name);
  modem_cmd(line, NULL, 2000);
  // A message left from before the restart -- "Switching to C64" -- goes,
  // or the one that waited for this moment takes its place.
  snprintf(line, sizeof(line), "AT+MSG:%s", boot_msg);
  modem_cmd(line, NULL, 2000);
  boot_msg[0] = 0;
}

void netdl_boot_message(const char *msg) {
  size_t n = 0;
  for(; msg && *msg && n < sizeof(boot_msg) - 1; msg++)
    boot_msg[n++] = (*msg == ' ') ? '_' : *msg;
  boot_msg[n] = 0;
}

// find out what is on the other end of the wire
static bool modem_detect(void) {
  bool ok = modem_ati();

  if(ok && wifi_down && modem == MODEM_ZIMODEM && wifi_cfg[0] && !wifi_tried) {
    if(modem_join_from_ini())
      ok = modem_wait_wifi(12);
  }

  if(ok && wifi_down && modem == MODEM_ZIMODEM) {
    // Zimodem has lost its WiFi and would wait a while before trying
    // again. ATZ reloads its settings and joins right away.
    // One reset only and then patience: repeated resets interrupt the
    // join in progress and the link flaps instead of coming up.
    net_set_message("Modem joining WiFi...");
    net_flush();
    net_puts("ATZ\r");
    vTaskDelay(pdMS_TO_TICKS(15000));
    ok = modem_wait_wifi(12);
    if(!ok || wifi_down) {
      link_state = ok ? LINK_NO_WIFI : LINK_NONE;
      net_set_message("Modem has no WiFi");
      return false;
    }
  }

  if(!ok) {
    // no answer: first suspect the port itself, see net_snapshot()
    net_snapshot("no answer");
    net_port_reset();
    ok = modem_ati();
    net_snapshot(ok ? "answer after port reset" : "still no answer");
  }
  if(modem_ip[0])   link_state = LINK_UP;
  else if(!ok)      link_state = LINK_NONE;
  else if(wifi_down) link_state = LINK_NO_WIFI;

  if(!ok) {
    // no answer: the modem may still sit in a stream from an earlier
    // attempt that was cut short. Escape, hang up, ask again.
    debugf("NET: no answer, trying to leave a stream");
    vTaskDelay(pdMS_TO_TICKS(1100));
    net_puts("+++");
    vTaskDelay(pdMS_TO_TICKS(1100));
    net_puts("ATH\r");
    vTaskDelay(pdMS_TO_TICKS(500));
    net_puts("AT+CIPCLOSE\r");
    vTaskDelay(pdMS_TO_TICKS(500));
    ok = modem_ati();
  }

  if(!ok) {
    // still nothing: the modem may have been left at a fast rate when a
    // request was cut short (a reset from the OSD, for instance). Look
    // for it at the rates we use and bring it back to 19200.
    static const int8_t fast_n[] = { NET_FAST_N, 3, NET_SMALL_N };
    for(unsigned i = 0; i < sizeof(fast_n) && !ok; i++) {
      debugf("NET: no answer at 19200, looking at N=%d", fast_n[i]);
      sys_set_val('N', fast_n[i]);
      vTaskDelay(pdMS_TO_TICKS(50));
      net_flush();
      if(modem_cmd("AT", NULL, 1500)) {
        net_puts("ATB19200\r");
        vTaskDelay(pdMS_TO_TICKS(40));
      }
      sys_set_val('N', 0);
      vTaskDelay(pdMS_TO_TICKS(50));
      net_flush();
      fast = false;
      ok = modem_ati();
    }
  }

  if(!ok) { net_set_message("No modem on port 1"); return false; }
  if(modem == MODEM_UNKNOWN) modem = MODEM_ESPAT;   // OK without a banner
  debugf("NET: modem is %s", (modem == MODEM_ZIMODEM)?"Zimodem":"ESP-AT");

  return true;
}

// open a raw TCP stream to host:port
static bool modem_connect(const char *host, int port) {
  char cmd[NET_LINE_LEN];

  if(modem == MODEM_ZIMODEM) {
    // no 'T': a raw stream, no telnet negotiation
    snprintf(cmd, sizeof(cmd), "ATD%s:%d", host, port);
    if(!modem_cmd(cmd, "CONNECT", 15000)) return false;
    return true;
  }

  // ESP-AT
  if(!modem_cmd("AT+CIPMUX=0", NULL, 2000)) return false;
  if(!modem_cmd("AT+CIPMODE=1", NULL, 2000)) return false;
  snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", host, port);
  if(!modem_cmd(cmd, NULL, 15000)) return false;

  // enter the passthrough stream, the modem answers with '>'
  net_flush();
  net_puts("AT+CIPSEND\r");
  unsigned char b;
  TickType_t start = xTaskGetTickCount();
  while((xTaskGetTickCount() - start) < pdMS_TO_TICKS(3000)) {
    if(net_getc(&b, 100) && b == '>') return true;
  }
  return false;
}

// leave the stream and hang up
static void modem_hangup(void) {
  vTaskDelay(pdMS_TO_TICKS(1100));
  net_puts("+++");
  vTaskDelay(pdMS_TO_TICKS(1100));

  if(modem == MODEM_ZIMODEM) {
    modem_cmd("ATH", NULL, 3000);
  } else {
    modem_cmd("AT+CIPCLOSE", NULL, 3000);
    modem_cmd("AT+CIPMODE=0", NULL, 2000);
  }
  net_flush();
}

// -------------------------------------------------------------- HTTP ----

// split "host:port" from the ini, port defaults to 80
// "192.168.1.2:8888" or "192.168.1.2:8888/games" - the folder is where
// every list starts, so a server holding more than the ST's share can be
// used as well
static bool net_server_parse(char *host, int max, int *port, char *base, int bmax) {
  if(!server[0]) { net_set_message("No server in ini file"); return false; }
  strncpy(host, server, max-1);
  host[max-1] = '\0';
  base[0] = '\0';

  char *s = strchr(host, '/');
  if(s) {
    *s++ = '\0';
    if(*s) {
      int n = snprintf(base, bmax, "%s", s);
      if(n > 0 && n < bmax - 1 && base[n-1] != '/') { base[n] = '/'; base[n+1] = '\0'; }
    }
  }

  *port = 80;
  char *c = strchr(host, ':');
  if(c) { *c = '\0'; *port = atoi(c+1); }
  return host[0] != '\0';
}

// --------------------------------------------------------- bit rate ----
// The modem talks 19200 for the ST. For a file it is worth switching to
// a fast rate for the duration, see NET_FAST_BAUD: Zimodem changes its
// rate the moment it sees ATB and already answers at the new one, so the
// core's port follows right behind the command. Whatever happens, the
// request loop puts 19200 back at the end.

static bool net_baud_fast(void) {
  if(modem != MODEM_ZIMODEM) return false;
  net_flush();
  // the space of the empty FIFO towards the modem tells its size
  unsigned char rx = 0, tx = 0;
  bool small = sys_port_status(NET_PORT, &rx, &tx) && tx < NET_SMALL_FIFO;
  net_puts(small ? "ATB" NET_SMALL_BAUD "\r" : "ATB" NET_FAST_BAUD "\r");
  vTaskDelay(pdMS_TO_TICKS(80));         // the command must leave the port at 19200
  sys_set_val('N', small ? NET_SMALL_N : NET_FAST_N);
  vTaskDelay(pdMS_TO_TICKS(50));
  net_flush();
  fast = true;
  if(modem_cmd("AT", NULL, 1500)) return true;
  // no answer at 115200: go back and stay slow
  sys_set_val('N', 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  net_flush();
  fast = false;
  modem_cmd("AT", NULL, 1500);
  return false;
}

static void net_baud_slow(void) {
  if(!fast) return;
  net_flush();
  net_puts("\r");                       // the modem lost the first character right after a transfer
  vTaskDelay(pdMS_TO_TICKS(100));
  net_flush();
  net_puts("ATB19200\r");
  vTaskDelay(pdMS_TO_TICKS(40));
  sys_set_val('N', 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  net_flush();
  fast = false;
  modem_cmd("AT", NULL, 1500);
}

// ------------------------------------------------------------ XMODEM ----
// Zimodem fetches the resource itself ("ATGxmodem:<url>") and hands it
// over in 128 or 1024 byte blocks, each with a CRC and an acknowledge.
// A byte lost on the serial line costs one repeated block, not the file.

#define XM_SOH 0x01
#define XM_STX 0x02
#define XM_EOT 0x04
#define XM_ACK 0x06
#define XM_NAK 0x15
#define XM_CAN 0x18
#define XM_CRC 'C'

static uint16_t crc16_xmodem(const unsigned char *d, int len) {
  uint16_t crc = 0;
  for(int i=0;i<len;i++) {
    crc ^= (uint16_t)d[i] << 8;
    for(int b=0;b<8;b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

static void xm_put(unsigned char c) { net_write((const char*)&c, 1); }

// read exactly n bytes into buf, false on timeout
static bool xm_read(unsigned char *buf, int n, int timeout_ms) {
  TickType_t start = xTaskGetTickCount();
  int got = 0;
  while(got < n) {
    int left = timeout_ms - (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    if(left <= 0) return false;
    size_t r = xStreamBufferReceive(rx_stream, buf + got, n - got, pdMS_TO_TICKS(left));
    got += r;
  }
  return true;
}

// The outcome of the last fetch in words, for the two small lines of the
// download dialog: where it went, and what the modem said about it. Zimodem
// sends one line before XMODEM or ERROR, "FETCH <got> of <len> bytes in <ms>
// ms, <why>, <n> connections, rssi <dBm>"; its why and rssi are what the
// user needs to tell a weak WiFi from a server that is not there.
static char fetch_to[40];
static char fetch_result[32];
static int fetch_rssi = 0;
static bool fetch_have_rssi = false;

const char *netdl_fetch_to(void) { return fetch_to; }
const char *netdl_fetch_result(void) { return fetch_result; }

static void fetch_note(const char *what) {
  if(fetch_have_rssi) snprintf(fetch_result, sizeof(fetch_result), "%s %ddBm", what, fetch_rssi);
  else                snprintf(fetch_result, sizeof(fetch_result), "%s", what);
}

// "FETCH 0 of 1234 bytes in 812 ms, stalled, 3 connections, rssi -78"
static void fetch_line(const char *line) {
  static const char *shorter[][2] = {
    { "the server answered something else", "server answer odd" },
    { "read returned nothing", "read nothing" },
    { "closed by the server", "closed by server" },
    { "no room in psram", "no room in psram" } };
  char why[40] = "";
  const char *a = strstr(line, " ms, ");
  if(a) {
    a += 5;
    const char *b = strstr(a, ", ");
    size_t len = b ? (size_t)(b - a) : strlen(a);
    if(len >= sizeof(why)) len = sizeof(why) - 1;
    memcpy(why, a, len);
    why[len] = 0;
    for(unsigned i = 0; i < sizeof(shorter)/sizeof(shorter[0]); i++)
      if(!strcmp(why, shorter[i][0])) strcpy(why, shorter[i][1]);
  }
  const char *r = strstr(line, "rssi ");
  fetch_have_rssi = (r != NULL);
  if(r) fetch_rssi = atoi(r + 5);
  fetch_note(why[0] ? why : "modem fetch");
  net_log(line);
}

static bool xmodem_get(const char *host, int port, const char *path, bool (*sink)(const unsigned char*, int)) {
  char line[NET_LINE_LEN];
  static unsigned char blk[1024 + 4];

  snprintf(fetch_to, sizeof(fetch_to), "to %s:%d", host, port);
  fetch_have_rssi = false;
  fetch_note("fetching...");

  // the modem fetches the whole resource first, that takes a while
  net_set_message("Modem fetching...");
  // Room for a path a few folders deep. Zimodem takes 256 characters per
  // command; a longer one would be cut in the middle of the address and
  // fetch something else, so say so instead.
  char cmd[256];
  int n = snprintf(cmd, sizeof(cmd), "AT&G\"xmodem:http://%s:%d/%s\"", host, port, path);   // AT&G is the web get; quoted, unquoted text ends at the first letter
  if(n < 0 || n >= (int)sizeof(cmd)) { net_set_message("Path too long for the modem"); return false; }
  net_flush();
  net_puts(cmd);
  net_puts("\r");

  bytes_total = 0;
  bool announced = false, answered = false, told = false;
  // The modem takes the whole file in before it answers, and over a weak
  // WiFi link it may ask the server again a few times for the rest; it
  // gives up after 100 s (Zimodem, xFetchToMemory()). Wait for that.
  while(net_read_line(line, sizeof(line), 120000)) {
    debugf("NET: xmodem '%s'", line);
    answered = true;
    if(!strncmp(line, "FETCH ", 6)) { fetch_line(line); told = true; continue; }
    if(!strncmp(line, "XMODEM ", 7)) { bytes_total = strtoul(line + 7, NULL, 10); announced = true; break; }
    if(!strcmp(line, "ERROR") || !strncmp(line, "NO CARRIER", 10)) break;
  }
  if(!announced) {
    // FETCH has said why; without it, at least what the modem did
    if(!told) fetch_note(!answered ? "modem did not answer" :
                         !strcmp(line, "ERROR") ? "modem: ERROR, no reason" : "modem: no carrier");
    net_set_message("Modem could not fetch");
    return false;
  }

  snprintf(line, sizeof(line), "Receiving %lu bytes", (unsigned long)bytes_total);
  net_set_message(line);
  bytes_done = 0;
  in_body = true;

  unsigned char expect = 1;
  int errors = 0;
  bool ok = false;
  TickType_t last_report = xTaskGetTickCount();

  // ask for CRC mode until the first block header shows up
  net_flush();
  xm_put(XM_CRC);
  while(1) {
    unsigned char h;
    if(!xm_read(&h, 1, 3000)) {
      if(++errors > 10) { net_set_message("Modem does not send"); break; }
      if(errors == 2 || errors == 4) {
        snprintf(line, sizeof(line), "quiet %d at %lu", errors, (unsigned long)bytes_done);
        net_snapshot(line);
      }
      if(errors == 3) net_port_reset();   // the sender retries for a while, give it a clean port
      xm_put(bytes_done ? XM_NAK : XM_CRC);
      continue;
    }

    if(h == XM_EOT) { xm_put(XM_ACK); ok = true; break; }
    if(h == XM_CAN) { net_set_message("Modem cancelled"); break; }
    if(h != XM_SOH && h != XM_STX) continue;   // noise between blocks

    int size = (h == XM_SOH) ? 128 : 1024;
    if(!xm_read(blk, size + 4, 3000)) {        // number, ~number, data, crc
      if(++errors > 10) { net_set_message("Block timed out"); break; }
      net_flush();
      xm_put(XM_NAK);
      continue;
    }

    unsigned char num = blk[0];
    uint16_t crc = ((uint16_t)blk[size + 2] << 8) | blk[size + 3];
    if((unsigned char)(blk[1] ^ 0xff) != num || crc16_xmodem(blk + 2, size) != crc) {
      if(++errors > 10) { net_set_message("Too many bad blocks"); break; }
      net_flush();
      xm_put(XM_NAK);
      continue;
    }

    if(num == expect) {
      // the last block is padded, keep only what the file has
      uint32_t left = bytes_total - bytes_done;
      int take = (left < (uint32_t)size) ? (int)left : size;
      if(take > 0 && !sink(blk + 2, take)) break;   // sink set the message
      bytes_done += take;
      expect++;
      errors = 0;
    }
    // a repeated block (num == expect-1) is simply acknowledged again
    xm_put(XM_ACK);

    if((xTaskGetTickCount() - last_report) > pdMS_TO_TICKS(250)) {
      last_report = xTaskGetTickCount();
      net_notify();
    }
  }
  in_body = false;

  // the modem closes with OK, wait for it so the next command is not
  // swallowed by the transfer's tail
  while(net_read_line(line, sizeof(line), 3000)) if(!strcmp(line, "OK") || !strcmp(line, "ERROR")) break;
  net_flush();

  if(ok && bytes_done != bytes_total) { net_set_message("Short transfer"); ok = false; }
  if(ok) {
    snprintf(line, sizeof(line), "fetched %lu KB", (unsigned long)((bytes_total + 1023) / 1024));
    fetch_note(line);
  } else
    fetch_note(message);     // what broke off the transfer
  return ok;
}

// Fetch /path from the server. Every body byte goes to sink(); a sink
// returning false aborts. Returns true when the whole body arrived.
static bool http_get(const char *path, bool (*sink)(const unsigned char*, int)) {
  char host[NET_SERVER_LEN];
  int port;
  char line[NET_LINE_LEN];
  char base[NET_PATH_LEN], full[NET_PATH_LEN + NET_HREF_LEN];

  fetch_result[0] = 0;       // nothing old in the dialog
  if(!net_server_parse(host, sizeof(host), &port, base, sizeof(base))) return false;
  snprintf(full, sizeof(full), "%s%s", base, path);
  path = full;

  if(!modem_detect()) return false;

  // Zimodem has the block wise way, no raw stream needed
  if(modem == MODEM_ZIMODEM) {
    net_baud_fast();                     // best effort, the transfer works at 19200 too
    return xmodem_get(host, port, path, sink);
  }

  net_set_message("Connecting...");
  if(!modem_connect(host, port)) {
    net_set_message("Connect failed");
    net_flush();
    return false;
  }

  // Zimodem rearranges its serial side when it enters the stream, and
  // bytes that arrive during that moment are lost: requests came in as
  // "GET /DIS_A.ST" or "GET /ind.tt". Give it a moment before talking.
  vTaskDelay(pdMS_TO_TICKS(400));
  net_flush();

  // the request. HTTP/1.0 and Connection: close keep it simple: no
  // chunked encoding, and the server closes when it is done
  char req[256];
  int rn = snprintf(req, sizeof(req), "GET /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
  if(rn < 0 || rn >= (int)sizeof(req)) { net_set_message("Path too long"); return false; }
  net_puts(req);

  // The reply head: the status line and the header lines, each ending in
  // CR LF, closed by an empty line. Read it byte by byte here, every byte
  // after the empty line already belongs to the file.
  int status = 0;
  bytes_total = 0;
  bool head_done = false;
  {
    int len = 0, lines = 0;
    unsigned char b;
    while(net_getc(&b, 10000)) {
      if(b == '\r') continue;
      if(b != '\n') {
        if(len < NET_LINE_LEN-1) line[len++] = (char)b;
        continue;
      }
      line[len] = '\0';
      if(len == 0) {
        if(lines) { head_done = true; break; }   // the empty line after the head
        continue;                                // stray line ends before it
      }
      if(lines == 0) {
        if(strncmp(line, "HTTP/1.", 7)) break;    // not a web server at all
        status = atoi(line + 9);
        debugf("NET: status %d", status);
      } else if(!strncasecmp(line, "Content-Length:", 15))
        bytes_total = strtoul(line + 15, NULL, 10);
      lines++;
      len = 0;
    }
  }

  if(!head_done) {
    char msg[48];
    snprintf(msg, sizeof(msg), "No HTTP: %.30s", line);
    net_set_message(msg);
    modem_hangup();
    return false;
  }
  if(status != 200) {
    snprintf(line, sizeof(line), "Server says %d", status);
    net_set_message(line);
    modem_hangup();
    return false;
  }
  bool ok = true;

  // the body: until Content-Length is reached, or the stream falls silent
  bytes_done = 0;
  in_body = true;
  static unsigned char chunk[NET_FILE_CHUNK];
  int fill = 0;
  TickType_t last_report = xTaskGetTickCount();
  bool complete = false;
  while(1) {
    size_t want = sizeof(chunk) - fill;
    if(bytes_total && bytes_total - bytes_done < want) want = bytes_total - bytes_done;
    // a WiFi link may pause for a while, so be patient when the length is known
    size_t got = want ? xStreamBufferReceive(rx_stream, chunk + fill, want, pdMS_TO_TICKS(bytes_total ? 30000 : 3000)) : 0;
    if(got == 0 && want) {
      // silence: with a known length that is a broken transfer,
      // without one it is the end of the file
      complete = (bytes_total == 0);
      if(!complete) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Stream stalled @%lu of %lu", (unsigned long)bytes_done, (unsigned long)bytes_total);
        net_set_message(msg);
      }
      break;
    }
    fill += got;
    bytes_done += got;

    if(fill == sizeof(chunk) || (bytes_total && bytes_done == bytes_total)) {
      if(!sink(chunk, fill)) { ok = false; break; }
      fill = 0;
    }
    if(bytes_total && bytes_done == bytes_total) { complete = true; break; }

    if((xTaskGetTickCount() - last_report) > pdMS_TO_TICKS(250)) {
      last_report = xTaskGetTickCount();
      net_notify();
    }
  }
  if(ok && complete && fill && !sink(chunk, fill)) ok = false;
  in_body = false;

  modem_hangup();
  return ok && complete;
}

// ------------------------------------------------------------ requests ----

// the server's answer to "/": a directory listing as HTML from python's
// http.server, or a plain text list if someone serves one instead
static char index_text[4096];
static int index_len;

static bool index_sink(const unsigned char *data, int len) {
  int room = sizeof(index_text) - 1 - index_len;
  if(len > room) len = room;
  memcpy(index_text + index_len, data, len);
  index_len += len;
  return true;
}

// "%20" and friends back into characters, for the name on the card
static void url_decode(const char *in, char *out, int max) {
  int o = 0;
  for(const char *p = in; *p && o < max-1; p++) {
    if(*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      char hex[3] = { p[1], p[2], 0 };
      out[o++] = (char)strtol(hex, NULL, 16);
      p += 2;
    } else
      out[o++] = *p;
  }
  out[o] = '\0';
}

// One link of the listing. Folders keep their slash and are shown with
// it, so the list tells them apart at a glance and download() knows what
// to do with them; ".." is the way back up.
static void add_entry(const char *href, uint8_t mark) {
  if(name_count >= NET_MAX_ENTRIES) return;
  int hl = strlen(href);
  if(hl == 0 || hl >= NET_HREF_LEN) return;
  if(strchr(href, '?')) return;                 // a view for browsers

  bool dir = (href[hl-1] == '/');
  bool up = !strcmp(href, "../");
  // a slash anywhere but at the end leads out of this folder
  if(!up && strchr(href, '/') != (dir ? href + hl - 1 : NULL)) return;
  if(up && !cwd[0]) return;                     // already at the top

  char name[NET_NAME_LEN];
  url_decode(up ? ".." : href, name, sizeof(name));
  if(!name[0]) return;
  strcpy(hrefs[name_count], href);
  strcpy(names[name_count], name);
  marked[name_count] = mark;
  name_count++;
}

static void fetch_list(void) {
  state = NET_STATE_BUSY;
  name_count = 0;
  index_len = 0;
  net_set_message("Fetching list...");

  // the folder itself; python's http.server answers with a listing
  if(!http_get(cwd, index_sink)) {
    state = NET_STATE_ERROR;
    char line[80];
    snprintf(line, sizeof(line), "FAIL list: %s", message);
    net_log(line);
    net_notify();
    return;
  }
  index_text[index_len] = '\0';

  if(strstr(index_text, "href=")) {
    // HTML: every <a href="name"> is a file, directories end with a slash
    char *p = index_text;
    while((p = strstr(p, "href=\"")) != NULL && name_count < NET_MAX_ENTRIES) {
      p += 6;
      char *e = strchr(p, '"');
      if(!e) break;
      *e = '\0';
      // the link's text follows the closing quote of the href
      char *text = strchr(e + 1, '>');
      uint8_t mark = NET_MARK_NONE;
      if(text && !strncmp(text + 1, "&gt; ", 5))    mark = NET_MARK_CARD;
      if(text && !strncmp(text + 1, "USB&gt; ", 8)) mark = NET_MARK_STICK;
      add_entry(p, mark);
      p = e + 1;
    }
  } else {
    // plain text: one file name per line, comments and blanks skipped
    char *p = index_text;
    while(*p && name_count < NET_MAX_ENTRIES) {
      char *e = p;
      while(*e && *e != '\n') e++;
      char *end = e;
      if(*e) e++;
      while(end > p && (end[-1] == '\r' || end[-1] == ' ')) end--;
      *end = '\0';
      if(*p && *p != ';' && *p != '#') add_entry(p, NET_MARK_NONE);
      p = e;
    }
  }

  if(!name_count) {
    net_set_message("List is empty");
    state = NET_STATE_ERROR;
  } else {
    snprintf(message, sizeof(message), "%d files", name_count);
    state = NET_STATE_LIST;
  }
  net_log(message);
  net_notify();
}

static FIL dl_file;
static bool dl_open;
static char last_file[NET_NAME_LEN];   // the last file that arrived whole, for the status
// The download goes to the USB stick. The stick hangs on the companion
// itself: no core reads it, so it needs no sdc_lock(), which would only
// hold the core's floppy up.
static bool dl_usb;

static void dl_lock(void)   { if(!dl_usb) sdc_lock(); }
static void dl_unlock(void) { if(!dl_usb) sdc_unlock(); }

static bool file_sink(const unsigned char *data, int len) {
  UINT bw = 0;
  dl_lock();
  FRESULT r = f_write(&dl_file, data, len, &bw);
  dl_unlock();
  if(r != FR_OK || bw != (UINT)len) {
    // f_write's result, the bytes it managed, the sector layer's reason
    // (1/2 read busy/timeout, 3/4 write busy/timeout) and the offset
    char msg[48];
    if(dl_usb) snprintf(msg, sizeof(msg), "USB write f%d w%u @%lu", r, bw, (unsigned long)bytes_done);
    else       snprintf(msg, sizeof(msg), "SD write f%d w%u e%d @%lu", r, bw, sdc_get_last_error(), (unsigned long)bytes_done);
    net_set_message(msg);
    return false;
  }

  // commit size and cluster chain every 64 KB, so a transfer that breaks
  // off later leaves a file that is at least consistent up to here
  if((bytes_done & 0xffff) < (uint32_t)len) {
    dl_lock();
    r = f_sync(&dl_file);
    dl_unlock();
    if(r != FR_OK) {
      char msg[48];
      snprintf(msg, sizeof(msg), "%s sync f%d e%d @%lu", dl_usb ? "USB" : "SD", r, sdc_get_last_error(), (unsigned long)bytes_done);
      net_set_message(msg);
      return false;
    }
  }
  return true;
}

// Up one level: cut the last part off the path.
static void cwd_up(void) {
  int n = strlen(cwd);
  if(n) n--;                                    // the trailing slash
  while(n > 0 && cwd[n-1] != '/') n--;
  cwd[n] = '\0';
}

// One file from the server, url as the server wants it, onto the card or
// the stick under name. mark is the catalogue's mark, if any. True when it
// arrived whole.
static bool download_file(const char *name, const char *url, uint8_t mark) {
  // the card or the stick: a game sent to one of them from the catalogue
  // goes there, any other file where the dialog says
  dl_usb = (mark == NET_MARK_STICK) || (mark == NET_MARK_NONE && to_stick);
  if(dl_usb && !mcu_hw_usb_msc_present()) {
    net_set_message("No USB stick on the hub");
    state = NET_STATE_ERROR;
    net_notify();
    return false;
  }
  const char *base = dl_usb ? "/usb" : CARD_MOUNTPOINT;

  state = NET_STATE_BUSY;
  bytes_done = bytes_total = 0;
  snprintf(message, sizeof(message), "Loading %s%s", name, dl_usb ? " to USB" : "");
  net_notify();

  char path[NET_NAME_LEN + 8];
  snprintf(path, sizeof(path), "%s/%s", base, name);
  // written under a temporary name and renamed at the end, so a copy
  // already there survives a failed attempt
  char tmp_path[16];
  snprintf(tmp_path, sizeof(tmp_path), "%s/NETDL.TMP", base);

  dl_lock();
  f_unlink(tmp_path);
  FRESULT r = f_open(&dl_file, tmp_path, FA_CREATE_ALWAYS | FA_WRITE);
  dl_unlock();
  if(r != FR_OK) {
    char msg[48];
    if(dl_usb) snprintf(msg, sizeof(msg), "Cannot create on USB f%d", r);
    else       snprintf(msg, sizeof(msg), "Cannot create f%d e%d", r, sdc_get_last_error());
    net_set_message(msg);
    state = NET_STATE_ERROR;
    net_notify();
    return false;
  }
  dl_open = true;

  bool ok = http_get(url, file_sink);            // the URL form, spaces and all encoded

  dl_lock();
  FRESULT cr = f_close(&dl_file);
  if(cr != FR_OK) {
    // the size is written on close: without it the file is empty
    snprintf(message, sizeof(message), "%s close f%d e%d", dl_usb ? "USB" : "SD", cr, sdc_get_last_error());
    ok = false;
  }
  if(ok) {
    f_unlink(path);            // replace an older copy
    FRESULT rr = f_rename(tmp_path, path);
    if(rr != FR_OK) {
      snprintf(message, sizeof(message), "%s rename f%d e%d", dl_usb ? "USB" : "SD", rr, sdc_get_last_error());
      ok = false;
    }
  }
  if(!ok) f_unlink(tmp_path);  // no half files on the card or the stick
  dl_unlock();
  dl_open = false;

  if(ok) {
    snprintf(last_file, sizeof(last_file), "%s", name);
    snprintf(message, sizeof(message), "%s: %lu bytes%s", name, (unsigned long)bytes_done, dl_usb ? " on USB" : "");
    state = NET_STATE_DONE;
  } else
    state = NET_STATE_ERROR;

  {
    char line[96];
    snprintf(line, sizeof(line), "%s %s%s (%lu/%lu) %s", ok ? "OK  " : "FAIL", name, dl_usb ? " -> USB" : "",
             (unsigned long)bytes_done, (unsigned long)bytes_total, message);
    net_log(line);
  }
  net_notify();
  return ok;
}

static void download(int index) {
  if(index < 0 || index >= name_count) return;
  const char *name = names[index];

  // a folder: show what is in it, and go back to where we were if that
  // folder cannot be read
  int nl = strlen(name);
  if(!strcmp(name, "..") || (nl && name[nl-1] == '/')) {
    char was[NET_PATH_LEN];
    strcpy(was, cwd);
    if(!strcmp(name, ".."))
      cwd_up();
    else if(strlen(cwd) + strlen(hrefs[index]) < sizeof(cwd))
      strcat(cwd, hrefs[index]);
    else
      { net_set_message("Folder path too long"); return; }
    fetch_list();
    if(state == NET_STATE_ERROR) strcpy(cwd, was);
    return;
  }

  char url[NET_PATH_LEN + NET_HREF_LEN];
  snprintf(url, sizeof(url), "%s%s", cwd, hrefs[index]);
  download_file(name, url, marked[index]);
}

/* ---- "Play" in stserve's catalogue: one click, and the game runs ----
   With play= in the ini the companion asks the server every so many
   seconds whether a game is waiting, at /_/job. stserve answers "NONE", or
   "PLAY <name>" and a "DISK <name>" line for each further disk of a set.
   The companion fetches them onto the card, as a game sent from the
   catalogue, puts the first one into the drive that takes its kind of
   file -- the core's own menu says which -- and starts the machine afresh.
   A C64 disk gets LOAD"*",8,1 typed, and RUN once the drive is quiet.

   Off unless the ini asks for it: every question takes the modem from the
   core for a second or two, and a terminal program on the C64 or the ST
   would lose its connection to that. */
#define PLAY_MAX 4
static int play_secs = 0;               // ini play=, 0 = off
static char play_names[PLAY_MAX][NET_NAME_LEN];
static char play_hrefs[PLAY_MAX][NET_HREF_LEN];
static int play_count = 0;
static char job_text[512];
static int job_len;

static bool job_sink(const unsigned char *data, int len) {
  int room = sizeof(job_text) - 1 - job_len;
  if(len > room) len = room;
  memcpy(job_text + job_len, data, len);
  job_len += len;
  return true;
}

/* ---- the device status, for stserve's catalogue in the browser ----
   With every question for a game to play, once after start-up and after
   every download the companion tells the server what it is: the core, its
   own commit and board, the modem's firmware and address, the WiFi signal
   at the last fetch, the image in the first drive and the last file that
   arrived. As a query on the request, kept short: Zimodem takes 256
   characters per command, the address and the quotes included. */
#ifndef COMPANION_VERSION
#define COMPANION_VERSION "unknown"
#endif
#if defined(TANG_NANO20K_V3923)
#define COMPANION_BOARD "3923"
#elif defined(TANG_NANO20K)
#define COMPANION_BOARD "3921"
#else
#define COMPANION_BOARD "other"
#endif

// s into out as a query value, at most max characters of out
static void query_add(char *out, size_t size, const char *key, const char *s, size_t max) {
  size_t o = strlen(out);
  if(!s || !*s || o + strlen(key) + 3 >= size) return;
  o += snprintf(out + o, size - o, "%s%s=", o ? "&" : "", key);
  size_t start = o;
  for(; *s && o + 4 < size && o - start + 3 <= max; s++) {
    unsigned char ch = (unsigned char)*s;
    if(isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out[o++] = ch;
    else o += snprintf(out + o, size - o, "%%%02X", ch);
  }
  out[o] = 0;
}

static void status_query(char *q, size_t size) {
  char num[16];
  q[0] = 0;
  query_add(q, size, "c", cfg ? cfg->name : NULL, 16);
  query_add(q, size, "v", COMPANION_VERSION, 12);
  query_add(q, size, "b", COMPANION_BOARD, 6);
  query_add(q, size, "m", modem_version, 12);
  query_add(q, size, "i", modem_ip, 16);
  if(fetch_have_rssi) { snprintf(num, sizeof(num), "%d", fetch_rssi); query_add(q, size, "r", num, 5); }
  snprintf(num, sizeof(num), "%lu", (unsigned long)(xTaskGetTickCount() / pdMS_TO_TICKS(60000)));
  query_add(q, size, "u", num, 6);
  snprintf(num, sizeof(num), "%d", play_secs);
  query_add(q, size, "p", num, 4);
  const char *img = sdc_get_image_name(0);
  if(img) {
    const char *b = strrchr(img, '/');
    query_add(q, size, "d", b ? b + 1 : img, 36);
  }
  query_add(q, size, "l", last_file, 36);
}

// The status alone, for when no question for a game goes out: quiet,
// and whatever the server answers is of no interest. A server that is not
// stserve answers 404, which is fine too.
static void status_report(void) {
  char path[200];
  char msg[sizeof(message)], to[sizeof(fetch_to)], res[sizeof(fetch_result)];
  int rssi = fetch_rssi;
  bool have = fetch_have_rssi;
  strcpy(msg, message); strcpy(to, fetch_to); strcpy(res, fetch_result);
  strcpy(path, "_/hello?");
  status_query(path + 8, sizeof(path) - 8);
  job_len = 0;
  net_quiet = true;
  http_get(path, job_sink);
  net_quiet = false;
  strcpy(message, msg); strcpy(fetch_to, to); strcpy(fetch_result, res);
  fetch_rssi = rssi; fetch_have_rssi = have;
}

// a name that can only be a file in the card's root
static bool play_name_ok(const char *name) {
  return name[0] && name[0] != '.' && !strpbrk(name, "/\\:*?\"<>|");
}

static bool card_job_parse(char *line);
static void card_jobs_run(void);
static int card_job_count;        // see the SD card from the browser, below
static char play_why[48];         // why the last question failed, for NETLOG

// Ask the server, quietly. True when a game is waiting; its disks are in
// play_names[] then. *answered tells a server that said "NONE" from one
// that could not be asked.
static bool play_poll(bool *answered) {
  char msg[sizeof(message)], to[sizeof(fetch_to)], res[sizeof(fetch_result)];
  int rssi = fetch_rssi;
  bool have = fetch_have_rssi;
  strcpy(msg, message); strcpy(to, fetch_to); strcpy(res, fetch_result);

  char path[200];
  strcpy(path, "_/job?");
  status_query(path + 6, sizeof(path) - 6);
  job_len = 0;
  net_quiet = true;
  bool ok = http_get(path, job_sink);
  net_quiet = false;
  if(!ok) snprintf(play_why, sizeof(play_why), "%s", message);

  strcpy(message, msg); strcpy(fetch_to, to); strcpy(fetch_result, res);
  fetch_rssi = rssi; fetch_have_rssi = have;

  *answered = ok;
  play_count = 0;
  card_job_count = 0;
  if(!ok) return false;
  job_text[job_len] = 0;

  for(char *line = strtok(job_text, "\r\n"); line && play_count < PLAY_MAX; line = strtok(NULL, "\r\n")) {
    const char *href = NULL;
    if(card_job_parse(line)) continue;
    if(!strncmp(line, "PLAY ", 5) && play_count == 0) href = line + 5;
    if(!strncmp(line, "DISK ", 5) && play_count > 0)  href = line + 5;
    if(!href || strlen(href) >= NET_HREF_LEN) continue;
    url_decode(href, play_names[play_count], NET_NAME_LEN);
    if(!play_name_ok(play_names[play_count])) continue;
    strcpy(play_hrefs[play_count], href);
    play_count++;
  }
  return play_count > 0;
}

/* ---- the SD card from the browser: stserve's "SD card" page ----
   The answer to /_/job may carry jobs for the card besides a game to play,
   one per line, names URL encoded, all in the card's root:
     LIST                 the list of files goes to the server
     READ <name>          a file goes to the server, the last 16 KB of a
                          longer one -- an ini to edit, NETLOG.TXT to read
     DEL <name>           delete it
     REN <name> <new>     rename it
     FILE <href> <name>   fetch href from the server onto the card as name:
                          a file uploaded in the browser, an edited ini
   Every job ends with a fresh list, with how each went. The way back to
   the server is the modem's fetch as well, see card_put(). An image a
   drive has open is not touched. */
#define CARD_JOBS 8
#define CARD_POST_MAX (16 * 1024)     // the list
#define CARD_TEXT_MAX 4096            // a file, the end of a longer one
typedef struct {
  char op;                        // 'L', 'R', 'D', 'N' (rename), 'F'
  char a[NET_NAME_LEN], b[NET_NAME_LEN];
  char href[NET_HREF_LEN];
} card_job_t;
static card_job_t card_jobs[CARD_JOBS];
static int card_job_count = 0;
static char card_results[400];    // how the jobs went, sent with the list

static void card_result(const char *fmt, const char *a, const char *b) {
  size_t n = strlen(card_results);
  if(n < sizeof(card_results) - 2)
    snprintf(card_results + n, sizeof(card_results) - n, fmt, a, b ? b : "");
}

// one job line of the answer to /_/job; false if it is none
static bool card_job_parse(char *line) {
  if(card_job_count >= CARD_JOBS) return false;
  card_job_t *j = &card_jobs[card_job_count];
  memset(j, 0, sizeof(*j));
  char *arg = strchr(line, ' ');
  if(arg) *arg++ = 0;
  char *arg2 = arg ? strchr(arg, ' ') : NULL;
  if(arg2) *arg2++ = 0;
  if(!strcmp(line, "LIST")) j->op = 'L';
  else if(!strcmp(line, "READ") && arg) j->op = 'R';
  else if(!strcmp(line, "DEL") && arg) j->op = 'D';
  else if(!strcmp(line, "REN") && arg && arg2) j->op = 'N';
  else if(!strcmp(line, "FILE") && arg && arg2) j->op = 'F';
  else return false;
  if(j->op == 'F') {
    if(strlen(arg) >= NET_HREF_LEN) return false;
    strcpy(j->href, arg);
    url_decode(arg2, j->a, NET_NAME_LEN);
  } else if(arg) {
    url_decode(arg, j->a, NET_NAME_LEN);
    if(arg2) url_decode(arg2, j->b, NET_NAME_LEN);
  }
  if(j->op != 'L' && !play_name_ok(j->a)) return false;
  if(j->op == 'N' && !play_name_ok(j->b)) return false;
  card_job_count++;
  return true;
}

// an image one of the drives has open
static bool card_in_use(const char *name) {
  for(int d = 0; d < MAX_DRIVES; d++) {
    const char *n = sdc_get_image_name(d);
    if(!n) continue;
    const char *b = strrchr(n, '/');
    if(!strcasecmp(b ? b + 1 : n, name)) return true;
  }
  return false;
}

// Data to the server, in pieces: _/card/put?<query>&s=<piece>&d=<data>,
// &e=1 on the last. Zimodem fetches, it sends nothing but a request line,
// and a POST over a plain connection through it (ATD, the bytes, +++)
// left it answering nothing at all on the board, for minutes, until it
// was switched off. A fetch is what works, so the data goes in the
// address: as much of it as Zimodem's 256 characters a command leave,
// about a hundred bytes a fetch. stserve puts the pieces together and
// takes a piece it already has once more without harm, so a failed
// fetch is simply repeated. True when every piece came through; else
// post_why says what went wrong, for NETLOG.
static char post_why[64];
static char put_reply[24];
static int put_len;

static bool put_sink(const unsigned char *data, int len) {
  int room = (int)sizeof(put_reply) - 1 - put_len;
  if(len > room) len = room;
  memcpy(put_reply + put_len, data, len);
  put_len += len;
  put_reply[put_len] = 0;
  return true;
}

// left as it is in the address; a space goes as '+'
static bool put_plain(unsigned char c) {
  return isalnum(c) || (c && strchr("-_.~!*'(),:;/@$", c));
}

static bool card_put(const char *query, const char *data, int len) {
  char host[NET_SERVER_LEN];
  int port;
  char base[NET_PATH_LEN];
  strcpy(post_why, "no server in the ini");
  if(!net_server_parse(host, sizeof(host), &port, base, sizeof(base))) return false;

  // quietly, as the questions for a game: no FETCH lines in NETLOG, the
  // dialog's lines as they were
  char msg[sizeof(message)], to[sizeof(fetch_to)], res[sizeof(fetch_result)];
  int rssi = fetch_rssi;
  bool have = fetch_have_rssi;
  strcpy(msg, message); strcpy(to, fetch_to); strcpy(res, fetch_result);
  net_quiet = true;

  bool ok = false;
  strcpy(post_why, "no modem");
  if(modem_detect()) {
    net_baud_fast();
    // what the command takes besides the path: AT&G"xmodem:http://host:port/"
    int fixed = snprintf(NULL, 0, "AT&G\"xmodem:http://%s:%d/\"", host, port);
    static char path[256];
    int o = 0, piece = 0;
    ok = true;
    do {
      int n = snprintf(path, sizeof(path), "%s_/card/put?%s&s=%d&d=", base, query, piece);
      int room = 255 - fixed - n - 4;                   // and "&e=1"
      if(n >= (int)sizeof(path) || room < 16) { strcpy(post_why, "the address is too long"); ok = false; break; }
      int take = 0;
      while(o + take < len) {
        unsigned char c = (unsigned char)data[o + take];
        int need = (c == ' ' || put_plain(c)) ? 1 : 3;
        if(need > room) break;
        if(c == ' ')     path[n++] = '+';
        else if(need == 1) path[n++] = (char)c;
        else             n += sprintf(path + n, "%%%02X", c);
        room -= need;
        take++;
      }
      if(o + take == len) n += sprintf(path + n, "&e=1");
      path[n] = 0;
      bool got = false;
      for(int tries = 0; tries < 3 && !got; tries++) {
        put_len = 0;
        put_reply[0] = 0;
        got = xmodem_get(host, port, path, put_sink) && !strncmp(put_reply, "OK", 2);
      }
      if(!got) {
        snprintf(post_why, sizeof(post_why), "piece %d of %d: %.30s", piece + 1,
                 len / 100 + 1, put_reply[0] ? put_reply : message);
        char *nl = strpbrk(post_why, "\r\n");
        if(nl) *nl = 0;
        ok = false;
        break;
      }
      o += take;
      piece++;
    } while(o < len);
  }

  net_quiet = false;
  strcpy(message, msg); strcpy(fetch_to, to); strcpy(fetch_result, res);
  fetch_rssi = rssi; fetch_have_rssi = have;
  return ok;
}

// the root of the card, one line a file: name, size, D for a folder,
// date and time; the results of the jobs first, "! ..." lines
static void card_send_list(void) {
  char *buf = pvPortMalloc(CARD_POST_MAX);
  if(!buf) return;
  int n = 0;
  for(char *r = strtok(card_results, "\n"); r; r = strtok(NULL, "\n"))
    n += snprintf(buf + n, CARD_POST_MAX - n, "! %s\n", r);
  card_results[0] = 0;
  DIR dir;
  FILINFO fno;
  int files = 0;
  bool full = false;
  sdc_lock();
  if(f_opendir(&dir, CARD_MOUNTPOINT) == FR_OK) {
    while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
      if(n > CARD_POST_MAX - 300) { full = true; break; }
      n += snprintf(buf + n, CARD_POST_MAX - n, "%s\t%lu\t%s\t%04u-%02u-%02u %02u:%02u\n",
                    fno.fname, (unsigned long)fno.fsize, (fno.fattrib & AM_DIR) ? "D" : "F",
                    ((fno.fdate >> 9) & 0x7f) + 1980, (fno.fdate >> 5) & 0x0f, fno.fdate & 0x1f,
                    (fno.ftime >> 11) & 0x1f, (fno.ftime >> 5) & 0x3f);
      files++;
    }
    f_closedir(&dir);
  }
  sdc_unlock();
  if(full) n += snprintf(buf + n, CARD_POST_MAX - n, "! the list ends here, the card has more\n");
  char q[64];
  snprintf(q, sizeof(q), "i=%s&k=list&n=%d", modem_ip, files);
  bool ok = card_put(q, buf, n);
  vPortFree(buf);
  char l[128];
  snprintf(l, sizeof(l), "card: list of %d sent%s%s", files, ok ? "" : ", FAILED: ", ok ? "" : post_why);
  net_log(l);
}

// a file to the server, the last CARD_TEXT_MAX bytes of a longer one
static void card_send_file(const char *name) {
  char full[NET_NAME_LEN + 8];
  snprintf(full, sizeof(full), "%s/%s", CARD_MOUNTPOINT, name);
  char *buf = pvPortMalloc(CARD_TEXT_MAX);
  if(!buf) { card_result("READ %s: out of memory%s\n", name, NULL); return; }
  FIL f;
  UINT got = 0;
  FSIZE_t size = 0, from = 0;
  sdc_lock();
  FRESULT r = f_open(&f, full, FA_OPEN_EXISTING | FA_READ);
  if(r == FR_OK) {
    size = f_size(&f);
    from = (size > CARD_TEXT_MAX) ? size - CARD_TEXT_MAX : 0;
    if(from) f_lseek(&f, from);
    r = f_read(&f, buf, CARD_TEXT_MAX, &got);
    f_close(&f);
  }
  sdc_unlock();
  if(r != FR_OK) {
    char e[8]; snprintf(e, sizeof(e), "f%d", r);
    card_result("READ %s: %s\n", name, e);
    vPortFree(buf);
    return;
  }
  char path[200], q[100];
  q[0] = 0;
  query_add(q, sizeof(q), "name", name, 60);
  snprintf(path, sizeof(path), "i=%s&k=text&size=%lu&from=%lu&%s", modem_ip,
           (unsigned long)size, (unsigned long)from, q);
  bool ok = card_put(path, buf, (int)got);
  vPortFree(buf);
  card_result(ok ? "READ %s: sent%s\n" : "READ %s: sending failed, %s\n", name, ok ? NULL : post_why);
}

// The jobs of the last answer, in order, then the list.
static void card_jobs_run(void) {
  if(!card_job_count) return;
  char full[NET_NAME_LEN + 8], to[NET_NAME_LEN + 8], e[8];
  for(int i = 0; i < card_job_count; i++) {
    card_job_t *j = &card_jobs[i];
    snprintf(full, sizeof(full), "%s/%s", CARD_MOUNTPOINT, j->a);
    FRESULT r = FR_OK;
    switch(j->op) {
    case 'R':
      card_send_file(j->a);
      break;
    case 'D':
      if(card_in_use(j->a)) { card_result("DEL %s: in a drive, not deleted%s\n", j->a, NULL); break; }
      sdc_lock(); r = f_unlink(full); sdc_unlock();
      snprintf(e, sizeof(e), "f%d", r);
      card_result("DEL %s: %s\n", j->a, r == FR_OK ? "deleted" : e);
      break;
    case 'N':
      if(card_in_use(j->a)) { card_result("REN %s: in a drive, not renamed%s\n", j->a, NULL); break; }
      snprintf(to, sizeof(to), "%s/%s", CARD_MOUNTPOINT, j->b);
      sdc_lock(); r = f_rename(full, to); sdc_unlock();
      snprintf(e, sizeof(e), "f%d", r);
      card_result("REN %s: %s\n", j->a, r == FR_OK ? j->b : r == FR_EXIST ? "the new name is taken" : e);
      break;
    case 'F':
      if(card_in_use(j->a)) { card_result("FILE %s: in a drive, not replaced%s\n", j->a, NULL); break; }
      card_result(download_file(j->a, j->href, NET_MARK_CARD) ? "FILE %s: on the card%s\n"
                                                               : "FILE %s: failed%s\n", j->a, NULL);
      break;
    }
    char l[NET_NAME_LEN + 16];
    snprintf(l, sizeof(l), "card: %s%s%s", j->op == 'L' ? "list asked" : j->op == 'R' ? "READ" :
             j->op == 'D' ? "DEL" : j->op == 'N' ? "REN" : "FILE", j->a[0] ? " " : "", j->a);
    net_log(l);
  }
  card_job_count = 0;
  card_send_list();
}

// the drive the core's menu offers for this kind of file, -1 if none
static int play_drive(config_menu_t *menu, const char *ext) {
  if(!menu) return -1;
  for(config_menu_entry_t *me = menu->entries; me; me = me->next) {
    if(me->type == CONFIG_MENU_ENTRY_MENU) {
      int d = play_drive(me->menu, ext);
      if(d >= 0) return d;
    }
    if(me->type == CONFIG_MENU_ENTRY_FILESELECTOR && me->fsel && me->fsel->ext &&
       me->fsel->index >= 0 && me->fsel->index < MAX_DRIVES)
      for(char **x = me->fsel->ext; *x; x++)
        if(!strcasecmp(*x, ext)) return me->fsel->index;
  }
  return -1;
}

// One key into the core, as the US keyboard the C64 core expects: shift
// held for the C64's ", its * on a key of its own.
static void play_key(uint8_t usage, bool shift) {
  if(shift) { kbd_tx_hid_ps2_make(1, 1); vTaskDelay(pdMS_TO_TICKS(30)); }
  kbd_tx_hid_ps2_make(usage, 0);
  vTaskDelay(pdMS_TO_TICKS(80));
  kbd_tx_hid_ps2_break(usage, 0);
  if(shift) { vTaskDelay(pdMS_TO_TICKS(30)); kbd_tx_hid_ps2_break(1, 1); }
  vTaskDelay(pdMS_TO_TICKS(80));
}

static void play_type(const char *s) {
  for(; *s; s++) {
    char ch = *s;
    if(ch >= 'A' && ch <= 'Z')      play_key(0x04 + (ch - 'A'), false);
    else if(ch >= '1' && ch <= '9') play_key(0x1e + (ch - '1'), false);
    else if(ch == '0')              play_key(0x27, false);
    else if(ch == '"')              play_key(0x1f, true);    // shift 2
    else if(ch == '*')              play_key(0x30, false);
    else if(ch == ',')              play_key(0x36, false);
    else if(ch == '\r')             play_key(0x28, false);
  }
}

static void play_reset(void) {
  if(config_get_action("cold_reset")) sys_run_action_by_name("cold_reset");
  else                                sys_run_action_by_name("reset");
}

// The first disk into its drive, the machine started afresh.
static void play_start(const char *name) {
  char line[96];
  const char *dot = strrchr(name, '.');
  const char *ext = dot ? dot + 1 : "";
  int drive = play_drive(cfg ? cfg->menu : NULL, ext);
  if(drive < 0) {
    snprintf(line, sizeof(line), "play: no drive for .%s in this core", ext);
    net_log(line);
    snprintf(message, sizeof(message), "No drive for .%s here", ext);
    net_notify();
    return;
  }
  bool c64 = cfg && cfg->name && !strcasecmp(cfg->name, "C64Nano");
  bool c64disk = c64 && (!strcasecmp(ext, "d64") || !strcasecmp(ext, "g64"));
  bool c64prg = c64 && !strcasecmp(ext, "prg");
  snprintf(line, sizeof(line), "play: %s into drive %d%s", name, drive,
           c64disk ? ", LOAD and RUN typed" : "");
  net_log(line);

  char img[NET_NAME_LEN];
  strcpy(img, name);
  sdc_set_cwd(drive, CARD_MOUNTPOINT);
  if(c64prg) {
    // the core puts a program into memory and runs it: into a machine
    // that has just started
    play_reset();
    vTaskDelay(pdMS_TO_TICKS(3000));
    sdc_image_open(drive, img);
    return;
  }
  if(sdc_image_open(drive, img) != 0) {
    snprintf(message, sizeof(message), "Cannot open %s", name);
    net_notify();
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(200));
  play_reset();
  if(!c64disk) return;

  // READY. after about two seconds; then LOAD, and RUN once the drive has
  // been busy and gone quiet for three seconds -- it asks the card for
  // sectors while it loads. A game that starts itself gets the RUN in its
  // keyboard buffer, which games ignore.
  vTaskDelay(pdMS_TO_TICKS(4000));
  play_type("LOAD\"*\",8,1\r");
  uint32_t seen = sdc_sector_requests;
  TickType_t t0 = xTaskGetTickCount(), last = t0;
  bool busy = false;
  while((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(240000)) {
    vTaskDelay(pdMS_TO_TICKS(250));
    if(sdc_sector_requests != seen) { seen = sdc_sector_requests; last = xTaskGetTickCount(); busy = true; }
    if(busy && (xTaskGetTickCount() - last) > pdMS_TO_TICKS(3000)) break;
    if(!busy && (xTaskGetTickCount() - t0) > pdMS_TO_TICKS(30000)) break;   // nothing asked: loaded at once
  }
  play_type("RUN\r");
  net_log("play: RUN typed");
}

// One round: ask, and if a game waits, fetch its disks and start it.
// Returns whether the server could be asked.
static bool play_round(void) {
  bool answered = false;
  netport_select();
  vTaskDelay(pdMS_TO_TICKS(50));
  bool got = play_poll(&answered);
  bool ok = false;
  card_jobs_run();
  if(got) {
    char line[96];
    snprintf(line, sizeof(line), "play: %s, %d disk%s", play_names[0], play_count, play_count == 1 ? "" : "s");
    net_log(line);
    for(int i = 0; i < play_count; i++) {
      bool one = download_file(play_names[i], play_hrefs[i], NET_MARK_CARD);
      if(i == 0) ok = one;
      if(!one) break;
    }
  }
  net_baud_slow();
  netport_restore();
  if(ok) play_start(play_names[0]);
  return answered;
}

void netdl_set_play(int s) { play_secs = (s <= 0) ? 0 : (s < 5) ? 5 : (s > 600) ? 600 : s; }
int netdl_get_play(void) { return play_secs; }

#define REQ_LIST      (-1)
#define REQ_MSG       (-2)

// A message for the modem's status display, AT+MSG:<title>|<text>, for
// when the machine's own screen is dark. Spaces go as '_', so the line
// gets through Zimodem's command parser in one piece.
static char disp_msg[120];
static volatile bool disp_msg_done = true;

static void display_message(void) {
  char line[NET_LINE_LEN];
  if(modem_ati() && modem == MODEM_ZIMODEM) {
    net_flush();
    net_puts("AT+MSG:");
    net_puts(disp_msg);
    net_puts("\r");
    while(net_read_line(line, sizeof(line), 1500))
      if(!strcmp(line, "OK") || !strcmp(line, "ERROR")) break;
  }
  disp_msg_done = true;
}

bool netdl_display_message(const char *msg, int wait_ms) {
  if(!req_queue) return false;    // no port in this core
  size_t n = 0;
  for(; msg && *msg && n < sizeof(disp_msg) - 1; msg++)
    disp_msg[n++] = (*msg == ' ') ? '_' : *msg;
  disp_msg[n] = 0;
  disp_msg_done = false;
  int req = REQ_MSG;
  if(xQueueSendToBack(req_queue, &req, 0) != pdTRUE) { disp_msg_done = true; return false; }
  for(int i = 0; i < wait_ms / 50 && !disp_msg_done; i++) vTaskDelay(pdMS_TO_TICKS(50));
  return disp_msg_done;
}


static void net_task(__attribute__((unused)) void *parms) {
  debugf("NET: task running");

  // Ask the modem for its address so the OSD can show it, and get it into
  // its network if it is not.
  //
  // A Zimodem does not retry a join of its own accord: when the one it
  // makes at power up fails, it reports "ERROR ON <ssid>" and sets its
  // retry delay to zero, so it stays out of the network until something
  // tells it otherwise. Its first join does fail now and then. So this
  // keeps asking as long as the modem answers without being in a network,
  // and every so often offers it the network from the ini or resets it,
  // which brings it in within seconds. Only a port where nothing answers
  // at all is given up on.
  //
  // The user's Serial setting is put back after every attempt, and a
  // request from the menu always comes first.
  //
  // TOS looks at its clock chip when it starts and when a program ends,
  // never in between. For the right time without a reset by hand the ST's
  // start is held back until the clock is set (see netdl_hold_start()), a
  // few seconds as a rule and never longer than clockwait= in the ini says.
  // While that lasts the probes come quickly.
  #define NET_PROBES 12            // silent ones before there is no modem
  #define NET_NUDGE_MS 45000       // between two attempts to get it in
  int probes = 0, silent = 0;
  bool nudged = false;
  TickType_t last_nudge = 0;
  TickType_t t0 = xTaskGetTickCount();
  TickType_t next_probe = t0 + pdMS_TO_TICKS(hold_release ? 1500 : 12000);
  TickType_t next_play = t0 + pdMS_TO_TICKS(30000);
  int play_fails = 0;
  bool reported = false;          // the status went to the server once

  while(1) {
    if(hold_release && (time_set || link_state == LINK_NONE ||
       (xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(clock_wait * 1000))) {
      char l[80];
      snprintf(l, sizeof(l), "start: ST let go after %d s, clock %s",
               (int)((xTaskGetTickCount() - t0) / pdMS_TO_TICKS(1000)),
               time_set ? "set" : "NOT set in time");
      net_log(l);
      void (*release)(void) = hold_release;
      hold_release = NULL;
      release();
    }

    if((!modem_ip[0] || !time_set) && link_state != LINK_NONE && state != NET_STATE_BUSY &&
       (int32_t)(xTaskGetTickCount() - next_probe) >= 0) {
      probes++;
      netport_select();
      vTaskDelay(pdMS_TO_TICKS(50));
      if(!modem_ati()) {
        // Nothing on the port. In its first seconds that is normal: a
        // Zimodem does not answer while it joins its network. Later on it
        // means no modem, and no point in asking on and on.
        if((xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(20000) && ++silent >= NET_PROBES)
          link_state = LINK_NONE;
        else
          link_state = LINK_TRYING;
      } else if(modem_ip[0]) {
        silent = 0;
        modem_time_sync();
      } else if(wifi_down && modem == MODEM_ZIMODEM &&
                (!nudged || (xTaskGetTickCount() - last_nudge) >= pdMS_TO_TICKS(NET_NUDGE_MS))) {
        // The modem answers and is not in a network: its own join has
        // failed, and it will not try again by itself. The first attempt
        // offers it the network from the ini, which also stores it; later
        // ones reset it, which was measured to bring it in within a few
        // seconds. Never faster than NET_NUDGE_MS, or a join in progress
        // would be cut short and the link would only flap.
        silent = 0;
        last_nudge = xTaskGetTickCount();
        if(!nudged && wifi_cfg[0] && !wifi_tried) {
          if(modem_join_from_ini() && modem_ati() && modem_ip[0]) modem_time_sync();
        } else {
          net_log("modem is not in its network: resetting it");
          net_puts("ATZ\r");
        }
        nudged = true;
      } else
        silent = 0;
      if(modem_ip[0]) {
        link_state = LINK_UP;
        // A request that failed while the modem was out of its network is
        // history once it is back. Without this the menu keeps showing
        // "Modem has no WiFi" for good, because the state of a request is
        // only ever left by the next one - and the line is there to say
        // what is true now.
        if(state == NET_STATE_ERROR)
          state = name_count ? NET_STATE_LIST : NET_STATE_IDLE;
      }
      else if(link_state != LINK_NONE) link_state = wifi_down ? LINK_NO_WIFI : LINK_TRYING;
      netport_restore();
      debugf("NET: modem address probe %d: '%s'", probes, modem_ip);
      {
        // the start-up in NETLOG.TXT, for the day the clock stays wrong
        char l[80];
        snprintf(l, sizeof(l), "start: probe %d after %d s: %s%s", probes,
                 (int)((xTaskGetTickCount() - t0) / pdMS_TO_TICKS(1000)),
                 (modem == MODEM_UNKNOWN) ? "no answer" : modem_ip[0] ? modem_ip :
                 wifi_down ? "modem has no WiFi yet" : "no address",
                 time_set ? ", clock set" : "");
        net_log(l);
      }
      next_probe = xTaskGetTickCount() +
        pdMS_TO_TICKS(hold_release ? 2000 :
                      ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(180000)) ? 15000 : 60000);
      continue;
    }

    // the status once after start-up, see status_report(); with play= the
    // questions for a game carry it anyway
    if(!reported && !play_secs && server[0] && modem_ip[0] && link_state == LINK_UP &&
       !hold_release && state != NET_STATE_BUSY) {
      reported = true;
      netport_select();
      vTaskDelay(pdMS_TO_TICKS(50));
      status_report();
      net_baud_slow();
      netport_restore();
      continue;
    }

    // a game waiting at the server? Not while a request runs or the ST
    // still waits for its clock; after failures less often, a server that
    // does not know /_/job (not stserve, or an older one) every 5 minutes
    if(play_secs && server[0] && modem_ip[0] && link_state == LINK_UP && !hold_release &&
       state != NET_STATE_BUSY && (int32_t)(xTaskGetTickCount() - next_play) >= 0) {
      // A question that fails is mostly the modem missing one fetch: the
      // next one comes soon, and only a server that keeps failing is
      // asked less often. NETLOG gets the first failure of a run and the
      // end of it, not every question.
      if(play_round()) {
        if(play_fails >= 3) {
          char l[64];
          snprintf(l, sizeof(l), "play: asking works again after %d failures", play_fails);
          net_log(l);
        }
        play_fails = 0;
      } else if(++play_fails == 3) {
        char l[96];
        snprintf(l, sizeof(l), "play: asking failed 3 times (%s), now once a minute", play_why);
        net_log(l);
      }
      next_play = xTaskGetTickCount() +
        pdMS_TO_TICKS(play_fails >= 10 ? 300000 : play_fails >= 3 ? 60000 :
                      play_fails ? 15000 : play_secs * 1000);
      continue;
    }

    int req;
    // wake up now and then while the address is still unknown
    TickType_t wait = hold_release ? pdMS_TO_TICKS(200) :
      ((!modem_ip[0] || !time_set) && link_state != LINK_NONE) ? pdMS_TO_TICKS(1000) : 0xffffffffUL;
    if(play_secs && server[0] && modem_ip[0] && link_state == LINK_UP) {
      int32_t left = (int32_t)(next_play - xTaskGetTickCount());
      TickType_t w = (left > 0) ? (TickType_t)left : 1;
      if(w < wait) wait = w;
    }
    if(xQueueReceive(req_queue, &req, wait)) {
      // Port 1 only exists while the core routes its serial pins to it.
      // Switch it on for the request regardless of the menu, and put the
      // user's setting back afterwards.
      netport_select();
      vTaskDelay(pdMS_TO_TICKS(50));

      if(req == REQ_LIST)          fetch_list();
      else if(req == REQ_MSG)      display_message();
      else {
        download(req);
        // the server learns what arrived, see status_report()
        if(state == NET_STATE_DONE && last_file[0]) status_report();
      }

      // the ST expects its modem at 19200 again
      net_baud_slow();

      // the clock, if the probes after power up did not get that far
      if(!time_set && modem_ip[0]) modem_time_sync();

      netport_restore();
    }
  }
}

void netdl_request_list(void) {
  int req = REQ_LIST;
  if(!req_queue) return;   // no port in this core, no task, no queue
  if(state != NET_STATE_BUSY) xQueueSendToBack(req_queue, &req, 0);
}


void netdl_request_download(int index) {
  if(!req_queue) return;
  if(state != NET_STATE_BUSY) xQueueSendToBack(req_queue, &index, 0);
}

// ------------------------------------------------------------- getters ----

void netdl_set_wifi(const char *s) {
  strncpy(wifi_cfg, s, sizeof(wifi_cfg)-1);
  wifi_cfg[sizeof(wifi_cfg)-1] = 0;
}
const char *netdl_get_wifi(void) { return wifi_cfg; }
const char *netdl_get_ip(void) { return time_busy ? "" : modem_ip; }

// One line for the OSD: where we stand with the modem.
const char *netdl_link_text(void) {
  switch(link_state) {
    case LINK_UP:      return modem_ip[0] ? modem_ip : "modem connected";
    case LINK_TRYING:  return "modem connecting...";
    // kept short: the OSD line is 128 pixels wide
    case LINK_NO_WIFI: return wifi_none ? "modem has no network"
                                        : "modem has no WiFi";
    case LINK_NONE:    return "no modem on port 1";
    default:           return "modem not asked yet";
  }
}
void netdl_set_timezone(const char *s) {
  strncpy(tz_cfg, s, sizeof(tz_cfg)-1);
  tz_cfg[sizeof(tz_cfg)-1] = 0;
}
const char *netdl_get_timezone(void) { return tz_cfg; }
void netdl_set_clock_wait(int s) { clock_wait = (s < 0) ? 0 : (s > 60) ? 60 : s; }
int netdl_get_clock_wait(void) { return clock_wait; }

// Called before the core is let out of reset. With a modem set up in the
// ini (wifi= or server=) the start is held back until the clock is set:
// returns true and calls release() from the net task then, or when
// clockwait= seconds are over. Without a modem nothing waits.
bool netdl_hold_start(void (*release)(void)) {
  // without a port there is no modem and no clock to wait for, so the core
  // must not be held back. main.c asks before netdl_init() has run, so the
  // core's menu is asked here; without that the ST never waited and booted
  // with the date TOS keeps, 1989 on TOS 1.04.
  if(!netport_ok) netport_ok = config_find_role("netport", &netport_id, &netport_val);
  if(!netport_ok) return false;
  if(clock_wait <= 0 || (!wifi_cfg[0] && !server[0])) return false;
  // Only TOS looks at its clock when it starts. The C64 has no clock to
  // wait for, and its start is not held back for one.
  if(!cfg || !cfg->name || strcasecmp(cfg->name, "MiSTeryNano") != 0) return false;
  hold_release = release;
  return true;
}

void netdl_set_server(const char *s) {
  strncpy(server, s, sizeof(server)-1);
  server[sizeof(server)-1] = '\0';
  debugf("NET: server %s", server);
}

const char *netdl_get_server(void) { return server; }
int netdl_state(void) { return state; }
const char *netdl_message(void) { return message; }
int netdl_entry_count(void) { return (state == NET_STATE_LIST || state == NET_STATE_DONE || state == NET_STATE_ERROR) ? name_count : 0; }
const char *netdl_entry_name(int index) { return (index >= 0 && index < name_count) ? names[index] : ""; }
int netdl_entry_mark(int index) { return (index >= 0 && index < name_count) ? marked[index] : NET_MARK_NONE; }
void netdl_set_to_stick(bool on) { to_stick = on; }
bool netdl_to_stick(void) { return to_stick; }
uint32_t netdl_bytes_done(void) { return bytes_done; }
uint32_t netdl_bytes_total(void) { return bytes_total; }

void netdl_init(void) {
  // Ask the running core where its companion port is. A core that offers none
  // gets no net task at all: it would have nothing to talk to, and writing a
  // variable on spec is how this feature used to cold boot the C64 over and
  // over.
  netport_ok = config_find_role("netport", &netport_id, &netport_val);
  if(!netport_ok) {
    debugf("NET: no netport role in this core, net disabled");
    return;
  }

  debugf("NET: init, netport is '%c' = %d", netport_id, netport_val);
  rx_stream = xStreamBufferCreate(NET_RX_BUF, 1);
  req_queue = xQueueCreate(2, sizeof(int));
  // the BL616 build sets 2048 words, see its CMakeLists.txt
#ifndef NET_TASK_STACK
#define NET_TASK_STACK 4096
#endif
  xTaskCreate(net_task, (char *)"net_task", NET_TASK_STACK, NULL, configMAX_PRIORITIES-10, NULL);
}
