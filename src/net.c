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
    net_set_message("Modem joining WiFi...");
    for(int attempt = 0; attempt < 3 && wifi_down; attempt++) {
      net_flush();
      net_puts("ATZ\r");
      vTaskDelay(pdMS_TO_TICKS(12000));
      ok = modem_ati();
    }
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

// Fetch /path from the server. Every body byte goes to sink(); a sink
// returning false aborts. Returns true when the whole body arrived.
static bool http_get(const char *path, bool (*sink)(const unsigned char*, int)) {
  char host[NET_SERVER_LEN];
  int port;
  char line[NET_LINE_LEN];

  if(!net_server_parse(host, sizeof(host), &port)) return false;

  if(!modem_detect()) return false;

  net_set_message("Connecting...");
  if(!modem_connect(host, port)) {
    net_set_message("Connect failed");
    net_flush();
    return false;
  }

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
  menu_notify(MENU_EVENT_NET_UPDATE);
}

#define REQ_LIST      (-1)

static void net_task(__attribute__((unused)) void *parms) {
  debugf("NET: task running");
  while(1) {
    int req;
    if(xQueueReceive(req_queue, &req, 0xffffffffUL)) {
      if(req == REQ_LIST) fetch_list();
      else                download(req);
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
