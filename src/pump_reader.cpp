#include "pump_reader.h"
#include "config.h"
#include "http_sender.h"
#include "net_manager.h"
#include "device_id.h"
#include "settings.h"
#include "eth_pins.h"
#include <time.h>
#include "driver/uart.h"

// The pump line is UART0 when the console is off (default: pump on GPIO44 = the UART0 pins), so UART1 and UART2 stay free for the
// infusion pump and the PCA pump (multi_dev). If the console stays on (custom pins / setting), UART0 is the console and the pump uses UART1.
static uart_port_t PUMP_UART = UART_NUM_1;
static AgiliaParser   parser;
static PumpCfg         pcfg;
static bool            consoleOff = false;
static volatile uint32_t lastFrameMs = 0;

// latestJson and the log are written by the loop task (decoder) and read by the web server task: guarded by dataMux
static SemaphoreHandle_t dataMux = nullptr;
static std::string latestJson;
static uint32_t    frameSeq = 0, lastSendMs = 0, lastKey = 0;
static bool        haveKey = false;

static String      logBuf[LOG_LINES];
static uint32_t    logTotal = 0;

const AgiliaStats& pumpStats()      { return parser.stats; }
std::string pumpLatestJson() {
  if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
  std::string copy = latestJson;
  if (dataMux) xSemaphoreGive(dataMux);
  return copy;
}

// ---------------------------------------------------------------- helpers
static String timestamp() {
  uint32_t ms = millis();
  char b[20];
  snprintf(b, sizeof(b), "%02lu:%02lu:%02lu.%03lu",
           (unsigned long)(ms / 3600000UL), (unsigned long)((ms / 60000UL) % 60),
           (unsigned long)((ms / 1000UL) % 60), (unsigned long)(ms % 1000UL));
  return String(b);
}

static String hexOf(const AgiliaFrame& f) {
  String h;
  h.reserve(f.sz * 3 + 8);
  char b[4];
  for (uint8_t i = 0; i < f.sz; i++) { snprintf(b, sizeof(b), "%02X ", f.payload[i]); h += b; }
  return h;
}

static AgiliaJsonContext makeCtx() {
  static char mac[20];
  static char recv[24];
  static char ip[20];
  snprintf(mac, sizeof(mac), "%s", netMacString().c_str());
  snprintf(ip, sizeof(ip), "%s", netActiveIp().c_str());
  time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(recv, sizeof(recv), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  } else {
    recv[0] = 0;
  }
  AgiliaJsonContext c;
  c.gatewayId = HOSTNAME;
  c.mac = mac;
  c.fw = FW_VERSION;
  c.uptimeMs = millis();
  c.receivedAtUtc = recv;
  c.includePatient = SEND_PATIENT_DATA;
  c.iface = netActiveIface();
  c.ip = ip;
  return c;
}

// ---------------------------------------------------------------- one decoded frame
static void handleFrame(const AgiliaFrame& f) {
  lastFrameMs = millis() ? millis() : 1;               // any CRC-valid frame proves the pump line works
  if (f.sz == 0) return;                              // empty keep-alive / ack frame
  frameSeq++;
  const uint8_t id = f.payload[0];
  AgiliaJsonContext ctx = makeCtx();
  std::string json;
  String line;
  bool post = false;

  if (id == 0x80) {                                   // Device status
    AgiliaStatus st;
    if (agiliaDecodeStatus(f, st, SEND_PATIENT_DATA)) {
      deviceIdSeen(agiliaDeviceName(st.deviceType), st.serial, st.version, st.hasStatusWord && (st.statusWord & 0x10));
      agiliaStatusToJson(f, st, ctx, json);
      line = agiliaSummary(st).c_str();
      uint32_t now = millis();
      bool changed = SEND_ON_CHANGE && haveKey && st.changeKey != lastKey && (now - lastSendMs) >= SEND_MIN_GAP_MS;
      bool due = !haveKey || (now - lastSendMs) >= SEND_INTERVAL_MS;
      if (changed || due) { post = true; lastKey = st.changeKey; haveKey = true; lastSendMs = now; }
    } else {
      agiliaRawToJson(f, ctx, json);
      line = "0x80 status frame of unsupported device";
    }
  } else if (id == 0xDF) {                            // Advanced infusion data
    if (agiliaAdvancedToJson(f, ctx, json)) { line = "0xDF advanced infusion data"; post = SEND_ADVANCED; }
    else { agiliaRawToJson(f, ctx, json); line = "0xDF (decode error)"; }
  } else {                                            // anything else: keep raw for the log
    agiliaRawToJson(f, ctx, json);
    char b[40];
    snprintf(b, sizeof(b), "frame 0x%02X (not decoded)", id);
    line = b;
  }

  String entry = "[" + timestamp() + "] #" + String(frameSeq) + " " + line + "\n    HEX: " + hexOf(f);
  if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
  latestJson = json;
  logBuf[logTotal % LOG_LINES] = entry;
  logTotal++;
  if (dataMux) xSemaphoreGive(dataMux);

  Console.printf("[%s] #%lu %s\n", timestamp().c_str(), (unsigned long)frameSeq, line.c_str());
#if PRINT_JSON_SERIAL
  Console.println(json.c_str());
#endif

  if (post) senderEnqueue(json);
}

// ---------------------------------------------------------------- public
void pumpBegin() {
  dataMux = xSemaphoreCreateMutex();
  settingsGetPump(pcfg);
#if !CONSOLE_ON_USB
  // The pump line is on the console pins (GPIO43 / GPIO44, the TTL header of the gateway). UART0 cannot be the console AND the
  // pump line: the console is switched off, so no debug text is ever sent out on the pump's line. Read the log on the web page.
  if (pumpUsesUart0Pins(pcfg.rx, pcfg.tx)) {
    // Kept on only when the pump uses RXD0 (GPIO44) alone and the setting asks for it: the console then keeps transmitting on TXD0
    // (GPIO43) - for a USB-UART adapter's RX pin. The pump's RX wire must NOT be on that pin.
    const bool keep = pcfg.console && pumpConsoleCanStay(pcfg.rx, pcfg.tx);
    if (keep) {
      Console.println("Pump UART on RXD0 (GPIO44); serial console kept ON on TXD0 (GPIO43): the pump's RX wire must stay disconnected");
    } else {
      Console.printf("Pump UART uses the console pins (RX=GPIO%d): the serial console is switched OFF now - use the web page log\n", pcfg.rx);
      Console.flush();
      delay(50);
      Console.end();
      consoleOff = true;
    }
  }
#endif
  PUMP_UART = consoleOff ? UART_NUM_0 : UART_NUM_1;
  uart_config_t uc;
  memset(&uc, 0, sizeof(uc));
  uc.baud_rate = (int)pcfg.baud;
  uc.data_bits = UART_DATA_8_BITS;                    // 8N1
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_APB;
  uart_driver_install(PUMP_UART, UART_RX_BUF_SIZE, 0, 0, nullptr, 0);      // receive buffer only: this firmware never transmits
  uart_param_config(PUMP_UART, &uc);
  uart_set_pin(PUMP_UART, pcfg.tx, pcfg.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  Console.printf("Pump UART%d: RX=GPIO%d TX=%d baud=%u 8N1\n", (int)PUMP_UART, pcfg.rx, pcfg.tx, (unsigned)pcfg.baud);
}

int      pumpUartNum()        { return (int)PUMP_UART; }

int      pumpRxPin()          { return pcfg.rx; }
uint32_t pumpBaudRate()       { return pcfg.baud; }
bool     pumpConsoleOff()     { return consoleOff; }
uint32_t pumpLastFrameAgeS()  { return lastFrameMs ? (millis() - lastFrameMs) / 1000 : 0xFFFFFFFFu; }

void pumpPoll() {
  AgiliaFrame f;
  uint8_t buf[128];
  int n;
  while ((n = uart_read_bytes(PUMP_UART, buf, sizeof(buf), 0)) > 0) {
    for (int i = 0; i < n; i++) {
#if RAW_DUMP_SERIAL
      Console.printf("%02X ", buf[i]);
#endif
      parser.push(buf[i], millis());
      while (parser.next(f, millis())) handleFrame(f);
    }
  }
  while (parser.next(f, millis())) handleFrame(f);    // flush / resync on idle
}

String pumpLogText() {
  String out;
  if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY);
  uint32_t count = (logTotal < LOG_LINES) ? logTotal : LOG_LINES;
  uint32_t first = logTotal - count;
  for (uint32_t i = 0; i < count; i++) { out += logBuf[(first + i) % LOG_LINES]; out += "\n"; }
  if (dataMux) xSemaphoreGive(dataMux);
  return out;
}
