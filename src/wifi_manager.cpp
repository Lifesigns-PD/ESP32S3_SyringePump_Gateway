#include "wifi_manager.h"
#include <string.h>
#include <vector>
#include "config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "eth_lan.h"
#include "net_manager.h"
#include "settings.h"
#include "utils.h"

namespace {
struct Cred { String ssid; String pass; };

std::vector<Cred>        known;
esp_netif_t*             staNetif = nullptr;
esp_netif_t*             apNetif = nullptr;
volatile bool            started = false;
volatile bool            connected = false;              // station has an IP address
volatile bool            apOn = false;
volatile uint32_t        scanStartMs = 0;                // a web-page scan is in progress (do not collide with the connect scan)
volatile int             scanState = 0;                  // 0 none, 1 running, 2 finished (results waiting)
char                     curSsid[33] = "";
}

bool wifiIsConnected() { return connected; }
bool wifiApActive()    { return apOn; }

// ---------------------------------------------------------------- events
static void onWifiEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) { connected = false; }
    else if (id == WIFI_EVENT_SCAN_DONE && scanState == 1) { scanState = 2; }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    connected = true;
  }
}

// ---------------------------------------------------------------- known networks
static int addKnownNetworks() {
  known.clear();
  const int saved = settingsWifiCount();
  for (int i = 0; i < saved; i++) {
    SavedWifi w;
    if (settingsWifiGet(i, w) && w.ssid.length()) known.push_back({w.ssid, w.pass});
  }
#if USE_DEFAULT_WIFI
  for (const auto& d : DEFAULT_WIFI_LIST) {
    bool overridden = false;                              // same SSID saved on the web page wins
    for (int i = 0; i < saved; i++) {
      SavedWifi w;
      if (settingsWifiGet(i, w) && w.ssid == d.ssid) overridden = true;
    }
    if (!overridden) known.push_back({String(d.ssid), String(d.pass)});
  }
#endif
  return (int)known.size();
}

// Like WiFiMulti::run: scan, take the strongest known network that is in range, connect, wait up to timeoutMs for the address.
static bool connectBestKnown(uint32_t timeoutMs) {
  wifi_scan_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.show_hidden = false;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;          // blocking scan, about 2-3 s
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 30) n = 30;
  wifi_ap_record_t rec[30];
  uint16_t m = n;
  if (esp_wifi_scan_get_ap_records(&m, rec) != ESP_OK) return false;

  int bestKnown = -1, bestRssi = -1000;
  for (uint16_t i = 0; i < m; i++) {
    for (size_t k = 0; k < known.size(); k++) {
      if (known[k].ssid == String((const char*)rec[i].ssid) && rec[i].rssi > bestRssi) { bestRssi = rec[i].rssi; bestKnown = (int)k; }
    }
  }
  if (bestKnown < 0) { Console.println("WiFi: none of the known networks is in range"); return false; }

  const Cred& c = known[bestKnown];
  wifi_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  strncpy((char*)cfg.sta.ssid, c.ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
  if (c.pass.length()) {
    strncpy((char*)cfg.sta.password, c.pass.c_str(), sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
  }
  esp_wifi_disconnect();
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  strncpy(curSsid, c.ssid.c_str(), sizeof(curSsid) - 1);
  esp_wifi_connect();
  const uint32_t t0 = millis();
  while (!connected && millis() - t0 < timeoutMs) delay(100);
  return connected;
}

// ---------------------------------------------------------------- setup hotspot
static void startAp() {
  wifi_config_t ap;
  memset(&ap, 0, sizeof(ap));
  strncpy((char*)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
  ap.ap.ssid_len = (uint8_t)strlen(AP_SSID);
  strncpy((char*)ap.ap.password, AP_PASS, sizeof(ap.ap.password) - 1);
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  ap.ap.authmode = strlen(AP_PASS) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  apOn = true;
  Console.printf("WiFi: no network joined -> setup hotspot \"%s\" ON, open http://%s\n", AP_SSID, wifiApIp().toString().c_str());
}

static void stopAp() {
  esp_wifi_set_mode(WIFI_MODE_STA);
  apOn = false;
  Console.println("WiFi: connected -> setup hotspot OFF");
}

// ---------------------------------------------------------------- task
static void wifiTask(void*) {
  bool mdnsOn = false, ntpOn = false, first = true;
  const bool ownServices = (netMode() == NET_WIFI);     // in LAN modes net_manager does NTP + mDNS for both interfaces
  uint32_t downSince = millis(), lastTry = 0;

  for (;;) {
    const bool scanBusy = scanStartMs && (millis() - scanStartMs < 15000);

    if (!connected) {
      if (mdnsOn) { netMdnsStop(); mdnsOn = false; }
      ntpOn = false;

      const uint32_t interval = apOn ? 60000 : 5000;      // retry gently while hotspot is on
      if (known.size() > 0 && !scanBusy && (first || millis() - lastTry >= interval)) {
        first = false;
        lastTry = millis();
        Console.println("WiFi: connecting to a known network ...");
        if (connectBestKnown(10000)) {
          Console.printf("WiFi connected: %s  IP: %s\n", wifiSsid().c_str(), wifiIp().toString().c_str());
        }
      }
#if AP_FALLBACK_ENABLE
      // (LAN first mode: no setup hotspot while the cable works)
      if (!apOn && !connected && !ethHasIp() && millis() - downSince >= AP_START_DELAY_MS) startAp();
#endif
    } else {
      downSince = millis();
      if (apOn) {
        stopAp();
        if (mdnsOn) { netMdnsStop(); mdnsOn = false; }
      }
      if (ownServices && !ntpOn) { netNtpStart(); ntpOn = true; }
      if (ownServices && !mdnsOn && netMdnsStart()) {
        mdnsOn = true;
        Console.printf("Web page: http://%s.local  (or http://%s)\n", HOSTNAME, wifiIp().toString().c_str());
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void wifiManagerBegin() {
  esp_netif_init();
  esp_err_t e = esp_event_loop_create_default();
  (void)e;                                                // already created by the LAN start-up in LAN-first mode
  staNetif = esp_netif_create_default_wifi_sta();
  apNetif  = esp_netif_create_default_wifi_ap();
  esp_netif_set_hostname(staNetif, HOSTNAME);

  wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&wic);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, nullptr);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  esp_wifi_set_ps(WIFI_PS_NONE);                          // no modem sleep (steady latency for the uploads)
  started = true;

  const int n = addKnownNetworks();
  Console.printf("WiFi: %d known network(s)\n", n);
  xTaskCreatePinnedToCore(wifiTask, "wifiTask", 6144, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------- information
static IPAddress netifIp(esp_netif_t* n, int which) {
  if (!n) return IPAddress();
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(n, &ip) != ESP_OK) return IPAddress();
  return IPAddress(which == 0 ? ip.ip.addr : which == 1 ? ip.netmask.addr : ip.gw.addr);
}

String    wifiSsid()    { return connected ? String(curSsid) : String(""); }
IPAddress wifiIp()      { return connected ? netifIp(staNetif, 0) : IPAddress(); }
IPAddress wifiMask()    { return connected ? netifIp(staNetif, 1) : IPAddress(); }
IPAddress wifiGateway() { return connected ? netifIp(staNetif, 2) : IPAddress(); }
IPAddress wifiApIp()    { return started ? netifIp(apNetif, 0) : IPAddress(); }

IPAddress wifiDns() {
  if (!connected || !staNetif) return IPAddress();
  esp_netif_dns_info_t d;
  if (esp_netif_get_dns_info(staNetif, ESP_NETIF_DNS_MAIN, &d) != ESP_OK) return IPAddress();
  return IPAddress(d.ip.u_addr.ip4.addr);
}

int wifiRssi() {
  wifi_ap_record_t ap;
  if (!connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
  return ap.rssi;
}

String wifiMacString() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char b[20];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

// ---------------------------------------------------------------- scan for the web page
// 1st call starts a scan -> {"status":"scanning"}; later calls return the list when it is done.
String wifiScanJson() {
  if (!started) return "{\"status\":\"off\"}";
  if (scanState == 1) return "{\"status\":\"scanning\"}";
  if (scanState == 0) {                                   // no scan yet -> start one (async)
    scanStartMs = millis();
    scanState = 1;
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) { scanState = 0; scanStartMs = 0; return "{\"status\":\"scanning\"}"; }
    return "{\"status\":\"scanning\"}";
  }

  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 30) n = 30;
  wifi_ap_record_t rec[30];
  uint16_t m = n;
  if (esp_wifi_scan_get_ap_records(&m, rec) != ESP_OK) m = 0;

  String out = "{\"status\":\"done\",\"networks\":[";
  bool firstItem = true;
  for (int i = 0; i < m; i++) {
    String ssid((const char*)rec[i].ssid);
    if (ssid.length() == 0) continue;
    bool dup = false;                                     // keep the first (strongest) entry per SSID
    for (int k = 0; k < i; k++) if (String((const char*)rec[k].ssid) == ssid) { dup = true; break; }
    if (dup) continue;
    if (!firstItem) out += ",";
    firstItem = false;
    out += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String((int)rec[i].rssi) +
           ",\"secure\":" + (rec[i].authmode != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  out += "]}";
  scanState = 0;
  scanStartMs = 0;
  return out;
}
