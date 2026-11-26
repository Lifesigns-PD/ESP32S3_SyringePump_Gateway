/*
 * net_manager.h / net_manager.cpp  -  chooses WiFi, LAN cable or both (Settings -> Network interface)
 *
 *   mode WiFi only      -> wifi_manager (exactly as before), LAN driver not started
 *   mode LAN only       -> eth_lan, the WiFi radio is never started (no hotspot either)
 *   mode LAN first      -> both; the cable is used whenever it has an address, WiFi is the backup
 *
 * The rest of the firmware only asks netIsUp() / netActiveIface() / netActiveIp(), so it does not care
 * which cable or radio carries the JSON.
 */
#pragma once
#include "gw_compat.h"

void        netBegin();                 // read the saved mode and start the interface(s)
uint8_t     netMode();                  // NET_WIFI / NET_ETH / NET_AUTO (from settings.h), as started
bool        netIsUp();                  // an allowed interface has an IP address right now
const char* netActiveIface();           // "ethernet" | "wifi" | "none"
String      netActiveIp();
String      netActiveMask();
String      netActiveGateway();
String      netActiveDns();
String      netMacString();             // MAC of the active interface (LAN MAC in LAN-only mode)
void        netNtpStart();              // (re)start SNTP time sync
bool        netMdnsStart();             // start mDNS (mydevice.local); true when running
void        netMdnsStop();
void        netResetButtonPoll();       // call from loop(): hold NET_RESET_PIN to restore network defaults; clears the power-cycle counter
void        netPowerCycleCheck();       // call once in setup(): 5 quick power cycles restore the network defaults
