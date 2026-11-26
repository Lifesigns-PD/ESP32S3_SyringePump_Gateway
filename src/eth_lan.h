/*
 * eth_lan.h / eth_lan.cpp  -  LAN cable (Ethernet) through a W5500 SPI chip
 *
 * The ESP32-S3 has no Ethernet MAC of its own, so the LAN port is a W5500 on SPI
 * (pins in config.h, section 3b). The driver is the ESP-IDF "esp_eth" W5500 driver that is already
 * inside Arduino core 2.0.x, glued to lwIP - so HTTPClient, WebServer, mDNS and SNTP work over the cable
 * exactly as they do over WiFi. Nothing in WiFi is touched.
 *
 * Address: DHCP, static, or "DHCP then static" (see NetCfg in settings.h).
 * All functions are safe to call even when ETH_ENABLED is 0 (they report "no LAN").
 */
#pragma once
#include "gw_compat.h"

void   ethBegin();                 // start the W5500 + a small helper task (DHCP fallback, mDNS, NTP)
bool   ethStarted();               // driver started OK
bool   ethLinkUp();                // cable connected, link established
bool   ethHasIp();                 // has an IPv4 address (DHCP or static)
bool   ethUsingStaticFallback();   // DHCP did not answer -> the static address is in use
String ethIpString();
String ethGatewayString();
String ethMaskString();
String ethMacString();             // "AA:BB:CC:DD:EE:FF" (also valid before the link is up)
String ethStateText();             // short human text for the web page
String ethDnsString();             // DNS server of the LAN interface
String ethProbeText();             // result of the W5500 check at start-up ("W5500 answered ..." / "no answer ...")

// "Find the LAN chip": briefly drives the SPI pins of each known wiring (saved pins, then presets) and reads the
// W5500 version register. Returns JSON {"allowed":bool,"message":"..","results":[{"name","pins":[..],"ok","text"}]}.
// Refused while the LAN is running (it would disturb the working link).
String ethScanPresetsJson();
