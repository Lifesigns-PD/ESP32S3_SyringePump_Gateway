/*
 * gw_compat.h / gw_compat.cpp  -  the few helpers the firmware logic needs, implemented directly on ESP-IDF
 *
 *   String, IPAddress   gw_string.h (plain C++)
 *   millis(), delay()   esp_timer / FreeRTOS
 *   Console             printf output that can be switched OFF (when the pump uses the UART0 pins, nothing may be sent on TXD0)
 *   Prefs               settings store in flash (NVS): SAME namespace, key names and value types as before, so settings saved by
 *                       the Arduino version of this firmware are read unchanged
 *   gwRestart()         esp_restart()
 */
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_string.h"

#ifndef PROGMEM
#define PROGMEM
#endif

uint32_t millis();
void     delay(uint32_t ms);
void     gwRestart();

class ConsoleShim {
 public:
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void println();
  void println(const char* s);
  void println(const String& s);
  void print(const char* s);
  void print(const String& s);
  void flush();
  void end();                    // switch the console off for good (log output of the SDK too)
  bool isOn() const;
};
extern ConsoleShim Console;

class Prefs {
 public:
  bool   begin(const char* ns, bool readOnly = false);
  bool   isKey(const char* key);
  String getString(const char* key, const char* def = "");
  size_t putString(const char* key, const String& v);
  bool   getBool(const char* key, bool def = false);
  size_t putBool(const char* key, bool v);
  uint8_t getUChar(const char* key, uint8_t def = 0);
  size_t  putUChar(const char* key, uint8_t v);
  int8_t  getChar(const char* key, int8_t def = 0);
  size_t  putChar(const char* key, int8_t v);
  uint16_t getUShort(const char* key, uint16_t def = 0);
  size_t   putUShort(const char* key, uint16_t v);
  uint32_t getUInt(const char* key, uint32_t def = 0);
  size_t   putUInt(const char* key, uint32_t v);
 private:
  uint32_t h_ = 0;
  bool open_ = false;
};
