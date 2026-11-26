/*
 * hl7_monitor.h / hl7_monitor.cpp  -  HL7 v2 patient-monitor input over the LAN (MLLP over TCP)
 *
 *   patient monitor (or the PC simulator tools/hl7_monitor_sim.py)
 *        --TCP, MLLP framing (0x0B <HL7> 0x1C 0x0D)-->  gateway :HL7_PORT
 *        gateway: parse ORU^R01 -> JSON -> latest/log (dashboard /monitor) + senderEnqueue (your server) + ACK back to the monitor
 *
 * Runs in its own FreeRTOS task; starts listening as soon as WiFi or LAN has an IP address. Works next to the Agilia UART pump input.
 */
#pragma once
#include "gw_compat.h"
#include <string>

struct Hl7Stats {
  uint32_t clients;      // connections accepted
  uint32_t msgOk;        // messages parsed and ACKed with AA
  uint32_t msgErr;       // messages rejected (AE)
  uint32_t bytes;
  bool     connected;    // a monitor is connected right now
  char     peer[24];     // last peer IP
};

void         hl7Begin();                 // start the listener task (call after netBegin)
Hl7Stats     hl7GetStats();
std::string  hl7LatestJson();            // newest decoded message as JSON ("" if none yet)
String       hl7LogText();               // last LOG_LINES messages, oldest first
uint32_t     hl7LastMsgAgeS();           // seconds since last valid message (0xFFFFFFFF = none)
