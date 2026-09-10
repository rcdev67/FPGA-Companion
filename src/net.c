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
static char names[NET_MAX_ENTRIES][NET_NAME_LEN];
static int name_count = 0;
static uint32_t bytes_done = 0, bytes_total = 0;
static int modem = MODEM_UNKNOWN;
static bool wifi_down = false;      // the modem reported no WiFi on the last ATI
static bool in_body = false;        // file data is streaming, keep it out of the tail

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
static bool modem_ati(void) {
  char line[NET_LINE_LEN];

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
    // Zimodem reports its WiFi as "CONNECTED TO <ssid> (<ip>)" or "ERROR ON <ssid>"
    if(!strncmp(line, "ERROR ON", 8)) wifi_down = true;
    if(!strcmp(line, "OK")) return true;
    if(!strcmp(line, "ERROR")) return false;
  }
  return false;
}

// find out what is on the other end of the wire
static bool modem_detect(void) {
  bool ok = modem_ati();

  if(ok && wifi_down && modem == MODEM_ZIMODEM) {
    // Zimodem has lost its WiFi and would wait a while before trying
    // again. ATZ reloads its settings and joins right away.
    // One reset only and then patience: repeated resets interrupt the
    // join in progress and the link flaps instead of coming up.
    net_set_message("Modem joining WiFi...");
    net_flush();
    net_puts("ATZ\r");
    vTaskDelay(pdMS_TO_TICKS(25000));
    ok = modem_ati();
    if(wifi_down) { net_set_message("Modem has no WiFi"); return false; }
  }

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
static bool net_server_parse(char *host, int max, int *port) {
  if(!server[0]) { net_set_message("No server in ini file"); return false; }
  strncpy(host, server, max-1);
  host[max-1] = '\0';
  *port = 80;
  char *c = strchr(host, ':');
  if(c) { *c = '\0'; *port = atoi(c+1); }
  return host[0] != '\0';
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
  snprintf(line, sizeof(line), "AT&G\"xmodem:http://%s:%d/%s\"", host, port, path);   // AT&G is the web get; quoted, unquoted text ends at the first letter
  net_flush();
  net_puts(line);
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

  if(!net_server_parse(host, sizeof(host), &port)) return false;

  if(!modem_detect()) return false;

  // Zimodem has the block wise way, no raw stream needed
  if(modem == MODEM_ZIMODEM) return xmodem_get(host, port, path, sink);

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
  snprintf(line, sizeof(line), "GET /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
  net_puts(line);

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

static char index_text[NET_MAX_ENTRIES * NET_NAME_LEN];
static int index_len;

static bool index_sink(const unsigned char *data, int len) {
  int room = sizeof(index_text) - 1 - index_len;
  if(len > room) len = room;
  memcpy(index_text + index_len, data, len);
  index_len += len;
  return true;
}

static void fetch_list(void) {
  state = NET_STATE_BUSY;
  name_count = 0;
  index_len = 0;
  net_set_message("Fetching list...");

  if(!http_get(NET_INDEX_FILE, index_sink)) {
    state = NET_STATE_ERROR;
    char line[80];
    snprintf(line, sizeof(line), "FAIL list: %s", message);
    net_log(line);
    menu_notify(MENU_EVENT_NET_UPDATE);
    return;
  }

  // one file name per line, comments and blanks skipped
  index_text[index_len] = '\0';
  char *p = index_text;
  while(*p && name_count < NET_MAX_ENTRIES) {
    char *e = p;
    while(*e && *e != '\n') e++;
    char *end = e;
    if(*e) e++;
    while(end > p && (end[-1] == '\r' || end[-1] == ' ')) end--;
    *end = '\0';
    if(*p && *p != ';' && *p != '#' && strlen(p) < NET_NAME_LEN) {
      strcpy(names[name_count++], p);
    }
    p = e;
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

static void download(int index) {
  if(index < 0 || index >= name_count) return;
  const char *name = names[index];

  state = NET_STATE_BUSY;
  bytes_done = bytes_total = 0;
  snprintf(message, sizeof(message), "Loading %s", name);
  menu_notify(MENU_EVENT_NET_UPDATE);

  char path[NET_NAME_LEN + 8];
  snprintf(path, sizeof(path), "%s/%s", CARD_MOUNTPOINT, name);

  sdc_lock();
  FRESULT r = f_open(&dl_file, path, FA_CREATE_ALWAYS | FA_WRITE);
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

  bool ok = http_get(name, file_sink);

  sdc_lock();
  FRESULT cr = f_close(&dl_file);
  if(cr != FR_OK) {
    // the size is written on close: without it the file is empty
    snprintf(message, sizeof(message), "SD close f%d e%d", cr, sdc_get_last_error());
    ok = false;
  }
  if(!ok) f_unlink(path);      // no half files on the card
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

static void net_task(__attribute__((unused)) void *parms) {
  debugf("NET: task running");
  while(1) {
    int req;
    if(xQueueReceive(req_queue, &req, 0xffffffffUL)) {
      // Port 1 only exists while the core routes the M0S connector to it
      // ("Serial: Netz"). Switch it on for the request regardless of the
      // menu, and put the user's setting back afterwards.
      sys_set_val('E', 2);
      vTaskDelay(pdMS_TO_TICKS(50));

      if(req == REQ_LIST) fetch_list();
      else                download(req);

      int e = menu_variable_get('E');
      sys_set_val('E', (e < 0) ? 0 : e);
    }
  }
}

void netdl_request_list(void) {
  int req = REQ_LIST;
  if(state != NET_STATE_BUSY) xQueueSendToBack(req_queue, &req, 0);
}

void netdl_request_download(int index) {
  if(state != NET_STATE_BUSY) xQueueSendToBack(req_queue, &index, 0);
}

// ------------------------------------------------------------- getters ----

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
