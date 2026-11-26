/*
 * pump_reader.h / pump_reader.cpp  -  the pump side of the project
 *
 *   UART bytes -> AgiliaParser (CRC checked frames) -> decode (agilia.cpp) -> JSON
 *              -> kept as "latest JSON" + log line for the web page
 *              -> handed to http_sender when it must be uploaded (on change / every SEND_INTERVAL_MS)
 */
#pragma once
#include "gw_compat.h"
#include <string>
#include "agilia.h"

void                pumpBegin();                 // start the UART
void                pumpPoll();                  // called by the loop task (the web server and uploads run in their own tasks)
const AgiliaStats&  pumpStats();
std::string         pumpLatestJson();            // newest decoded frame as JSON ("" if none yet); a copy, safe to call from the web server task
String              pumpLogText();               // last LOG_LINES frames, oldest first
int                 pumpUartNum();               // UART peripheral of the pump line (0 normally, 1 if the console stays on)
int                 pumpRxPin();                 // pin the pump line is read on (saved setting)
uint32_t            pumpBaudRate();
bool                pumpConsoleOff();            // true when the pump uses GPIO43/44: the serial console is switched off
uint32_t            pumpLastFrameAgeS();         // seconds since the last CRC-valid frame (0xFFFFFFFF = none since start)
