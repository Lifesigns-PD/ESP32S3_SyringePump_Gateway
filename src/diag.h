/*
 * diag.h / diag.cpp  -  "Connection test": finds out WHERE the way from the gateway to the server breaks
 *
 * Runs on the gateway itself (Settings page -> Connection test). Steps:
 *   1. which interface is in use, its IP address / mask / gateway / DNS, LAN chip state
 *   2. is the target on the same network as the gateway, or does it need a router?
 *   3. DNS lookup (if the target is a name)
 *   4. TCP connect to host:port, with the result in plain words (refused / no answer / no route)
 *   5. optional: one small HTTP POST to the Server path, with the server's answer
 */
#pragma once
#include "gw_compat.h"

void   diagBegin();
bool   diagStart(const String& host, uint16_t port, bool sendPost);   // false if a test is already running
String diagJson();                                                    // {"running":..,"done":..,"lines":[{"k":"i|o|w|f","t":".."}]}
