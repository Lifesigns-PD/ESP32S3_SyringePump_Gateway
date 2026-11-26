/*
 * pump_scan.h / pump_scan.cpp  -  "Find the pump line": listens on the likely pins and reports where the pump's data arrives
 *
 * Gateways with a TTL header often have the labels the other way round, or wire the header to the console UART (GPIO43/44).
 * The scan listens on each candidate pin for a few seconds at the saved baud rate and runs the Agilia frame parser on what it
 * hears. A pin where CRC-valid frames arrive is the pump line - there is no guessing.
 *
 * Candidates: GPIO44 (UART0 RX), GPIO43 (UART0 TX pad), GPIO17 (older default), GPIO18.
 * Auto mode (PumpCfg.autoFind): ONCE after start-up, if no valid frame arrived within PUMP_AUTO_FIND_DELAY_S, the scan runs by
 * itself; if the pump is found on another pin, that pin is saved and the gateway restarts.
 */
#pragma once
#include "gw_compat.h"

void   pumpScanBegin();              // starts the auto-find task (idle when switched off or when frames arrive)
bool   pumpScanStart();              // start a scan now (false if one is already running)
String pumpScanJson();               // {"running":..,"done":..,"baud":..,"best":pin|-1,"note":"..","results":[..]}
