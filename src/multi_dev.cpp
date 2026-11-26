#include "multi_dev.h"
#include "config.h"
#include "http_sender.h"
#include "net_manager.h"
#include "pump_reader.h"
#include <string.h>
#include <errno.h>
#include <time.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "lwip/sockets.h"

namespace {

struct LineBuf { std::string s; bool over = false; };

struct Dev {
  DevType           type;
  const char*       name;
  bool              enabled;
  int               tcpPort;
  bool              tcpOn;
  bool              uartOn;
  int               uartNum, rx, tx, baud;
  SemaphoreHandle_t mux;
  std::string       latest;
  String            log[LOG_LINES];
  uint32_t          logTotal;
  DevStats          st;
  volatile uint32_t lastMs;           // last valid line
  uint32_t          seq, sendLastMs;
  std::string       lastKey;
  LineBuf           uartLine, tcpLine;
  int               ls, cs;
  uint32_t          lastRxMs, listenRetryMs;
};

Dev devs[2];

struct Lock {
  SemaphoreHandle_t m;
  explicit Lock(SemaphoreHandle_t mm) : m(mm) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
  ~Lock() { if (m) xSemaphoreGive(m); }
};

void setErr(Dev& d, const char* e) {
  Lock l(d.mux);
  snprintf(d.st.lastError, sizeof(d.st.lastError), "%s", e);
}

// one complete line from the UART or from the TCP link
void processLine(Dev& d, const std::string& line, const char* transport, int sock) {
  DevMsg m;
  std::string err;
  const bool ok = devParseLine(d.type, line, m, err);
  char tag[12];
  snprintf(tag, sizeof(tag), "%s", d.type == DEV_INFUSION ? "inf" : "pca");

  if (!ok) {
    {
      Lock l(d.mux);
      d.st.lines++; d.st.err++;
      snprintf(d.st.lastError, sizeof(d.st.lastError), "%s", err.c_str());
      snprintf(d.st.lastTransport, sizeof(d.st.lastTransport), "%s", transport);
      d.log[d.logTotal % LOG_LINES] = "[" + String((unsigned long)(millis() / 1000)) + "s] REJECTED (" + err.c_str() + ")";
      d.logTotal++;
    }
    Console.printf("[%s] rejected: %s\n", tag, err.c_str());
    if (sock >= 0) { std::string r = "ERR," + err + "\r\n"; send(sock, r.data(), r.size(), 0); }
    return;
  }

  char mac[20], ip[20], recv[24];
  snprintf(mac, sizeof(mac), "%s", netMacString().c_str());
  snprintf(ip, sizeof(ip), "%s", netActiveIp().c_str());
  recv[0] = 0;
  const time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(recv, sizeof(recv), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  }
  d.seq++;
  DevJsonCtx c;
  c.gatewayId = HOSTNAME; c.mac = mac; c.fw = FW_VERSION; c.iface = netActiveIface(); c.ip = ip;
  c.transport = transport; c.receivedAtUtc = recv; c.uptimeMs = millis(); c.seq = d.seq;
  const std::string js = devToJson(m, c);
  const std::string sum = devSummary(m);
  const std::string key = devChangeKey(m);

  {
    Lock l(d.mux);
    d.latest = js;
    d.lastMs = millis() ? millis() : 1;
    d.st.lines++; d.st.ok++;
    snprintf(d.st.lastTransport, sizeof(d.st.lastTransport), "%s", transport);
    d.log[d.logTotal % LOG_LINES] = "[" + String((unsigned long)(millis() / 1000)) + "s] #" + String((unsigned long)d.seq) + " " + transport + " " + sum.c_str();
    d.logTotal++;
  }
  Console.printf("[%s] #%lu %s %s\n", tag, (unsigned long)d.seq, transport, sum.c_str());
#if PRINT_JSON_SERIAL
  Console.println(js.c_str());
#endif

  const uint32_t now = millis();
  const bool changed = key != d.lastKey && d.sendLastMs != 0 && (now - d.sendLastMs) >= 1000;
  const bool due = d.sendLastMs == 0 || (now - d.sendLastMs) >= DEV_SEND_INTERVAL_MS;
  if (changed || due) { d.sendLastMs = now ? now : 1; d.lastKey = key; senderEnqueue(js); }

  if (sock >= 0) send(sock, "OK\r\n", 4, 0);
}

void feed(Dev& d, LineBuf& lb, const uint8_t* p, int n, const char* transport, int sock) {
  for (int i = 0; i < n; i++) {
    const char ch = (char)p[i];
    if (ch == '\n') {
      if (lb.over) { lb.over = false; setErr(d, "line too long"); Lock l(d.mux); d.st.lines++; d.st.err++; }
      else if (!lb.s.empty()) processLine(d, lb.s, transport, sock);
      lb.s.clear();
    } else if (!lb.over) {
      if (lb.s.size() < DEV_MAX_LINE) lb.s += ch;
      else { lb.over = true; lb.s.clear(); }
    }
  }
}

void closeClient(Dev& d) {
  if (d.cs >= 0) { close(d.cs); d.cs = -1; }
  d.tcpLine.s.clear(); d.tcpLine.over = false;
  Lock l(d.mux);
  d.st.connected = false;
}

void closeAll(Dev& d) {
  closeClient(d);
  if (d.ls >= 0) { close(d.ls); d.ls = -1; }
}

bool openListen(Dev& d) {
  const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (s < 0) return false;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(d.tcpPort);
  if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 2) != 0) {
    Console.printf("[%s] bind/listen on port %d failed (errno %d)\n", d.name, d.tcpPort, errno);
    close(s);
    return false;
  }
  d.ls = s;
  Console.printf("[%s] simulator listener on TCP port %d\n", d.name, d.tcpPort);
  return true;
}

void devTask(void* arg) {
  Dev& d = *(Dev*)arg;
  uint8_t rx[256];
  for (;;) {
    if (d.uartOn) {
      int n;
      while ((n = uart_read_bytes((uart_port_t)d.uartNum, rx, sizeof(rx), 0)) > 0) {
        { Lock l(d.mux); d.st.bytes += n; }
        feed(d, d.uartLine, rx, n, "uart", -1);
      }
    }
    if (!d.tcpOn) {                                         // UART line only: no socket, just poll the line (works without a network too)
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (!netIsUp()) {
      closeAll(d);
      vTaskDelay(pdMS_TO_TICKS(d.uartOn ? 20 : 500));
      continue;
    }
    if (d.ls < 0) {
      if (millis() - d.listenRetryMs < 3000 && d.listenRetryMs != 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
      d.listenRetryMs = millis() ? millis() : 1;
      if (!openListen(d)) continue;
    }
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(d.ls, &rf);
    int mx = d.ls;
    if (d.cs >= 0) { FD_SET(d.cs, &rf); if (d.cs > mx) mx = d.cs; }
    struct timeval tv = {0, 20000};
    const int r = select(mx + 1, &rf, nullptr, nullptr, &tv);
    if (r > 0) {
      if (FD_ISSET(d.ls, &rf)) {
        struct sockaddr_in pa;
        socklen_t pl = sizeof(pa);
        const int ns = accept(d.ls, (struct sockaddr*)&pa, &pl);
        if (ns >= 0) {
          closeClient(d);                                   // the newest connection wins (a restarted simulator reconnects at once)
          d.cs = ns;
          d.lastRxMs = millis();
          Lock l(d.mux);
          d.st.clients++;
          d.st.connected = true;
          inet_ntoa_r(pa.sin_addr, d.st.peer, sizeof(d.st.peer));
          Console.printf("[%s] simulator connected from %s\n", d.name, d.st.peer);
        }
      }
      if (d.cs >= 0 && FD_ISSET(d.cs, &rf)) {
        const int n = recv(d.cs, rx, sizeof(rx), 0);
        if (n <= 0) { Console.printf("[%s] simulator disconnected\n", d.name); closeClient(d); }
        else {
          d.lastRxMs = millis();
          { Lock l(d.mux); d.st.bytes += n; }
          feed(d, d.tcpLine, rx, n, "tcp", d.cs);
        }
      }
    }
    if (d.cs >= 0 && (millis() - d.lastRxMs) > (uint32_t)DEV_IDLE_TIMEOUT_S * 1000UL) {
      Console.printf("[%s] simulator idle, connection closed\n", d.name);
      closeClient(d);
    }
  }
}

void uartOpen(Dev& d) {
  if (d.uartNum == pumpUartNum()) {                         // that UART is taken by the syringe pump line: never touch it
    Console.printf("[%s] UART%d is used by the syringe pump line: this device stays on its TCP port only\n", d.name, d.uartNum);
    d.uartOn = false;
    d.st.uartOn = false;
    return;
  }
  uart_config_t uc;
  memset(&uc, 0, sizeof(uc));
  uc.baud_rate = d.baud;
  uc.data_bits = UART_DATA_8_BITS;                          // 8N1
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_APB;
  uart_driver_install((uart_port_t)d.uartNum, 2048, 0, 0, nullptr, 0);   // receive buffer only: this firmware never transmits
  uart_param_config((uart_port_t)d.uartNum, &uc);
  uart_set_pin((uart_port_t)d.uartNum, d.tx, d.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  gpio_set_pull_mode((gpio_num_t)d.rx, GPIO_PULLUP_ONLY);   // idle level HIGH: an unconnected RX pin must not pick up noise
  Console.printf("[%s] UART%d: RX=GPIO%d TX=GPIO%d baud=%d 8N1\n", d.name, d.uartNum, d.rx, d.tx, d.baud);
}

void init(Dev& d, DevType t, const char* name, bool en, bool tcpOn, int port, bool uartOn, int un, int rx, int tx, int baud) {
  d.type = t; d.name = name; d.enabled = en; d.tcpOn = en && tcpOn; d.tcpPort = port; d.uartOn = en && uartOn;
  d.uartNum = un; d.rx = rx; d.tx = tx; d.baud = baud;
  d.mux = xSemaphoreCreateMutex();
  d.logTotal = 0; d.lastMs = 0; d.seq = 0; d.sendLastMs = 0;
  d.ls = -1; d.cs = -1; d.lastRxMs = 0; d.listenRetryMs = 0;
  memset(&d.st, 0, sizeof(d.st));
  d.st.tcpOn = d.tcpOn; d.st.tcpPort = d.tcpOn ? port : 0; d.st.uartOn = d.uartOn; d.st.uartRx = rx; d.st.uartTx = tx;
}

}  // namespace

void devBegin() {
  init(devs[0], DEV_INFUSION, "inf", DEV_INF_ENABLED, DEV_INF_TCP_ENABLED, DEV_INF_TCP_PORT, DEV_INF_UART_ENABLED, DEV_INF_UART_NUM, DEV_INF_RX_PIN, DEV_INF_TX_PIN, DEV_INF_BAUD);
  init(devs[1], DEV_PCA, "pca", DEV_PCA_ENABLED, DEV_PCA_TCP_ENABLED, DEV_PCA_TCP_PORT, DEV_PCA_UART_ENABLED, DEV_PCA_UART_NUM, DEV_PCA_RX_PIN, DEV_PCA_TX_PIN, DEV_PCA_BAUD);
  for (int i = 0; i < 2; i++) {
    if (!devs[i].enabled) continue;
    if (devs[i].uartOn) uartOpen(devs[i]);
    if (!devs[i].uartOn && !devs[i].tcpOn) { Console.printf("[%s] no input enabled (UART and TCP both off)\n", devs[i].name); continue; }
    xTaskCreatePinnedToCore(devTask, i == 0 ? "dev_inf" : "dev_pca", 8192, &devs[i], 1, nullptr, 0);
  }
}

DevStats devGetStats(DevType t) {
  Dev& d = devs[t];
  DevStats s;
  if (d.mux) { Lock l(d.mux); s = d.st; } else memset(&s, 0, sizeof(s));
  return s;
}

std::string devLatestJson(DevType t) {
  Dev& d = devs[t];
  if (!d.mux) return "";
  Lock l(d.mux);
  return d.latest;
}

String devLogText(DevType t) {
  Dev& d = devs[t];
  String out;
  if (!d.mux) return out;
  Lock l(d.mux);
  const uint32_t n = d.logTotal < LOG_LINES ? d.logTotal : LOG_LINES;
  const uint32_t first = d.logTotal - n;
  for (uint32_t i = 0; i < n; i++) { out += d.log[(first + i) % LOG_LINES]; out += "\n"; }
  return out;
}

uint32_t devLastAgeS(DevType t) { return devs[t].lastMs ? (millis() - devs[t].lastMs) / 1000 : 0xFFFFFFFFu; }
bool     devEnabled(DevType t)  { return devs[t].enabled; }
