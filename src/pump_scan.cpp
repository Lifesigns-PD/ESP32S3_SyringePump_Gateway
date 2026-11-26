#include "pump_scan.h"
#include "config.h"
#include "settings.h"
#include "pump_reader.h"
#include "agilia.h"
#include "utils.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"

struct Cand { int pin; const char* label; };
static const Cand CAND[] = {
  {44, "GPIO44 = RXD0 (module pad 36, U0RXD)"},
  {43, "GPIO43 = TXD0 (module pad 37, U0TXD)"},
  {17, "GPIO17 (default of earlier firmware versions)"},
  {18, "GPIO18"},
};
static const int NCAND = sizeof(CAND) / sizeof(CAND[0]);
static const uint32_t WINDOW_MS = 3500;                  // the pump sends a status frame about every 2 s

struct Res { int pin; uint32_t bytes, frames, crc; int idle; bool done; };
static Res  res[NCAND];
static int  nres = 0;
static int  bestPin = -1;
static String note;
static uint32_t scanBaud = 0;
static volatile bool running = false, done = false;
static SemaphoreHandle_t mux = nullptr;
static const uart_port_t SCAN_UART = UART_NUM_2;      // the scan listens on UART2 so the running pump line (UART1) is not disturbed

static void scanUartOpen(int pin, uint32_t baud) {
  uart_config_t uc;
  memset(&uc, 0, sizeof(uc));
  uc.baud_rate = (int)baud;
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_APB;
  uart_driver_install(SCAN_UART, 1024, 0, 0, nullptr, 0);
  uart_param_config(SCAN_UART, &uc);
  uart_set_pin(SCAN_UART, UART_PIN_NO_CHANGE, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static void scanUartClose() { uart_driver_delete(SCAN_UART); }

static void setNote(const String& s) {
  if (!mux) return;
  xSemaphoreTake(mux, portMAX_DELAY);
  note = s;
  xSemaphoreGive(mux);
  Console.printf("[pump scan] %s\n", s.c_str());
}

static bool pinUsedByLan(int pin) {
  NetCfg n;
  settingsGetNet(n);
  if (n.mode == NET_WIFI) return false;
  return pin == n.pSclk || pin == n.pMiso || pin == n.pMosi || pin == n.pCs || pin == n.pInt || pin == n.pRst;
}

static void runScan() {
#if (DEV_INF_ENABLED && DEV_INF_UART_ENABLED && DEV_INF_UART_NUM == 2) || (DEV_PCA_ENABLED && DEV_PCA_UART_ENABLED && DEV_PCA_UART_NUM == 2)
  setNote("pin finder is off: UART2 is used by the PCA pump input (config.h DEV_PCA_UART_ENABLED)");
  return;
#endif
  PumpCfg pc;
  settingsGetPump(pc);
  xSemaphoreTake(mux, portMAX_DELAY);
  nres = 0; bestPin = -1; scanBaud = pc.baud; note = "scanning ...";
  xSemaphoreGive(mux);
  uint32_t bestFrames = 0;

  for (int i = 0; i < NCAND; i++) {
    const int pin = CAND[i].pin;
    if (pinUsedByLan(pin)) continue;                       // never touch the LAN chip pins
    if (pin == pumpRxPin()) {                              // the pin in use: report its live counters, do not disturb it
      const AgiliaStats& live = pumpStats();
      xSemaphoreTake(mux, portMAX_DELAY);
      res[nres++] = {pin, live.bytes, live.framesOk, live.crc8Err + live.crc16Err, 1, true};
      xSemaphoreGive(mux);
      if (live.framesOk > bestFrames) { bestFrames = live.framesOk; bestPin = pin; }
      Console.printf("[pump scan] GPIO%d is the pin in use: %u bytes, %u valid frames so far\n", pin, (unsigned)live.bytes, (unsigned)live.framesOk);
      continue;
    }
    if (pin == 43 && !pumpConsoleOff() && pumpRxPin() == 44) continue;   // console TX is kept on this pin: leave it alone
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    delay(3);
    const int idle = gpio_get_level((gpio_num_t)pin);      // a UART line idles HIGH
    scanUartOpen(pin, pc.baud);
    AgiliaParser p;
    AgiliaFrame f;
    const uint32_t t0 = millis();
    while (millis() - t0 < WINDOW_MS) {
      uint8_t rb[64];
      int rn;
      while ((rn = uart_read_bytes(SCAN_UART, rb, sizeof(rb), 0)) > 0) {
        for (int k = 0; k < rn; k++) {
          p.push(rb[k], millis());
          while (p.next(f, millis())) {}
        }
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    while (p.next(f, millis())) {}
    scanUartClose();

    xSemaphoreTake(mux, portMAX_DELAY);
    res[nres++] = {pin, p.stats.bytes, p.stats.framesOk, p.stats.crc8Err + p.stats.crc16Err, idle, true};
    xSemaphoreGive(mux);
    Console.printf("[pump scan] GPIO%d: idle %s, %u bytes, %u valid frames, %u CRC errors\n", pin, idle ? "high" : "LOW", (unsigned)p.stats.bytes, (unsigned)p.stats.framesOk, (unsigned)(p.stats.crc8Err + p.stats.crc16Err));
    if (p.stats.framesOk > bestFrames) { bestFrames = p.stats.framesOk; bestPin = pin; }
  }

#if !CONSOLE_ON_USB
  if (!pumpConsoleOff() && pumpRxPin() != 44) uart_set_pin(UART_NUM_0, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);   // scanning GPIO43/44 detached the console: attach it again
#endif
  if (bestPin >= 0) setNote("pump found on GPIO" + String(bestPin) + " (" + String((unsigned)bestFrames) + " valid frames)");
  else setNote("no valid pump frame on any of the pins. Check: pump powered on, pump TX wire, common GND, baud rate (" + String((unsigned)pc.baud) + "), RS-232 versus TTL levels.");
}

static void scanTask(void*) {
  runScan();
  xSemaphoreTake(mux, portMAX_DELAY);
  done = true;
  running = false;
  xSemaphoreGive(mux);
  vTaskDelete(nullptr);
}

bool pumpScanStart() {
  if (!mux) mux = xSemaphoreCreateMutex();
  xSemaphoreTake(mux, portMAX_DELAY);
  if (running) { xSemaphoreGive(mux); return false; }
  running = true;
  done = false;
  xSemaphoreGive(mux);
  xTaskCreatePinnedToCore(scanTask, "pumpScan", 6144, nullptr, 1, nullptr, 0);
  return true;
}

String pumpScanJson() {
  if (!mux) mux = xSemaphoreCreateMutex();
  xSemaphoreTake(mux, portMAX_DELAY);
  String j = String("{\"running\":") + (running ? "true" : "false") + ",\"done\":" + (done ? "true" : "false") + ",\"baud\":" + String((unsigned)scanBaud) +
             ",\"best\":" + String(bestPin) + ",\"note\":\"" + jsonEscape(note) + "\",\"results\":[";
  for (int i = 0; i < nres; i++) {
    const char* label = "";
    for (int k = 0; k < NCAND; k++) if (CAND[k].pin == res[i].pin) label = CAND[k].label;
    if (i) j += ",";
    j += "{\"pin\":" + String(res[i].pin) + ",\"label\":\"" + jsonEscape(label) + "\",\"bytes\":" + String((unsigned)res[i].bytes) + ",\"frames\":" + String((unsigned)res[i].frames) +
         ",\"crc\":" + String((unsigned)res[i].crc) + ",\"idle\":" + String(res[i].idle) + "}";
  }
  j += "]}";
  xSemaphoreGive(mux);
  return j;
}

// ---- auto mode: once after start-up, only when the pump has not been heard
static void autoTask(void*) {
  vTaskDelay(pdMS_TO_TICKS((uint32_t)PUMP_AUTO_FIND_DELAY_S * 1000UL));
  PumpCfg pc;
  settingsGetPump(pc);
  if (!pc.autoFind || pumpLastFrameAgeS() != 0xFFFFFFFFu) vTaskDelete(nullptr);     // switched off, or the saved pin works
  setNote("no pump frame since start-up: looking for the pump line once ...");
  if (!pumpScanStart()) vTaskDelete(nullptr);
  vTaskDelay(pdMS_TO_TICKS(1000));
  while (running) vTaskDelay(pdMS_TO_TICKS(500));
  if (bestPin >= 0 && bestPin != pc.rx) {
    pc.rx = (int8_t)bestPin;
    if (pc.tx == bestPin) pc.tx = -1;
    String err;
    if (settingsSetPump(pc, err)) {
      setNote("pump found on GPIO" + String(bestPin) + ": saved. Restarting to use it ...");
      vTaskDelay(pdMS_TO_TICKS(1500));
      gwRestart();
    } else {
      setNote("pump found on GPIO" + String(bestPin) + " but it could not be saved: " + err);
    }
  }
  vTaskDelete(nullptr);
}

void pumpScanBegin() {
  if (!mux) mux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(autoTask, "pumpAuto", 4096, nullptr, 1, nullptr, 0);
}
