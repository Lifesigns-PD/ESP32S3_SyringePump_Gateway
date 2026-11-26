/*
 * wifi_manager.h / wifi_manager.cpp  -  WiFi through ESP-IDF (esp_wifi), no Arduino
 *
 * Same behaviour as before: known networks (saved on the web page + the built-in list) are tried strongest first, retry every 5 s
 * (every 60 s while the setup hotspot is on), setup hotspot after AP_START_DELAY_MS without a network, async scan for the web page.
 */
#pragma once
#include "gw_compat.h"

void   wifiManagerBegin();
bool   wifiIsConnected();               // has an IP address on the WiFi station interface
bool   wifiApActive();                  // setup hotspot is on
String wifiScanJson();                  // async scan helper for the web page (see /api/wifi/scan)

// information about the WiFi interfaces (all safe to call when the WiFi radio was never started)
String    wifiSsid();                   // network we are connected to ("" if none)
int       wifiRssi();
IPAddress wifiIp();
IPAddress wifiMask();
IPAddress wifiGateway();
IPAddress wifiDns();
IPAddress wifiApIp();
String    wifiMacString();              // station MAC
