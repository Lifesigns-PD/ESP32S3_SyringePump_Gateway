/*
 * settings.h / settings.cpp  -  settings that can be changed at run time from the web page.
 *
 * Stored in flash (Preferences namespace "cfg"), so they survive reboot AND OTA updates:
 *   - WiFi networks (up to WIFI_MAX_SAVED)         -> Settings page, "WiFi" card
 *   - Server: scheme, host/IP, port, path, header  -> Settings page, "Server" card
 *   - Network: WiFi / LAN cable / both, LAN address -> Settings page, "Network interface" card
 *   - Connected device: auto-detect or typed in     -> Settings page, "Connected device" card
 *   - Server-managed firmware update (fleet OTA)    -> Firmware update page
 *
 * All functions are thread-safe (the web server, WiFi task and sender task all use them).
 */
#pragma once
#include "gw_compat.h"

#define WIFI_MAX_SAVED 4

struct ServerCfg {
  String   scheme;      // "http" or "https"
  String   host;        // IP address or domain; empty = sending disabled
  uint16_t port;        // 0 = default port of the scheme (80 / 443)
  String   path;        // always starts with '/'
  String   hdrName;     // optional extra header, e.g. "Authorization" or "x-api-key"
  String   hdrValue;    // optional header value, e.g. "Bearer abc123"
  ServerCfg() : port(0) {}
  bool   enabled() const { return host.length() > 0; }
  String url() const;   // scheme://host[:port]/path   ("" if disabled)
};

struct SavedWifi { String ssid; String pass; };

void settingsBegin();

// ---- server ----
void settingsGetServer(ServerCfg& out);
bool settingsSetServer(const ServerCfg& in, String& err);      // validates + saves

// ---- WiFi networks saved from the web page ----
int  settingsWifiCount();
bool settingsWifiGet(int idx, SavedWifi& out);
bool settingsWifiAdd(const String& ssid, const String& pass, String& err);   // add or update password
bool settingsWifiRemove(int idx);

// ---- network interface: WiFi / LAN cable ----
enum { NET_WIFI = 0, NET_ETH = 1, NET_AUTO = 2 };                      // NetCfg.mode
enum { ETHIP_DHCP = 0, ETHIP_STATIC = 1, ETHIP_DHCP_THEN_STATIC = 2 }; // NetCfg.ipMode

struct NetCfg {
  uint8_t mode;         // NET_WIFI / NET_ETH / NET_AUTO (LAN first, WiFi backup)
  uint8_t ipMode;       // how the LAN gets its address
  String  ip, mask, gw, dns;   // dotted text; used for static / fallback
  int8_t  pSclk, pMiso, pMosi, pCs, pInt, pRst;   // W5500 SPI pins (pRst -1 = not wired)
  uint8_t spiMhz;       // W5500 SPI clock in MHz
  uint8_t dhcpWaitS;    // "DHCP then static": seconds to wait for DHCP before using the static address (5-120)
  uint8_t macMode;      // 0 = automatic (unique, derived from this chip), 1 = the fixed MAC below
  String  mac;          // "AA:BB:CC:DD:EE:FF" used when macMode = 1
  NetCfg();             // defaults from config.h
};
void settingsGetNet(NetCfg& out);
bool settingsSetNet(const NetCfg& in, String& err);   // validates + saves (caller reboots)
void settingsResetNet();                              // back to the config.h defaults

// ---- connected device (what is on the gateway's UART) ----
struct DeviceCfg {
  bool   manual;        // false = auto-detect from the frames
  String cls;           // manual: syringe_pump | infusion_pump | patient_monitor | other
  String model;         // manual
  String serial;        // manual
  DeviceCfg() : manual(false) {}
};
void settingsGetDevice(DeviceCfg& out);
bool settingsSetDevice(const DeviceCfg& in, String& err);

// ---- server-managed firmware update (fleet OTA) ----
struct FleetCfg {
  uint8_t  mode;        // 0 off, 1 check only, 2 install automatically
  String   baseUrl;     // "" = same scheme/host/port as the data server; else "http://host:port"
  String   path;        // check URL path, default /api/ota/check
  uint16_t intervalMin; // minutes between checks
  FleetCfg();
};
void settingsGetFleet(FleetCfg& out);
bool settingsSetFleet(const FleetCfg& in, String& err);

// ---- pump serial input (UART)
struct PumpCfg {
  int8_t   rx;          // pin that receives the pump's TX
  int8_t   tx;          // -1 = none (this firmware only listens)
  uint32_t baud;
  bool     autoFind;    // once after start-up, when no pump frame arrives: find the pin automatically
  bool     console;     // keep the serial console on TXD0 (GPIO43) while the pump RX is on GPIO44
  PumpCfg();
};
void settingsGetPump(PumpCfg& out);
bool settingsSetPump(const PumpCfg& in, String& err);

// ---- small remembered texts (last seen device, last update result, ...) ----
String settingsNoteGet(const char* key);              // key: up to 12 characters
void   settingsNoteSet(const char* key, const String& value);

