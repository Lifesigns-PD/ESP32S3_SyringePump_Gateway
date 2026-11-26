#include "settings.h"
#include "config.h"
#include "fleet_logic.h"
#include "eth_pins.h"

static Prefs             prefs;
static SemaphoreHandle_t mux;
static ServerCfg         serverCfg;
static SavedWifi         wifiList[WIFI_MAX_SAVED];
static int               wifiN = 0;
static NetCfg            netCfg;
static DeviceCfg         deviceCfg;
static FleetCfg          fleetCfg;
static PumpCfg           pumpCfg;

PumpCfg::PumpCfg() : rx(PUMP_RX_PIN), tx(PUMP_TX_PIN), baud(PUMP_BAUD), autoFind(PUMP_AUTO_FIND != 0), console(PUMP_CONSOLE_ON != 0) {}

NetCfg::NetCfg() : mode(NET_DEFAULT_MODE), ipMode(ETH_DEFAULT_IP_MODE),
                   ip(ETH_DEFAULT_IP), mask(ETH_DEFAULT_MASK), gw(ETH_DEFAULT_GW), dns(ETH_DEFAULT_DNS),
                   pSclk(ETH_PIN_SCLK), pMiso(ETH_PIN_MISO), pMosi(ETH_PIN_MOSI), pCs(ETH_PIN_CS), pInt(ETH_PIN_INT), pRst(ETH_PIN_RST),
                   spiMhz(ETH_SPI_MHZ), dhcpWaitS(ETH_DHCP_TIMEOUT_MS / 1000), macMode(0), mac(ETH_REF_MAC) {}
FleetCfg::FleetCfg() : mode(FLEET_DEFAULT_MODE), baseUrl(""), path(FLEET_DEFAULT_PATH), intervalMin(FLEET_DEFAULT_INTERVAL_MIN) {}

String ServerCfg::url() const {
  if (host.length() == 0) return String("");
  const uint16_t defPort = (scheme == "https") ? 443 : 80;
  String u = scheme + "://" + host;
  if (port != 0 && port != defPort) u += ":" + String((unsigned)port);
  u += (path.length() ? path : String("/"));
  return u;
}

// Reads a string from flash. Checking isKey() first avoids the harmless but noisy
// "nvs_get_str len fail: ... NOT_FOUND" error message on first boot (key not saved yet).
static String prefStr(const char* key, const char* def) {
  return prefs.isKey(key) ? prefs.getString(key, def) : String(def);
}

// ---------------------------------------------------------------- validation helpers
static bool validHost(const String& h) {
  if (h.length() == 0 || h.length() > 100) return false;
  for (size_t i = 0; i < h.length(); i++) {
    char c = h[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return false;
  }
  return true;
}

static bool validHeaderName(const String& n) {
  if (n.length() > 64) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
  }
  return true;
}

// Old firmware stored a single full URL (key "url"). Split it into scheme/host/port/path.
static void parseUrl(const String& url, ServerCfg& c) {
  String u = url;
  u.trim();
  c.scheme = "http";
  if (u.startsWith("https://"))     { c.scheme = "https"; u = u.substring(8); }
  else if (u.startsWith("http://")) { u = u.substring(7); }
  int slash = u.indexOf('/');
  String hostport = (slash >= 0) ? u.substring(0, slash) : u;
  c.path = (slash >= 0) ? u.substring(slash) : String("/");
  int colon = hostport.lastIndexOf(':');
  c.port = 0;
  if (colon >= 0) {
    c.port = (uint16_t)hostport.substring(colon + 1).toInt();
    c.host = hostport.substring(0, colon);
  } else {
    c.host = hostport;
  }
}

// ---------------------------------------------------------------- flash read / write (mutex already taken)
static void saveServerLocked() {
  prefs.putString("scheme", serverCfg.scheme);
  prefs.putString("host", serverCfg.host);
  prefs.putUShort("port", serverCfg.port);
  prefs.putString("path", serverCfg.path);
  prefs.putString("hname", serverCfg.hdrName);
  prefs.putString("hval", serverCfg.hdrValue);
  prefs.putBool("svset", true);
}

static void saveWifiLocked() {
  for (int i = 0; i < WIFI_MAX_SAVED; i++) {
    String ks = "ws" + String(i), kp = "wp" + String(i);
    prefs.putString(ks.c_str(), i < wifiN ? wifiList[i].ssid : String(""));
    prefs.putString(kp.c_str(), i < wifiN ? wifiList[i].pass : String(""));
  }
}

void settingsBegin() {
  mux = xSemaphoreCreateMutex();
  xSemaphoreTake(mux, portMAX_DELAY);
  prefs.begin("cfg", false);

  // ---- server ----
  serverCfg.hdrName  = prefStr("hname", "");
  serverCfg.hdrValue = prefStr("hval", "");
  if (prefs.getBool("svset", false)) {
    serverCfg.scheme = prefStr("scheme", "https");
    serverCfg.host   = prefStr("host", "");
    serverCfg.port   = prefs.getUShort("port", 0);
    serverCfg.path   = prefStr("path", "/");
  } else {
    String legacy = prefStr("url", "");           // settings of firmware 1.0.x
    if (legacy.length()) {
      parseUrl(legacy, serverCfg);
      saveServerLocked();
    } else {
      serverCfg.scheme = DEFAULT_SERVER_SCHEME;
      serverCfg.host   = DEFAULT_SERVER_HOST;
      serverCfg.port   = DEFAULT_SERVER_PORT;
      serverCfg.path   = DEFAULT_SERVER_PATH;
    }
  }

  // ---- WiFi list ----
  wifiN = 0;
  for (int i = 0; i < WIFI_MAX_SAVED; i++) {
    String ks = "ws" + String(i), kp = "wp" + String(i);
    String s = prefStr(ks.c_str(), "");
    if (s.length()) {
      wifiList[wifiN].ssid = s;
      wifiList[wifiN].pass = prefStr(kp.c_str(), "");
      wifiN++;
    }
  }

  // ---- network interface ----
  netCfg = NetCfg();
  if (prefs.getBool("netset", false)) {
    netCfg.mode   = prefs.getUChar("nmode", NET_DEFAULT_MODE);
    netCfg.ipMode = prefs.getUChar("nipm", ETH_DEFAULT_IP_MODE);
    netCfg.ip     = prefStr("nip", ETH_DEFAULT_IP);
    netCfg.mask   = prefStr("nmask", ETH_DEFAULT_MASK);
    netCfg.gw     = prefStr("ngw", ETH_DEFAULT_GW);
    netCfg.dns    = prefStr("ndns", ETH_DEFAULT_DNS);
    netCfg.pSclk  = prefs.getChar("pclk", ETH_PIN_SCLK);
    netCfg.pMiso  = prefs.getChar("pmis", ETH_PIN_MISO);
    netCfg.pMosi  = prefs.getChar("pmos", ETH_PIN_MOSI);
    netCfg.pCs    = prefs.getChar("pcs", ETH_PIN_CS);
    netCfg.pInt   = prefs.getChar("pint", ETH_PIN_INT);
    netCfg.pRst   = prefs.getChar("prst", ETH_PIN_RST);
    netCfg.spiMhz = prefs.getUChar("pmhz", ETH_SPI_MHZ);
    netCfg.dhcpWaitS = prefs.getUChar("ndhw", ETH_DHCP_TIMEOUT_MS / 1000);
    netCfg.macMode   = prefs.getUChar("nmacm", 0) ? 1 : 0;
    netCfg.mac       = prefStr("nmac", ETH_REF_MAC);
    if (netCfg.dhcpWaitS < 5 || netCfg.dhcpWaitS > 120) netCfg.dhcpWaitS = ETH_DHCP_TIMEOUT_MS / 1000;
    if (netCfg.mode > NET_AUTO) netCfg.mode = NET_DEFAULT_MODE;
    if (netCfg.ipMode > ETHIP_DHCP_THEN_STATIC) netCfg.ipMode = ETH_DEFAULT_IP_MODE;
  }

  // ---- pump serial input ----
  pumpCfg = PumpCfg();
  if (prefs.getBool("uset", false)) {
    pumpCfg.rx       = prefs.getChar("urx", PUMP_RX_PIN);
    pumpCfg.tx       = prefs.getChar("utx", PUMP_TX_PIN);
    pumpCfg.baud     = prefs.getUInt("ubaud", PUMP_BAUD);
    pumpCfg.autoFind = prefs.getBool("uauto", PUMP_AUTO_FIND != 0);
    pumpCfg.console  = prefs.getBool("ucons", PUMP_CONSOLE_ON != 0);
    if (!pumpPinUsable(pumpCfg.rx, MODULE_PSRAM_OCTAL) || !pumpBaudValid(pumpCfg.baud)) pumpCfg = PumpCfg();   // damaged value: back to the defaults
  }

  // ---- connected device ----
  deviceCfg.manual = prefs.getBool("dman", false);
  deviceCfg.cls    = prefStr("dcls", "other");
  deviceCfg.model  = prefStr("dmod", "");
  deviceCfg.serial = prefStr("dser", "");

  // ---- fleet OTA ----
  fleetCfg = FleetCfg();
  if (prefs.getBool("fset", false)) {
    fleetCfg.mode        = prefs.getUChar("fmode", FLEET_DEFAULT_MODE);
    fleetCfg.baseUrl     = prefStr("fbase", "");
    fleetCfg.path        = prefStr("fpath", FLEET_DEFAULT_PATH);
    fleetCfg.intervalMin = prefs.getUShort("fint", FLEET_DEFAULT_INTERVAL_MIN);
    if (fleetCfg.mode > 2) fleetCfg.mode = 0;
    if (fleetCfg.intervalMin < 1 || fleetCfg.intervalMin > 1440) fleetCfg.intervalMin = FLEET_DEFAULT_INTERVAL_MIN;
  }
  xSemaphoreGive(mux);
}

// ---------------------------------------------------------------- server
void settingsGetServer(ServerCfg& out) {
  xSemaphoreTake(mux, portMAX_DELAY);
  out = serverCfg;
  xSemaphoreGive(mux);
}

bool settingsSetServer(const ServerCfg& in, String& err) {
  ServerCfg c = in;
  c.scheme.trim(); c.scheme.toLowerCase();
  c.host.trim();
  c.path.trim();
  c.hdrName.trim();
  c.hdrValue.trim();

  if (c.scheme != "http" && c.scheme != "https") { err = "Scheme must be http or https"; return false; }
  if (c.host.length() && !validHost(c.host)) { err = "Invalid host (use an IP address or domain name, no http:// and no port)"; return false; }
  if (c.path.length() == 0) c.path = "/";
  if (c.path[0] != '/') c.path = "/" + c.path;
  if (c.path.length() > 200) { err = "Path too long (max 200)"; return false; }
  for (size_t i = 0; i < c.path.length(); i++) {
    if ((unsigned char)c.path[i] <= 0x20) { err = "Path must not contain spaces"; return false; }
  }
  if (!validHeaderName(c.hdrName)) { err = "Invalid header name"; return false; }
  if (c.hdrValue.length() > 400 || c.hdrValue.indexOf('\r') >= 0 || c.hdrValue.indexOf('\n') >= 0) {
    err = "Invalid header value"; return false;
  }

  xSemaphoreTake(mux, portMAX_DELAY);
  serverCfg = c;
  saveServerLocked();
  xSemaphoreGive(mux);
  return true;
}

// ---------------------------------------------------------------- WiFi networks
int settingsWifiCount() {
  xSemaphoreTake(mux, portMAX_DELAY);
  int n = wifiN;
  xSemaphoreGive(mux);
  return n;
}

bool settingsWifiGet(int idx, SavedWifi& out) {
  bool ok = false;
  xSemaphoreTake(mux, portMAX_DELAY);
  if (idx >= 0 && idx < wifiN) { out = wifiList[idx]; ok = true; }
  xSemaphoreGive(mux);
  return ok;
}

bool settingsWifiAdd(const String& ssidIn, const String& pass, String& err) {
  String ssid = ssidIn;
  ssid.trim();
  if (ssid.length() < 1 || ssid.length() > 32) { err = "SSID must be 1-32 characters"; return false; }
  if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 63)) {
    err = "Password must be 8-63 characters (leave empty only for an open network)"; return false;
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  int idx = -1;
  for (int i = 0; i < wifiN; i++) if (wifiList[i].ssid == ssid) idx = i;
  if (idx < 0) {
    if (wifiN >= WIFI_MAX_SAVED) {
      xSemaphoreGive(mux);
      err = "List is full (max " + String(WIFI_MAX_SAVED) + "). Remove a network first.";
      return false;
    }
    idx = wifiN++;
  }
  wifiList[idx].ssid = ssid;
  wifiList[idx].pass = pass;
  saveWifiLocked();
  xSemaphoreGive(mux);
  return true;
}

bool settingsWifiRemove(int idx) {
  bool ok = false;
  xSemaphoreTake(mux, portMAX_DELAY);
  if (idx >= 0 && idx < wifiN) {
    for (int i = idx; i < wifiN - 1; i++) wifiList[i] = wifiList[i + 1];
    wifiN--;
    wifiList[wifiN].ssid = "";
    wifiList[wifiN].pass = "";
    saveWifiLocked();
    ok = true;
  }
  xSemaphoreGive(mux);
  return ok;
}

// ================================================================= network interface
static bool validIp(const String& s, IPAddress& out) {
  return s.length() >= 7 && s.length() <= 15 && out.fromString(s);
}

static bool validMask(const IPAddress& m) {            // contiguous 1-bits, not 0.0.0.0
  uint32_t v = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) | ((uint32_t)m[2] << 8) | m[3];
  if (v == 0) return false;
  uint32_t inv = ~v;
  return (inv & (inv + 1)) == 0;
}

void settingsGetNet(NetCfg& out) {
  xSemaphoreTake(mux, portMAX_DELAY);
  out = netCfg;
  xSemaphoreGive(mux);
}

bool settingsSetNet(const NetCfg& in, String& err) {
  NetCfg c = in;
  c.ip.trim(); c.mask.trim(); c.gw.trim(); c.dns.trim();
  if (c.mode > NET_AUTO)                  { err = "Unknown network mode"; return false; }
  if (c.ipMode > ETHIP_DHCP_THEN_STATIC)  { err = "Unknown LAN address mode"; return false; }
  if (!ETH_ENABLED && c.mode != NET_WIFI) { err = "This firmware was built without LAN support (ETH_ENABLED 0 in config.h)"; return false; }

  IPAddress a, m, g, d;
  const bool needStatic = (c.ipMode != ETHIP_DHCP);
  if (needStatic || c.ip.length()) {
    if (!validIp(c.ip, a))      { err = "LAN IP address is not valid (example 192.168.50.2)"; return false; }
    if (!validIp(c.mask, m) || !validMask(m)) { err = "Subnet mask is not valid (example 255.255.255.0)"; return false; }
    if (a == IPAddress(0, 0, 0, 0) || a == IPAddress(255, 255, 255, 255)) { err = "LAN IP address is not usable"; return false; }
  }
  if (c.gw.length()  && !validIp(c.gw, g))  { err = "Gateway address is not valid"; return false; }
  if (c.dns.length() && !validIp(c.dns, d)) { err = "DNS address is not valid"; return false; }
  if (c.dhcpWaitS < 5 || c.dhcpWaitS > 120) { err = "Wait for DHCP must be 5-120 seconds"; return false; }
  c.mac.trim();
  c.mac.toUpperCase();
  if (c.macMode) {
    unsigned char mb[6];
    std::string me;
    if (!ethMacParse(c.mac.c_str(), mb, me)) { err = me.c_str(); return false; }
  }
  if (c.mode != NET_WIFI) {                                // LAN chip pins only matter when the LAN is used
    std::string pe;
    if (!ethPinsValid(c.pSclk, c.pMiso, c.pMosi, c.pCs, c.pInt, c.pRst, c.spiMhz, pumpCfg.rx, MODULE_PSRAM_OCTAL, CONSOLE_ON_USB, pe)) { err = String("LAN chip pins: ") + pe.c_str(); return false; }
  }

  if (c.mode != NET_WIFI && pumpCfg.tx >= 0 && (pumpCfg.tx == c.pSclk || pumpCfg.tx == c.pMiso || pumpCfg.tx == c.pMosi || pumpCfg.tx == c.pCs || pumpCfg.tx == c.pInt || pumpCfg.tx == c.pRst)) {
    err = "LAN chip pins: one of them is the pump TX pin"; return false;
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  netCfg = c;
  prefs.putUChar("nmode", c.mode);
  prefs.putUChar("nipm", c.ipMode);
  prefs.putString("nip", c.ip);
  prefs.putString("nmask", c.mask);
  prefs.putString("ngw", c.gw);
  prefs.putString("ndns", c.dns);
  prefs.putChar("pclk", c.pSclk);
  prefs.putChar("pmis", c.pMiso);
  prefs.putChar("pmos", c.pMosi);
  prefs.putChar("pcs", c.pCs);
  prefs.putChar("pint", c.pInt);
  prefs.putChar("prst", c.pRst);
  prefs.putUChar("pmhz", c.spiMhz);
  prefs.putUChar("ndhw", c.dhcpWaitS);
  prefs.putUChar("nmacm", c.macMode);
  prefs.putString("nmac", c.mac);
  prefs.putBool("netset", true);
  xSemaphoreGive(mux);
  return true;
}

void settingsResetNet() {
  xSemaphoreTake(mux, portMAX_DELAY);
  netCfg = NetCfg();
  prefs.putBool("netset", false);
  xSemaphoreGive(mux);
}

// ================================================================= connected device
void settingsGetDevice(DeviceCfg& out) {
  xSemaphoreTake(mux, portMAX_DELAY);
  out = deviceCfg;
  xSemaphoreGive(mux);
}

static bool printableText(const String& s) {
  for (size_t i = 0; i < s.length(); i++) if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7F) return false;
  return true;
}

bool settingsSetDevice(const DeviceCfg& in, String& err) {
  DeviceCfg c = in;
  c.cls.trim(); c.model.trim(); c.serial.trim();
  if (c.manual) {
    if (!fleetClassValid(c.cls.c_str()))      { err = "Choose a device class"; return false; }
    if (c.model.length() < 1 || c.model.length() > 40) { err = "Model is required (1-40 characters)"; return false; }
    if (c.serial.length() > 30)               { err = "Serial number too long (max 30)"; return false; }
    if (!printableText(c.model) || !printableText(c.serial)) { err = "Model / serial contain invalid characters"; return false; }
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  deviceCfg = c;
  prefs.putBool("dman", c.manual);
  prefs.putString("dcls", c.cls);
  prefs.putString("dmod", c.model);
  prefs.putString("dser", c.serial);
  xSemaphoreGive(mux);
  return true;
}

// ================================================================= fleet OTA
void settingsGetFleet(FleetCfg& out) {
  xSemaphoreTake(mux, portMAX_DELAY);
  out = fleetCfg;
  xSemaphoreGive(mux);
}

bool settingsSetFleet(const FleetCfg& in, String& err) {
  FleetCfg c = in;
  c.baseUrl.trim(); c.path.trim();
  if (c.mode > 2) { err = "Unknown update mode"; return false; }
  while (c.baseUrl.endsWith("/")) c.baseUrl.remove(c.baseUrl.length() - 1);
  if (c.baseUrl.length()) {
    if (!(c.baseUrl.startsWith("http://") || c.baseUrl.startsWith("https://"))) { err = "Update server URL must start with http:// or https://"; return false; }
    if (c.baseUrl.length() > 150) { err = "Update server URL too long"; return false; }
    for (size_t i = 0; i < c.baseUrl.length(); i++) if ((unsigned char)c.baseUrl[i] <= 0x20) { err = "Update server URL must not contain spaces"; return false; }
  }
  if (c.path.length() == 0) c.path = FLEET_DEFAULT_PATH;
  if (c.path[0] != '/') c.path = "/" + c.path;
  if (c.path.length() > 100) { err = "Path too long"; return false; }
  for (size_t i = 0; i < c.path.length(); i++) if ((unsigned char)c.path[i] <= 0x20) { err = "Path must not contain spaces"; return false; }
  if (c.intervalMin < 1 || c.intervalMin > 1440) { err = "Interval must be 1-1440 minutes"; return false; }

  xSemaphoreTake(mux, portMAX_DELAY);
  fleetCfg = c;
  prefs.putUChar("fmode", c.mode);
  prefs.putString("fbase", c.baseUrl);
  prefs.putString("fpath", c.path);
  prefs.putUShort("fint", c.intervalMin);
  prefs.putBool("fset", true);
  xSemaphoreGive(mux);
  return true;
}

// ================================================================= small remembered texts
String settingsNoteGet(const char* key) {
  String k = String("n_") + key;
  xSemaphoreTake(mux, portMAX_DELAY);
  String v = prefStr(k.c_str(), "");
  xSemaphoreGive(mux);
  return v;
}

void settingsNoteSet(const char* key, const String& value) {
  String k = String("n_") + key;
  String v = value.length() > 120 ? value.substring(0, 120) : value;
  xSemaphoreTake(mux, portMAX_DELAY);
  if (prefStr(k.c_str(), "") != v) prefs.putString(k.c_str(), v);       // spare the flash: write only on change
  xSemaphoreGive(mux);
}

// ================================================================= pump serial input
void settingsGetPump(PumpCfg& out) {
  xSemaphoreTake(mux, portMAX_DELAY);
  out = pumpCfg;
  xSemaphoreGive(mux);
}

bool settingsSetPump(const PumpCfg& in, String& err) {
  PumpCfg c = in;
  NetCfg n;
  settingsGetNet(n);
  const int lan[6] = {n.pSclk, n.pMiso, n.pMosi, n.pCs, n.pInt, n.pRst};
  std::string pe;
  if (!pumpCfgValid(c.rx, c.tx, (long)c.baud, lan, (n.mode != NET_WIFI) ? 6 : 0, MODULE_PSRAM_OCTAL, pe)) { err = pe.c_str(); return false; }
  if (c.console && pumpUsesUart0Pins(c.rx, c.tx) && !pumpConsoleCanStay(c.rx, c.tx)) { err = "\"Keep the serial console\" needs the console pin GPIO43 (TXD0) free: use RX GPIO44 and no TX pin"; return false; }
  xSemaphoreTake(mux, portMAX_DELAY);
  pumpCfg = c;
  prefs.putChar("urx", c.rx);
  prefs.putChar("utx", c.tx);
  prefs.putUInt("ubaud", c.baud);
  prefs.putBool("uauto", c.autoFind);
  prefs.putBool("ucons", c.console);
  prefs.putBool("uset", true);
  xSemaphoreGive(mux);
  return true;
}
