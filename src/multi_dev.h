/*
 * multi_dev.h / multi_dev.cpp  -  infusion pump + PCA pump inputs (own task each; UART line and/or TCP simulator port)
 *
 *   real device --UART (optional, receive only)--+
 *                                                 +--> line parser (dev_proto) -> JSON -> latest/log (dashboard /devices) + senderEnqueue
 *   PC simulator --TCP :2601 / :2602 -------------+                                     + "OK" / "ERR,reason" back on the TCP link
 *
 * Works on WiFi and on the LAN cable (the TCP listener starts when either has an IP). Independent from the Agilia syringe pump
 * (pump_reader) and the HL7 monitor.
 */
#pragma once
#include "gw_compat.h"
#include "dev_proto.h"
#include <string>

struct DevStats {
  uint32_t lines;         // lines received (valid + rejected)
  uint32_t ok;
  uint32_t err;
  uint32_t bytes;
  uint32_t clients;       // TCP connections accepted
  bool     connected;     // a TCP simulator is connected now
  bool     uartOn;        // hardware UART line is configured
  bool     tcpOn;         // TCP simulator port is open for business (config.h DEV_xxx_TCP_ENABLED)
  char     peer[24];
  char     lastTransport[5];   // "uart" / "tcp"
  char     lastError[48];
  int      tcpPort;
  int      uartRx;
  int      uartTx;
};

void        devBegin();                          // start both tasks (call after netBegin)
DevStats    devGetStats(DevType t);
std::string devLatestJson(DevType t);            // "" if none yet
String      devLogText(DevType t);
uint32_t    devLastAgeS(DevType t);              // seconds since the last valid line (0xFFFFFFFF = none)
bool        devEnabled(DevType t);
