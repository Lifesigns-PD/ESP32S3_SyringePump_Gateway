#include "hl7_monitor.h"
#include "config.h"
#include "hl7_parse.h"
#include "http_sender.h"
#include "net_manager.h"
#include <string.h>
#include <errno.h>
#include "lwip/sockets.h"

#if HL7_ENABLED

#define MLLP_SB 0x0B
#define MLLP_EB 0x1C
#define MLLP_CR 0x0D

static SemaphoreHandle_t dataMux;
static std::string       latestJson;
static String            logBuf[LOG_LINES];
static uint32_t          logTotal = 0;
static Hl7Stats          stats;
static volatile uint32_t lastMsgMs = 0;
static uint32_t          sendLastMs = 0;

Hl7Stats hl7GetStats() {
  Hl7Stats s;
  if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
  s = stats;
  if (dataMux) xSemaphoreGive(dataMux);
  return s;
}

std::string hl7LatestJson() {
  if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
  std::string c = latestJson;
  if (dataMux) xSemaphoreGive(dataMux);
  return c;
}

String hl7LogText() {
  String out;
  if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
  uint32_t n = logTotal < LOG_LINES ? logTotal : LOG_LINES;
  uint32_t first = logTotal - n;
  for (uint32_t i = 0; i < n; i++) { out += logBuf[(first + i) % LOG_LINES]; out += "\n"; }
  if (dataMux) xSemaphoreGive(dataMux);
  return out;
}

uint32_t hl7LastMsgAgeS() { return lastMsgMs ? (millis() - lastMsgMs) / 1000 : 0xFFFFFFFFu; }

static String shortSummary(const Hl7Msg& m) {
  // "HR=72 bpm SpO2=98 % ..." : first few observations
  String s;
  for (size_t i = 0; i < m.obs.size() && i < 6; i++) {
    if (i) s += " ";
    s += (m.obs[i].name.empty() ? m.obs[i].code : m.obs[i].name).c_str();
    s += "=";
    s += m.obs[i].value.c_str();
  }
  return s;
}

// one complete HL7 message (MLLP stripped): parse, ACK, store, upload
static void handleMessage(int sock, const std::string& raw) {
  Hl7Msg m = hl7Parse(raw, HL7_KEEP_PATIENT);
  const std::string ack = hl7BuildAck(m, m.valid, m.valid ? "" : m.error.c_str());
  std::string framed;
  framed += (char)MLLP_SB; framed += ack; framed += (char)MLLP_EB; framed += (char)MLLP_CR;
  send(sock, framed.data(), framed.size(), 0);

  String line;
  if (m.valid) {
    const std::string js = hl7ToJson(m, HOSTNAME, millis());
    line = "[" + String((unsigned long)(millis() / 1000)) + "s] " + m.msgType.c_str() + " id=" + m.controlId.c_str() + " bed=" + m.bed.c_str() + "  " + shortSummary(m);
    if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
    latestJson = js;
    stats.msgOk++;
    lastMsgMs = millis() ? millis() : 1;
    if (dataMux) xSemaphoreGive(dataMux);
#if PRINT_JSON_SERIAL
    Console.println(js.c_str());
#endif
    if (millis() - sendLastMs >= HL7_SEND_INTERVAL_MS || sendLastMs == 0) { sendLastMs = millis() ? millis() : 1; senderEnqueue(js); }
  } else {
    line = "[" + String((unsigned long)(millis() / 1000)) + "s] REJECTED (" + m.error.c_str() + ")";
    if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
    stats.msgErr++;
    if (dataMux) xSemaphoreGive(dataMux);
  }
  if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
  logBuf[logTotal % LOG_LINES] = line;
  logTotal++;
  if (dataMux) xSemaphoreGive(dataMux);
  Console.printf("[hl7] %s\n", line.c_str());
}

// serve one connected monitor until it disconnects / goes silent
static void serveClient(int sock) {
  struct timeval tv = { HL7_IDLE_TIMEOUT_S, 0 };
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  std::string buf;
  bool inMsg = false;
  uint8_t rx[512];
  for (;;) {
    int n = recv(sock, rx, sizeof(rx), 0);
    if (n <= 0) break;                                  // closed, error or idle timeout
    if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
    stats.bytes += n;
    if (dataMux) xSemaphoreGive(dataMux);
    for (int i = 0; i < n; i++) {
      uint8_t c = rx[i];
      if (c == MLLP_SB) { buf.clear(); inMsg = true; }
      else if (c == MLLP_EB && inMsg) {                 // end block; the CR that follows is skipped below
        handleMessage(sock, buf);
        buf.clear(); inMsg = false;
      } else if (inMsg) {
        if (buf.size() < HL7_MAX_MSG_BYTES) buf += (char)c;
        else { buf.clear(); inMsg = false; }            // oversize: drop it, wait for the next start block
      }
    }
  }
}

static void hl7Task(void*) {
  for (;;) {
    while (!netIsUp()) vTaskDelay(pdMS_TO_TICKS(1000));   // wait for an IP address (LAN or WiFi)

    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(HL7_PORT);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 2) != 0) {
      Console.printf("[hl7] bind/listen on port %d failed (errno %d), retry\n", HL7_PORT, errno);
      close(ls);
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    Console.printf("[hl7] MLLP listener on TCP port %d\n", HL7_PORT);

    for (;;) {
      struct sockaddr_in pa;
      socklen_t pl = sizeof(pa);
      int cs = accept(ls, (struct sockaddr*)&pa, &pl);
      if (cs < 0) { if (!netIsUp()) break; vTaskDelay(pdMS_TO_TICKS(200)); continue; }
      if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
      stats.clients++;
      stats.connected = true;
      inet_ntoa_r(pa.sin_addr, stats.peer, sizeof(stats.peer));
      if (dataMux) xSemaphoreGive(dataMux);
      Console.printf("[hl7] monitor connected from %s\n", stats.peer);
      serveClient(cs);
      close(cs);
      if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
      stats.connected = false;
      if (dataMux) xSemaphoreGive(dataMux);
      Console.println("[hl7] monitor disconnected");
    }
    close(ls);
  }
}

void hl7Begin() {
  dataMux = xSemaphoreCreateMutex();
  memset(&stats, 0, sizeof(stats));
  xTaskCreatePinnedToCore(hl7Task, "hl7", 10240, nullptr, 1, nullptr, 0);
}

#else   // HL7_ENABLED 0 : empty stubs so the rest of the firmware still links
Hl7Stats    hl7GetStats()    { Hl7Stats s; memset(&s, 0, sizeof(s)); return s; }
std::string hl7LatestJson()  { return ""; }
String      hl7LogText()     { return String(); }
uint32_t    hl7LastMsgAgeS() { return 0xFFFFFFFFu; }
void        hl7Begin()       {}
#endif
