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
#include "debug.h"

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
static int name_count = 0;
static uint32_t bytes_done = 0, bytes_total = 0;
static int modem = MODEM_UNKNOWN;
static bool wifi_down = false;      // the modem reported no WiFi on the last ATI
static bool in_body = false;        // file data is streaming, keep it out of the tail
// The core's network UART: system value N selects 0=19200 1=38400 2=57600
// 3=115200 4=460800. Transfers run at the fastest one; the port FIFOs in
// the core hold 255 bytes, enough for the few milliseconds the companion
// may be busy with the card while a block arrives.
#define NET_FAST_BAUD   "460800"
#define NET_FAST_N      4

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
static void net_log(const char *line) {
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
  menu_notify(MENU_EVENT_NET_UPDATE);
}

// send a command and wait for a final reply line: true on "OK" or the
// expected prefix, false on "ERROR", "NO CARRIER" or timeout
static bool modem_cmd(const char *cmd, const char *expect, int timeout_ms) {
  char line[NET_LINE_LEN];

  net_flush();
  net_puts(cmd);
  net_puts("\r");

  while(net_read_line(line, sizeof(line), timeout_ms)) {
    debugf("NET: modem '%s'", line);
    if(!strcmp(line, cmd)) continue;        // the modem's echo of the command
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

static void net_port_reset(void) {
  sys_set_val('E', 1);
  vTaskDelay(pdMS_TO_TICKS(50));
  sys_set_val('E', 2);
  vTaskDelay(pdMS_TO_TICKS(50));
  net_flush();
  net_log("port reset");
}

static bool wifi_none = false;      // the modem has no network configured
static char modem_ip[20];           // from the modem's "CONNECTED TO <ssid> (<ip>)"
static char joy_status[40] = "not asked yet";   // the Bluetooth controller, as the modem reports it
static bool joy_busy = false;
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
    if(strstr(line, "Zimodem") || strstr(line, "zimodem")) modem = MODEM_ZIMODEM;
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
    if(!strcmp(line, "OK")) return true;
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
    static const int8_t fast_n[] = { NET_FAST_N, 3 };
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
// 115200 for the duration: Zimodem changes its rate the moment it sees
// ATB and already answers at the new one, so the core's port follows
// right behind the command. Whatever happens, the request loop puts
// 19200 back at the end.

static bool net_baud_fast(void) {
  if(modem != MODEM_ZIMODEM) return false;
  net_flush();
  net_puts("ATB" NET_FAST_BAUD "\r");
  vTaskDelay(pdMS_TO_TICKS(80));         // the command must leave the port at 19200
  sys_set_val('N', NET_FAST_N);
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

static bool xmodem_get(const char *host, int port, const char *path, bool (*sink)(const unsigned char*, int)) {
  char line[NET_LINE_LEN];
  static unsigned char blk[1024 + 4];

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
  bool announced = false;
  while(net_read_line(line, sizeof(line), 60000)) {
    debugf("NET: xmodem '%s'", line);
    if(!strncmp(line, "XMODEM ", 7)) { bytes_total = strtoul(line + 7, NULL, 10); announced = true; break; }
    if(!strcmp(line, "ERROR") || !strncmp(line, "NO CARRIER", 10)) break;
  }
  if(!announced) { net_set_message("Modem could not fetch"); return false; }

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
      menu_notify(MENU_EVENT_NET_UPDATE);
    }
  }
  in_body = false;

  // the modem closes with OK, wait for it so the next command is not
  // swallowed by the transfer's tail
  while(net_read_line(line, sizeof(line), 3000)) if(!strcmp(line, "OK") || !strcmp(line, "ERROR")) break;
  net_flush();

  if(ok && bytes_done != bytes_total) { net_set_message("Short transfer"); ok = false; }
  return ok;
}

// Fetch /path from the server. Every body byte goes to sink(); a sink
// returning false aborts. Returns true when the whole body arrived.
static bool http_get(const char *path, bool (*sink)(const unsigned char*, int)) {
  char host[NET_SERVER_LEN];
  int port;
  char line[NET_LINE_LEN];
  char base[NET_PATH_LEN], full[NET_PATH_LEN + NET_HREF_LEN];

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
      menu_notify(MENU_EVENT_NET_UPDATE);
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
static void add_entry(const char *href) {
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
    menu_notify(MENU_EVENT_NET_UPDATE);
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
      add_entry(p);
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
      if(*p && *p != ';' && *p != '#') add_entry(p);
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
  menu_notify(MENU_EVENT_NET_UPDATE);
}

static FIL dl_file;
static bool dl_open;

static bool file_sink(const unsigned char *data, int len) {
  UINT bw = 0;
  sdc_lock();
  FRESULT r = f_write(&dl_file, data, len, &bw);
  sdc_unlock();
  if(r != FR_OK || bw != (UINT)len) {
    // f_write's result, the bytes it managed, the sector layer's reason
    // (1/2 read busy/timeout, 3/4 write busy/timeout) and the offset
    char msg[48];
    snprintf(msg, sizeof(msg), "SD write f%d w%u e%d @%lu", r, bw, sdc_get_last_error(), (unsigned long)bytes_done);
    net_set_message(msg);
    return false;
  }

  // commit size and cluster chain every 64 KB, so a transfer that breaks
  // off later leaves a file that is at least consistent up to here
  if((bytes_done & 0xffff) < (uint32_t)len) {
    sdc_lock();
    r = f_sync(&dl_file);
    sdc_unlock();
    if(r != FR_OK) {
      char msg[48];
      snprintf(msg, sizeof(msg), "SD sync f%d e%d @%lu", r, sdc_get_last_error(), (unsigned long)bytes_done);
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

  state = NET_STATE_BUSY;
  bytes_done = bytes_total = 0;
  snprintf(message, sizeof(message), "Loading %s", name);
  menu_notify(MENU_EVENT_NET_UPDATE);

  char path[NET_NAME_LEN + 8];
  snprintf(path, sizeof(path), "%s/%s", CARD_MOUNTPOINT, name);
  // written under a temporary name and renamed at the end, so a copy
  // already on the card survives a failed attempt
  static const char tmp_path[] = CARD_MOUNTPOINT "/NETDL.TMP";

  sdc_lock();
  f_unlink(tmp_path);
  FRESULT r = f_open(&dl_file, tmp_path, FA_CREATE_ALWAYS | FA_WRITE);
  sdc_unlock();
  if(r != FR_OK) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Cannot create f%d e%d", r, sdc_get_last_error());
    net_set_message(msg);
    state = NET_STATE_ERROR;
    menu_notify(MENU_EVENT_NET_UPDATE);
    return;
  }
  dl_open = true;

  char url[NET_PATH_LEN + NET_HREF_LEN];
  snprintf(url, sizeof(url), "%s%s", cwd, hrefs[index]);
  bool ok = http_get(url, file_sink);            // the URL form, spaces and all encoded

  sdc_lock();
  FRESULT cr = f_close(&dl_file);
  if(cr != FR_OK) {
    // the size is written on close: without it the file is empty
    snprintf(message, sizeof(message), "SD close f%d e%d", cr, sdc_get_last_error());
    ok = false;
  }
  if(ok) {
    f_unlink(path);            // replace an older copy
    FRESULT rr = f_rename(tmp_path, path);
    if(rr != FR_OK) {
      snprintf(message, sizeof(message), "SD rename f%d e%d", rr, sdc_get_last_error());
      ok = false;
    }
  }
  if(!ok) f_unlink(tmp_path);  // no half files on the card
  sdc_unlock();
  dl_open = false;

  if(ok) {
    snprintf(message, sizeof(message), "%s: %lu bytes", name, (unsigned long)bytes_done);
    state = NET_STATE_DONE;
  } else
    state = NET_STATE_ERROR;

  {
    char line[96];
    snprintf(line, sizeof(line), "%s %s (%lu/%lu) %s", ok ? "OK  " : "FAIL", name,
             (unsigned long)bytes_done, (unsigned long)bytes_total, message);
    net_log(line);
  }
  menu_notify(MENU_EVENT_NET_UPDATE);
}

#define REQ_LIST      (-1)
#define REQ_JOY       (-2)
#define REQ_JOY_PAIR  (-3)

// The modem's Bluetooth side: AT+JOY answers "JOYPAD <model>" or "JOYPAD
// NONE", AT+JOYPAIR forgets the bond and waits for a controller in pairing
// mode. A modem without these commands answers ERROR.
static void joy_request(bool pair) {
  char line[NET_LINE_LEN];

  joy_busy = true;
  snprintf(joy_status, sizeof(joy_status), "asking the modem...");
  menu_notify(MENU_EVENT_NET_UPDATE);

  if(!modem_ati()) {
    net_port_reset();
    if(!modem_ati()) {
      snprintf(joy_status, sizeof(joy_status), "no modem on port 1");
      goto done;
    }
  }
  if(modem != MODEM_ZIMODEM) {
    snprintf(joy_status, sizeof(joy_status), "modem has no Bluetooth");
    goto done;
  }

  if(pair && !modem_cmd("AT+JOYPAIR", NULL, 3000)) {
    snprintf(joy_status, sizeof(joy_status), "modem has no Bluetooth");
    goto done;
  }

  net_flush();
  net_puts("AT+JOY\r");
  snprintf(joy_status, sizeof(joy_status), "modem has no Bluetooth");
  while(net_read_line(line, sizeof(line), 2000)) {
    debugf("NET: joy '%s'", line);
    if(!strncmp(line, "JOYPAD ", 7)) {
      if(!strcmp(line + 7, "NONE"))
        snprintf(joy_status, sizeof(joy_status), "%s", pair ? "pairing: hold its pair button" : "none connected");
      else if(!strcmp(line + 7, "UNSUPPORTED"))
        snprintf(joy_status, sizeof(joy_status), "modem built without Bluetooth");
      else
        snprintf(joy_status, sizeof(joy_status), "%.38s", line + 7);
    }
    if(!strcmp(line, "OK") || !strcmp(line, "ERROR")) break;
  }

done:
  joy_busy = false;
  menu_notify(MENU_EVENT_NET_UPDATE);
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
      sys_set_val('E', 2);
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
      if(modem_ip[0])                  link_state = LINK_UP;
      else if(link_state != LINK_NONE) link_state = wifi_down ? LINK_NO_WIFI : LINK_TRYING;
      int e0 = menu_variable_get('E');
      sys_set_val('E', (e0 < 0) ? 0 : e0);
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

    int req;
    // wake up now and then while the address is still unknown
    TickType_t wait = hold_release ? pdMS_TO_TICKS(200) :
      ((!modem_ip[0] || !time_set) && link_state != LINK_NONE) ? pdMS_TO_TICKS(1000) : 0xffffffffUL;
    if(xQueueReceive(req_queue, &req, wait)) {
      // Port 1 only exists while the core routes the M0S connector to it
      // ("Serial: Netz"). Switch it on for the request regardless of the
      // menu, and put the user's setting back afterwards.
      sys_set_val('E', 2);
      vTaskDelay(pdMS_TO_TICKS(50));

      if(req == REQ_LIST)          fetch_list();
      else if(req == REQ_JOY)      joy_request(false);
      else if(req == REQ_JOY_PAIR) joy_request(true);
      else                         download(req);

      // the ST expects its modem at 19200 again
      net_baud_slow();

      // the clock, if the probes after power up did not get that far
      if(!time_set && modem_ip[0]) modem_time_sync();

      int e = menu_variable_get('E');
      sys_set_val('E', (e < 0) ? 0 : e);
    }
  }
}

void netdl_request_list(void) {
  int req = REQ_LIST;
  if(state != NET_STATE_BUSY) xQueueSendToBack(req_queue, &req, 0);
}

void netdl_request_joy(bool pair) {
  int req = pair ? REQ_JOY_PAIR : REQ_JOY;
  if(state != NET_STATE_BUSY && !joy_busy) xQueueSendToBack(req_queue, &req, 0);
}
const char *netdl_joy_status(void) { return joy_status; }
bool netdl_joy_busy(void) { return joy_busy; }

void netdl_request_download(int index) {
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
  if(clock_wait <= 0 || (!wifi_cfg[0] && !server[0])) return false;
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
uint32_t netdl_bytes_done(void) { return bytes_done; }
uint32_t netdl_bytes_total(void) { return bytes_total; }

void netdl_init(void) {
  debugf("NET: init");
  rx_stream = xStreamBufferCreate(NET_RX_BUF, 1);
  req_queue = xQueueCreate(2, sizeof(int));
  xTaskCreate(net_task, (char *)"net_task", 4096, NULL, configMAX_PRIORITIES-10, NULL);
}
