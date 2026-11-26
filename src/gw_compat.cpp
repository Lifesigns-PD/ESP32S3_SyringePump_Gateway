#include "gw_compat.h"
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "nvs.h"

uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }
void delay(uint32_t ms) { vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 0); if (!ms) taskYIELD(); }
void gwRestart() { esp_restart(); }

// ---------------------------------------------------------------- console
static volatile bool consoleOn = true;

void ConsoleShim::printf(const char* fmt, ...) {
  if (!consoleOn) return;
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}
void ConsoleShim::println() { if (consoleOn) fputs("\r\n", stdout); }
void ConsoleShim::println(const char* s) { if (consoleOn) { fputs(s, stdout); fputs("\r\n", stdout); } }
void ConsoleShim::println(const String& s) { println(s.c_str()); }
void ConsoleShim::print(const char* s) { if (consoleOn) fputs(s, stdout); }
void ConsoleShim::print(const String& s) { print(s.c_str()); }
void ConsoleShim::flush() { if (consoleOn) fflush(stdout); }
void ConsoleShim::end() {
  if (consoleOn) fflush(stdout);
  consoleOn = false;
  esp_log_level_set("*", ESP_LOG_NONE);                 // the SDK's own messages must not leave the chip either
  gpio_reset_pin((gpio_num_t)43);                       // release the TXD0 pad (like Serial.end() did): nothing can be sent towards the pump's RX line
}
bool ConsoleShim::isOn() const { return consoleOn; }
ConsoleShim Console;

// ---------------------------------------------------------------- settings store (NVS)
bool Prefs::begin(const char* ns, bool readOnly) {
  nvs_handle_t h;
  if (nvs_open(ns, readOnly ? NVS_READONLY : NVS_READWRITE, &h) != ESP_OK) return false;
  h_ = (uint32_t)h;
  open_ = true;
  return true;
}

bool Prefs::isKey(const char* key) {
  if (!open_) return false;
  uint8_t v;                                              // any lookup tells "exists" (value or type mismatch) from "not found"
  esp_err_t e = nvs_get_u8((nvs_handle_t)h_, key, &v);
  return e == ESP_OK || e == ESP_ERR_NVS_TYPE_MISMATCH;
}

String Prefs::getString(const char* key, const char* def) {
  if (!open_) return String(def);
  size_t len = 0;
  if (nvs_get_str((nvs_handle_t)h_, key, nullptr, &len) != ESP_OK || len == 0) return String(def);
  char* b = (char*)malloc(len);
  if (!b) return String(def);
  String r(def);
  if (nvs_get_str((nvs_handle_t)h_, key, b, &len) == ESP_OK) r = String(b);
  free(b);
  return r;
}

#define PUT(call) do { if (!open_) return 0; if ((call) != ESP_OK) return 0; if (nvs_commit((nvs_handle_t)h_) != ESP_OK) return 0; } while (0)
size_t Prefs::putString(const char* key, const String& v) { PUT(nvs_set_str((nvs_handle_t)h_, key, v.c_str())); return v.length(); }
size_t Prefs::putBool(const char* key, bool v)             { PUT(nvs_set_u8((nvs_handle_t)h_, key, v ? 1 : 0)); return 1; }
size_t Prefs::putUChar(const char* key, uint8_t v)         { PUT(nvs_set_u8((nvs_handle_t)h_, key, v)); return 1; }
size_t Prefs::putChar(const char* key, int8_t v)           { PUT(nvs_set_i8((nvs_handle_t)h_, key, v)); return 1; }
size_t Prefs::putUShort(const char* key, uint16_t v)       { PUT(nvs_set_u16((nvs_handle_t)h_, key, v)); return 2; }
size_t Prefs::putUInt(const char* key, uint32_t v)         { PUT(nvs_set_u32((nvs_handle_t)h_, key, v)); return 4; }

bool Prefs::getBool(const char* key, bool def)             { uint8_t v = def ? 1 : 0; if (open_ && nvs_get_u8((nvs_handle_t)h_, key, &v) != ESP_OK) v = def ? 1 : 0; return v != 0; }
uint8_t Prefs::getUChar(const char* key, uint8_t def)      { uint8_t v = def; if (open_ && nvs_get_u8((nvs_handle_t)h_, key, &v) != ESP_OK) v = def; return v; }
int8_t Prefs::getChar(const char* key, int8_t def)         { int8_t v = def; if (open_ && nvs_get_i8((nvs_handle_t)h_, key, &v) != ESP_OK) v = def; return v; }
uint16_t Prefs::getUShort(const char* key, uint16_t def)   { uint16_t v = def; if (open_ && nvs_get_u16((nvs_handle_t)h_, key, &v) != ESP_OK) v = def; return v; }
uint32_t Prefs::getUInt(const char* key, uint32_t def)     { uint32_t v = def; if (open_ && nvs_get_u32((nvs_handle_t)h_, key, &v) != ESP_OK) v = def; return v; }
