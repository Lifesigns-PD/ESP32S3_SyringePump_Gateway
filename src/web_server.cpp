#include "web_server.h"
#include "config.h"
#include "settings.h"
#include "wifi_manager.h"
#include "pump_reader.h"
#include "http_sender.h"
#include "ota_update.h"
#include "net_manager.h"
#include "eth_lan.h"
#include "device_id.h"
#include "fleet_ota.h"
#include "diag.h"
#include "pump_scan.h"
#include "hl7_monitor.h"
#include "multi_dev.h"
#include "fleet_logic.h"
#include "utils.h"
#include "web_pages.h"

static volatile uint32_t rebootAtMs = 0;            // set by WiFi add/remove -> reboot after the answer was sent

bool webCheckAuth() {
  if (!server.authenticate(WEB_USER, WEB_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- pages
static void sendPage(const char* title, const char* body) {
  String page;
  page.reserve(9000);
  page += HTML_HEAD_A;
  page += title;
  page += HTML_HEAD_B;
  page += HTML_NAV;
  page += body;
  page += HTML_FOOT;
  server.send(200, "text/html", page);
}

static void pageDashboard() { if (webCheckAuth()) sendPage("Dashboard", PAGE_DASHBOARD); }
static void pageSettings()  { if (webCheckAuth()) sendPage("Settings", PAGE_SETTINGS); }
static void pageOta()       { if (webCheckAuth()) sendPage("Firmware update", PAGE_OTA); }

// ---------------------------------------------------------------- dashboard API
static void apiInfo() {
  if (!webCheckAuth()) return;
  uint32_t s = millis() / 1000;
  const AgiliaStats& st = pumpStats();
  ServerCfg c;
  settingsGetServer(c);

  String t;
  const uint8_t nm = netMode();
  t = String("Network: <b>") + (nm == NET_WIFI ? "WiFi only" : nm == NET_ETH ? "LAN cable only" : "LAN first, WiFi backup") +
      "</b> &nbsp; in use: <b>" + netActiveIface() + "</b> " + netActiveIp() + "<br>";
  if (nm != NET_WIFI)
    t += "LAN: <b>" + htmlEscape(ethStateText()) + "</b> &nbsp; IP: " + ethIpString() + " &nbsp; MAC: " + ethMacString() + "<br>";
  if (nm != NET_ETH) {
    if (wifiIsConnected())
      t += "WiFi: <b>" + htmlEscape(wifiSsid()) + "</b> &nbsp; IP: <b>" + wifiIp().toString() +
           "</b> &nbsp; RSSI: " + String(wifiRssi()) + " dBm";
    else
      t += "WiFi: <b>not connected</b>";
    if (wifiApActive())
      t += " &nbsp; | Setup hotspot: <b>" + htmlEscape(AP_SSID) + "</b> (http://" + wifiApIp().toString() + ")";
    t += "<br>";
  }
  {
    DeviceIdentity id;
    deviceIdGet(id);
    if (id.known)
      t += "Connected device: <b>" + htmlEscape(id.model) + "</b> &nbsp; serial <b>" + htmlEscape(id.serial) + "</b> &nbsp; (" +
           deviceClassText(id.cls) + (id.manual ? ", typed in" : id.live ? ", detected" : ", last seen " + String(id.ageS / 60) + " min ago") + ")<br>";
    else
      t += "Connected device: <b>not known yet</b> (no frame decoded)<br>";
  }

  t += "Pump UART " + String((unsigned long)pumpBaudRate()) + " baud (RX GPIO" + String(pumpRxPin()) + (pumpConsoleOff() ? ", serial console off" : "") + "): bytes <b>" + String((unsigned long)st.bytes) +
       "</b> &nbsp; frames OK <b>" + String((unsigned long)st.framesOk) +
       "</b> &nbsp; CRC errors " + String((unsigned long)st.crc8Err) + "/" + String((unsigned long)st.crc16Err) + "<br>";

  t += "Server: <b>" + (c.enabled() ? htmlEscape(c.url()) : String("sending disabled")) + "</b><br>";
  t += "Upload: OK <b>" + String((unsigned long)senderOkCount()) + "</b> &nbsp; failed " + String((unsigned long)senderFailCount()) +
       " &nbsp; dropped " + String((unsigned long)senderDroppedCount()) + " &nbsp; last HTTP code <b>" + String(senderLastCode()) + "</b>";
  if (senderLastResultAgeS() != 0xFFFFFFFFu)
    t += " (" + htmlEscape(httpCodeText(senderLastCode())) + ", " + String((unsigned long)senderLastResultAgeS()) + " s ago)";
  t += "<br>";
  t += "FW " FW_VERSION " &nbsp; Uptime: " + String(s / 3600) + "h " + String((s / 60) % 60) + "m " + String(s % 60) + "s";
  server.send(200, "text/html", t);
}

static void apiLog() {
  if (!webCheckAuth()) return;
  server.send(200, "text/plain", pumpLogText());
}

static void apiLatest() {
  if (!webCheckAuth()) return;
  const std::string j = pumpLatestJson();
  server.send(200, "application/json", j.empty() ? "{}" : j.c_str());
}

// ---------------------------------------------------------------- HL7 patient monitor
static void pageMonitor() { if (webCheckAuth()) sendPage("Patient monitor", PAGE_MONITOR); }

static void apiHl7() {
  if (!webCheckAuth()) return;
  const Hl7Stats s = hl7GetStats();
  const std::string j = hl7LatestJson();
  String o = "{\"port\":" + String((unsigned)HL7_PORT) + ",\"connected\":" + (s.connected ? "true" : "false") +
             ",\"peer\":\"" + jsonEscape(String(s.peer)) + "\",\"clients\":" + String((unsigned long)s.clients) +
             ",\"ok\":" + String((unsigned long)s.msgOk) + ",\"err\":" + String((unsigned long)s.msgErr) +
             ",\"bytes\":" + String((unsigned long)s.bytes) + ",\"age_s\":" +
             (hl7LastMsgAgeS() == 0xFFFFFFFFu ? String("-1") : String((unsigned long)hl7LastMsgAgeS())) +
             ",\"ip\":\"" + netActiveIp() + "\",\"latest\":" + (j.empty() ? String("null") : String(j.c_str())) + "}";
  server.send(200, "application/json", o);
}

static void apiHl7Log() {
  if (!webCheckAuth()) return;
  server.send(200, "text/plain", hl7LogText());
}

// ---------------------------------------------------------------- infusion pump + PCA pump (multi_dev)
static void pageDevices() { if (webCheckAuth()) sendPage("Infusion + PCA pumps", PAGE_DEVICES); }

static bool devFromArg(DevType& t) {
  const String a = server.arg("d");
  if (a == "inf") { t = DEV_INFUSION; return true; }
  if (a == "pca") { t = DEV_PCA; return true; }
  return false;
}

static void apiDev() {
  if (!webCheckAuth()) return;
  DevType t;
  if (!devFromArg(t)) { server.send(400, "text/plain", "use ?d=inf or ?d=pca"); return; }
  const DevStats s = devGetStats(t);
  const std::string j = devLatestJson(t);
  const uint32_t age = devLastAgeS(t);
  String o = String("{\"enabled\":") + (devEnabled(t) ? "true" : "false") + ",\"port\":" + String((unsigned)s.tcpPort) +
             ",\"connected\":" + (s.connected ? "true" : "false") + ",\"peer\":\"" + jsonEscape(String(s.peer)) +
             "\",\"tcp\":" + (s.tcpOn ? "true" : "false") + ",\"uart\":" + (s.uartOn ? "true" : "false") + ",\"rx\":" + String(s.uartRx) + ",\"tx\":" + String(s.uartTx) +
             ",\"clients\":" + String((unsigned long)s.clients) + ",\"lines\":" + String((unsigned long)s.lines) +
             ",\"ok\":" + String((unsigned long)s.ok) + ",\"err\":" + String((unsigned long)s.err) +
             ",\"bytes\":" + String((unsigned long)s.bytes) + ",\"age_s\":" + (age == 0xFFFFFFFFu ? String("-1") : String((unsigned long)age)) +
             ",\"last_error\":\"" + jsonEscape(String(s.lastError)) + "\",\"transport\":\"" + String(s.lastTransport) +
             "\",\"ip\":\"" + netActiveIp() + "\",\"latest\":" + (j.empty() ? String("null") : String(j.c_str())) + "}";
  server.send(200, "application/json", o);
}

static void apiDevLog() {
  if (!webCheckAuth()) return;
  DevType t;
  if (!devFromArg(t)) { server.send(400, "text/plain", "use ?d=inf or ?d=pca"); return; }
  server.send(200, "text/plain", devLogText(t));
}

static void apiSendNow() {
  if (!webCheckAuth()) return;
  ServerCfg c;
  settingsGetServer(c);
  if (pumpLatestJson().empty()) { server.send(404, "text/plain", "No pump frame decoded yet"); return; }
  if (!c.enabled())             { server.send(400, "text/plain", "Server host is empty - sending disabled"); return; }
  senderEnqueue(pumpLatestJson());
  server.send(200, "text/plain", "Queued - see 'last HTTP code' on the Dashboard");
}

// ---------------------------------------------------------------- server settings API
static void apiServerGet() {
  if (!webCheckAuth()) return;
  ServerCfg c;
  settingsGetServer(c);
  String j = "{\"scheme\":\"" + jsonEscape(c.scheme) + "\",\"host\":\"" + jsonEscape(c.host) +
             "\",\"port\":" + String((unsigned)c.port) + ",\"path\":\"" + jsonEscape(c.path) +
             "\",\"hname\":\"" + jsonEscape(c.hdrName) + "\",\"hval\":\"" + jsonEscape(c.hdrValue) +
             "\",\"url\":\"" + jsonEscape(c.url()) + "\"}";
  server.send(200, "application/json", j);
}

static void apiServerPost() {
  if (!webCheckAuth()) return;
  ServerCfg c;
  c.scheme   = server.arg("scheme");
  c.host     = server.arg("host");
  c.path     = server.arg("path");
  c.hdrName  = server.arg("hname");
  c.hdrValue = server.arg("hval");
  long p = server.arg("port").toInt();
  if (p < 0 || p > 65535) { server.send(400, "text/plain", "Port must be 1-65535"); return; }
  c.port = (uint16_t)p;

  String err;
  if (!settingsSetServer(c, err)) { server.send(400, "text/plain", err); return; }
  ServerCfg saved;
  settingsGetServer(saved);
  Console.printf("Server saved: %s\n", saved.enabled() ? saved.url().c_str() : "(disabled)");
  server.send(200, "text/plain", saved.enabled() ? "Saved: " + saved.url() : String("Saved (sending disabled)"));
}

// ---------------------------------------------------------------- WiFi settings API
static void apiWifiGet() {
  if (!webCheckAuth()) return;
  String j = "{\"connected\":";
  j += wifiIsConnected() ? "true" : "false";
  j += ",\"ssid\":\"" + jsonEscape(wifiSsid()) + "\",\"ip\":\"" + wifiIp().toString() + "\"";
  j += ",\"ap\":";
  j += wifiApActive() ? "true" : "false";
  j += ",\"ap_ssid\":\"" + jsonEscape(AP_SSID) + "\",\"ap_ip\":\"" + wifiApIp().toString() + "\"";

  j += ",\"saved\":[";
  const int n = settingsWifiCount();
  for (int i = 0; i < n; i++) {
    SavedWifi w;
    if (!settingsWifiGet(i, w)) continue;
    if (i) j += ",";
    j += "\"" + jsonEscape(w.ssid) + "\"";
  }
  j += "],\"defaults\":[";
#if USE_DEFAULT_WIFI
  bool firstItem = true;
  for (const auto& d : DEFAULT_WIFI_LIST) {
    if (!firstItem) j += ",";
    firstItem = false;
    j += "\"" + jsonEscape(d.ssid) + "\"";
  }
#endif
  j += "]}";
  server.send(200, "application/json", j);
}

static void apiWifiScan() {
  if (!webCheckAuth()) return;
  if (netMode() == NET_ETH) { server.send(200, "application/json", "{\"status\":\"off\"}"); return; }   // WiFi radio is not started in LAN-only mode
  server.send(200, "application/json", wifiScanJson());
}

static void apiWifiAdd() {
  if (!webCheckAuth()) return;
  String err;
  if (!settingsWifiAdd(server.arg("ssid"), server.arg("pass"), err)) { server.send(400, "text/plain", err); return; }
  Console.printf("WiFi network saved: %s -> rebooting\n", server.arg("ssid").c_str());
  server.send(200, "text/plain", "Saved. Device is rebooting and will join the network (about 15 s). "
                                 "Then open http://" HOSTNAME ".local or the new IP address.");
  rebootAtMs = millis() + 1500;
}

static void apiWifiRemove() {
  if (!webCheckAuth()) return;
  if (!settingsWifiRemove(server.arg("idx").toInt())) { server.send(400, "text/plain", "Unknown entry"); return; }
  Console.println("WiFi network removed -> rebooting");
  server.send(200, "text/plain", "Removed. Device is rebooting (about 15 s).");
  rebootAtMs = millis() + 1500;
}


// ---------------------------------------------------------------- network interface API
static String boolText(bool b) { return b ? "true" : "false"; }

static void apiNetGet() {
  if (!webCheckAuth()) return;
  NetCfg c;
  settingsGetNet(c);
  String j = "{\"mode\":" + String(c.mode) + ",\"ipmode\":" + String(c.ipMode) +
             ",\"ip\":\"" + jsonEscape(c.ip) + "\",\"mask\":\"" + jsonEscape(c.mask) +
             "\",\"gw\":\"" + jsonEscape(c.gw) + "\",\"dns\":\"" + jsonEscape(c.dns) + "\"";
  j += ",\"pins\":{\"sclk\":" + String(c.pSclk) + ",\"miso\":" + String(c.pMiso) + ",\"mosi\":" + String(c.pMosi) + ",\"cs\":" + String(c.pCs) +
       ",\"int\":" + String(c.pInt) + ",\"rst\":" + String(c.pRst) + ",\"mhz\":" + String(c.spiMhz) + "}";
  j += ",\"dhcpwait\":" + String(c.dhcpWaitS) + ",\"probe\":\"" + jsonEscape(ethProbeText()) + "\"";
  j += ",\"macmode\":" + String(c.macMode) + ",\"mac\":\"" + jsonEscape(c.mac) + "\",\"refmac\":\"" ETH_REF_MAC "\"";
  j += ",\"eth_built\":" + boolText(ETH_ENABLED);
  j += ",\"running_mode\":" + String(netMode());
  j += ",\"active\":\"" + String(netActiveIface()) + "\",\"active_ip\":\"" + netActiveIp() + "\"";
  j += ",\"eth\":{\"started\":" + boolText(ethStarted()) + ",\"link\":" + boolText(ethLinkUp()) + ",\"has_ip\":" + boolText(ethHasIp()) +
       ",\"state\":\"" + jsonEscape(ethStateText()) + "\",\"ip\":\"" + ethIpString() + "\",\"mask\":\"" + ethMaskString() +
       "\",\"gw\":\"" + ethGatewayString() + "\",\"mac\":\"" + ethMacString() + "\",\"fallback\":" + boolText(ethUsingStaticFallback()) + "}";
  j += ",\"wifi\":{\"connected\":" + boolText(wifiIsConnected()) + ",\"ssid\":\"" + jsonEscape(wifiSsid()) + "\",\"ip\":\"" + wifiIp().toString() + "\"}";
  j += "}";
  server.send(200, "application/json", j);
}

static void apiNetPost() {
  if (!webCheckAuth()) return;
  NetCfg c;
  settingsGetNet(c);                                   // pins keep their saved value unless the request sends them
  c.mode   = (uint8_t)server.arg("mode").toInt();
  c.ipMode = (uint8_t)server.arg("ipmode").toInt();
  c.ip     = server.arg("ip");
  c.mask   = server.arg("mask");
  c.gw     = server.arg("gw");
  c.dns    = server.arg("dns");
  if (server.hasArg("sclk")) c.pSclk = (int8_t)server.arg("sclk").toInt();
  if (server.hasArg("miso")) c.pMiso = (int8_t)server.arg("miso").toInt();
  if (server.hasArg("mosi")) c.pMosi = (int8_t)server.arg("mosi").toInt();
  if (server.hasArg("cs"))   c.pCs   = (int8_t)server.arg("cs").toInt();
  if (server.hasArg("int"))  c.pInt  = server.arg("int").length() ? (int8_t)server.arg("int").toInt() : (int8_t)-1;   // empty = INT not wired
  if (server.hasArg("rst"))  c.pRst  = server.arg("rst").length() ? (int8_t)server.arg("rst").toInt() : (int8_t)-1;
  if (server.hasArg("mhz"))  c.spiMhz = (uint8_t)server.arg("mhz").toInt();
  if (server.hasArg("dhcpwait")) c.dhcpWaitS = (uint8_t)(server.arg("dhcpwait").toInt() < 0 ? 0 : server.arg("dhcpwait").toInt() > 255 ? 255 : server.arg("dhcpwait").toInt());
  if (server.hasArg("macmode")) c.macMode = server.arg("macmode") == "1" ? 1 : 0;
  if (server.hasArg("mac")) c.mac = server.arg("mac");
  String err;
  if (!settingsSetNet(c, err)) { server.send(400, "text/plain", err); return; }
  Console.printf("Network settings saved: mode %u, LAN address mode %u -> rebooting\n", c.mode, c.ipMode);
  String msg = "Saved. The device is rebooting (about 15 s). ";
  if (c.mode == NET_WIFI) msg += "Then open http://" HOSTNAME ".local or the WiFi IP address.";
  else if (c.ipMode == ETHIP_STATIC) msg += "Then open http://" + c.ip + " (set your PC to an address in the same network, for example " + c.gw + ").";
  else if (c.ipMode == ETHIP_DHCP_THEN_STATIC) msg += "It asks DHCP for an address; if none answers within " + String(ETH_DHCP_TIMEOUT_MS / 1000) + " s it uses " + c.ip + ".";
  else msg += "Then open http://" HOSTNAME ".local or the address your DHCP server gave it.";
  server.send(200, "text/plain", msg);
  rebootAtMs = millis() + 1500;
}

// ---------------------------------------------------------------- connected device API
static String identityJson(const DeviceIdentity& id) {
  return "{\"known\":" + boolText(id.known) + ",\"live\":" + boolText(id.live) + ",\"manual\":" + boolText(id.manual) +
         ",\"cls\":\"" + jsonEscape(id.cls) + "\",\"cls_text\":\"" + deviceClassText(id.cls) + "\",\"model\":\"" + jsonEscape(id.model) +
         "\",\"serial\":\"" + jsonEscape(id.serial) + "\",\"version\":\"" + jsonEscape(id.version) +
         "\",\"age_s\":" + String(id.ageS) + ",\"infusing\":" + boolText(id.infusing) + "}";
}

static void apiDeviceGet() {
  if (!webCheckAuth()) return;
  DeviceIdentity id;
  deviceIdGet(id);
  DeviceCfg c;
  settingsGetDevice(c);
  String j = "{\"effective\":" + identityJson(id) + ",\"cfg\":{\"manual\":" + boolText(c.manual) + ",\"cls\":\"" + jsonEscape(c.cls) +
             "\",\"model\":\"" + jsonEscape(c.model) + "\",\"serial\":\"" + jsonEscape(c.serial) + "\"}}";
  server.send(200, "application/json", j);
}

static void apiDevicePost() {
  if (!webCheckAuth()) return;
  DeviceCfg c;
  c.manual = server.arg("manual") == "1";
  c.cls    = server.arg("cls");
  c.model  = server.arg("model");
  c.serial = server.arg("serial");
  String err;
  if (!settingsSetDevice(c, err)) { server.send(400, "text/plain", err); return; }
  server.send(200, "text/plain", c.manual ? "Saved: this gateway now reports " + c.model + " " + c.serial : String("Saved: the device is detected automatically"));
}

// ---------------------------------------------------------------- server-managed update API
static void apiFleetGet() {
  if (!webCheckAuth()) return;
  FleetCfg c;
  settingsGetFleet(c);
  ServerCfg s;
  settingsGetServer(s);
  String derived = s.enabled() ? (s.scheme + "://" + s.host + (s.port ? ":" + String((unsigned)s.port) : String(""))) : String("");
  String j = "{\"cfg\":{\"mode\":" + String(c.mode) + ",\"base\":\"" + jsonEscape(c.baseUrl) + "\",\"path\":\"" + jsonEscape(c.path) +
             "\",\"interval\":" + String(c.intervalMin) + ",\"derived_base\":\"" + jsonEscape(derived) + "\"},\"status\":" + fleetStatusJson() + "}";
  server.send(200, "application/json", j);
}

static void apiFleetPost() {
  if (!webCheckAuth()) return;
  FleetCfg c;
  c.mode        = (uint8_t)server.arg("mode").toInt();
  c.baseUrl     = server.arg("base");
  c.path        = server.arg("path");
  c.intervalMin = (uint16_t)server.arg("interval").toInt();
  String err;
  if (!settingsSetFleet(c, err)) { server.send(400, "text/plain", err); return; }
  server.send(200, "text/plain", c.mode == 0 ? "Saved: server-managed update is OFF" :
                                 c.mode == 1 ? "Saved: the gateway will check for updates and wait for you to install" :
                                               "Saved: the gateway will check and install updates automatically");
}

static void apiFleetCheck() {
  if (!webCheckAuth()) return;
  FleetCfg c;
  settingsGetFleet(c);
  if (c.mode == 0) { server.send(400, "text/plain", "Switch server-managed update on first (mode is Off)"); return; }
  fleetCheckNow();
  server.send(200, "text/plain", "Checking ...");
}

static void apiFleetInstall() {
  if (!webCheckAuth()) return;
  if (!fleetInstallNow()) { server.send(400, "text/plain", "No update is offered. Press Check now first."); return; }
  server.send(200, "text/plain", "Installing ... the device reboots when it is done");
}


// ---------------------------------------------------------------- pump serial input
static void apiPumpGet() {
  if (!webCheckAuth()) return;
  PumpCfg c;
  settingsGetPump(c);
  const AgiliaStats& st = pumpStats();
  const uint32_t age = pumpLastFrameAgeS();
  String j = "{\"cfg\":{\"rx\":" + String(c.rx) + ",\"tx\":" + String(c.tx) + ",\"baud\":" + String((unsigned long)c.baud) + ",\"auto\":" + (c.autoFind ? "true" : "false") + ",\"console\":" + (c.console ? "true" : "false") + "}";
  j += ",\"live\":{\"rx\":" + String(pumpRxPin()) + ",\"baud\":" + String((unsigned long)pumpBaudRate()) + ",\"console_off\":" + (pumpConsoleOff() ? "true" : "false") +
       ",\"bytes\":" + String((unsigned long)st.bytes) + ",\"frames\":" + String((unsigned long)st.framesOk) + ",\"crc\":" + String((unsigned long)(st.crc8Err + st.crc16Err)) +
       ",\"age_s\":" + (age == 0xFFFFFFFFu ? String("-1") : String((unsigned long)age)) + "}";
  j += ",\"scan\":" + pumpScanJson() + "}";
  server.send(200, "application/json", j);
}

static void apiPumpPost() {
  if (!webCheckAuth()) return;
  PumpCfg c;
  settingsGetPump(c);
  c.rx = (int8_t)server.arg("rx").toInt();
  c.tx = server.arg("tx").length() ? (int8_t)server.arg("tx").toInt() : (int8_t)-1;
  c.baud = (uint32_t)server.arg("baud").toInt();
  c.autoFind = server.arg("auto") == "1";
  c.console = server.arg("console") == "1";
  String err;
  if (!settingsSetPump(c, err)) { server.send(400, "text/plain", err); return; }
  Console.printf("Pump serial input saved: RX=GPIO%d TX=%d baud=%u -> rebooting\n", c.rx, c.tx, (unsigned)c.baud);
  server.send(200, "text/plain", "Saved. The device is rebooting (about 15 s).");
  rebootAtMs = millis() + 1500;
}

static void apiPumpScan() {
  if (!webCheckAuth()) return;
  if (!pumpScanStart()) { server.send(409, "text/plain", "A scan is already running"); return; }
  server.send(200, "text/plain", "Scan started (about 15 seconds)");
}

// ---------------------------------------------------------------- connection test + LAN chip scan
static void apiDiagGet() {
  if (!webCheckAuth()) return;
  server.send(200, "application/json", diagJson());
}

static void apiDiagRun() {
  if (!webCheckAuth()) return;
  ServerCfg sc;
  settingsGetServer(sc);
  String host = server.arg("host");
  host.trim();
  if (host.length() == 0) host = sc.host;
  long port = server.arg("port").toInt();
  if (port <= 0 || port > 65535) port = sc.port ? sc.port : (sc.scheme == "https" ? 443 : 80);
  if (host.length() == 0) { server.send(400, "text/plain", "Enter a host / IP address to test (or set the Server card first)"); return; }
  if (!diagStart(host, (uint16_t)port, server.arg("post") == "1")) { server.send(409, "text/plain", "A test is already running"); return; }
  server.send(200, "text/plain", "Test started");
}

static void apiLanScan() {
  if (!webCheckAuth()) return;
  server.send(200, "application/json", ethScanPresetsJson());       // takes about a second per pin set
}

// ---------------------------------------------------------------- start
void webBegin() {
  server.on("/",         WEB_GET, pageDashboard);
  server.on("/settings", WEB_GET, pageSettings);
  server.on("/ota",      WEB_GET, pageOta);

  server.on("/monitor",    WEB_GET,  pageMonitor);
  server.on("/api/hl7",    WEB_GET,  apiHl7);
  server.on("/api/hl7/log", WEB_GET, apiHl7Log);
  server.on("/devices",    WEB_GET,  pageDevices);
  server.on("/api/dev",    WEB_GET,  apiDev);
  server.on("/api/devlog", WEB_GET,  apiDevLog);
  server.on("/api/info",   WEB_GET,  apiInfo);
  server.on("/api/log",    WEB_GET,  apiLog);
  server.on("/api/latest", WEB_GET,  apiLatest);
  server.on("/api/sendnow", WEB_POST, apiSendNow);

  server.on("/api/server", WEB_GET,  apiServerGet);
  server.on("/api/server", WEB_POST, apiServerPost);

  server.on("/api/wifi",        WEB_GET,  apiWifiGet);
  server.on("/api/wifi/scan",   WEB_GET,  apiWifiScan);
  server.on("/api/wifi/add",    WEB_POST, apiWifiAdd);
  server.on("/api/wifi/remove", WEB_POST, apiWifiRemove);

  server.on("/api/net",    WEB_GET,  apiNetGet);
  server.on("/api/net",    WEB_POST, apiNetPost);
  server.on("/api/device", WEB_GET,  apiDeviceGet);
  server.on("/api/device", WEB_POST, apiDevicePost);
  server.on("/api/pump",      WEB_GET,  apiPumpGet);
  server.on("/api/pump",      WEB_POST, apiPumpPost);
  server.on("/api/pump/scan", WEB_POST, apiPumpScan);
  server.on("/api/diag",      WEB_GET,  apiDiagGet);
  server.on("/api/diag/run",  WEB_POST, apiDiagRun);
  server.on("/api/lan/scan",  WEB_POST, apiLanScan);
  server.on("/api/fleet",         WEB_GET,  apiFleetGet);
  server.on("/api/fleet",         WEB_POST, apiFleetPost);
  server.on("/api/fleet/check",   WEB_POST, apiFleetCheck);
  server.on("/api/fleet/install", WEB_POST, apiFleetInstall);

  server.on("/update", WEB_POST, otaHandleDone, otaHandleUpload);   // firmware upload (ota_update.cpp); optional ?tcls=&tmodel=&tserial= guard

  server.begin(80);
}

void webLoop() {                       // the HTTP server runs in its own task; this only performs a requested reboot
  if (rebootAtMs && (int32_t)(millis() - rebootAtMs) >= 0) gwRestart();
}
