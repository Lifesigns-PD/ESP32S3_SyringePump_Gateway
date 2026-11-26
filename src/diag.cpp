#include "diag.h"
#include "config.h"
#include "settings.h"
#include "net_manager.h"
#include "eth_lan.h"
#include "utils.h"
#include <netdb.h>
#include "http_util.h"
#include "wifi_manager.h"
#include <lwip/sockets.h>
#include <errno.h>
#include <vector>

struct Line { char k; String t; };       // k: i = info, o = ok, w = warning, f = failure

static SemaphoreHandle_t mux = nullptr;
static std::vector<Line> lines;
static volatile bool running = false, done = false;
static String   tHost;
static uint16_t tPort = 0;
static bool     tPost = false;

static void add(char k, const String& t) {
  if (!mux) return;
  xSemaphoreTake(mux, portMAX_DELAY);
  if (lines.size() < 40) lines.push_back({k, t});
  xSemaphoreGive(mux);
  Console.printf("[test] %c %s\n", k, t.c_str());
}

// 0 = connected, otherwise an errno value
static int tcpConnectTest(const IPAddress& ip, uint16_t port, uint32_t timeoutMs, uint32_t& ms) {
  const uint32_t t0 = millis();
  int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { ms = 0; return errno ? errno : ENOMEM; }
  lwip_fcntl(s, F_SETFL, lwip_fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = (uint32_t)ip;
  int err = 0;
  if (lwip_connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) {
    if (errno == EINPROGRESS) {
      fd_set wf;
      FD_ZERO(&wf);
      FD_SET(s, &wf);
      struct timeval tv;
      tv.tv_sec = timeoutMs / 1000;
      tv.tv_usec = (timeoutMs % 1000) * 1000;
      int n = lwip_select(s + 1, nullptr, &wf, nullptr, &tv);
      if (n == 0) err = ETIMEDOUT;
      else if (n < 0) err = errno;
      else { socklen_t l = sizeof(err); lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l); }
    } else {
      err = errno;
    }
  }
  ms = millis() - t0;
  lwip_close(s);
  return err;
}

// DNS lookup through lwIP (works on the LAN and the WiFi interface alike)
static bool resolveHost(const String& host, IPAddress& out) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
  out = IPAddress(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(res);
  return true;
}

static void runTest() {
  lines.clear();
  const uint8_t mode = netMode();
  add('i', String("Network mode: ") + (mode == NET_WIFI ? "WiFi only" : mode == NET_ETH ? "LAN cable only" : "LAN first, WiFi backup"));
  if (mode != NET_WIFI) {
    add('i', "LAN chip: " + ethProbeText());
    add('i', "LAN state: " + ethStateText());
  }
  if (!netIsUp()) {
    add('f', "The gateway has NO IP address on the interface it uses, so nothing can be sent.");
    if (mode != NET_WIFI && !ethStarted()) add('f', "The LAN chip did not start: use \"Find the LAN chip\" / check the pins (LAN chip pins on this page).");
    else if (mode != NET_WIFI && !ethLinkUp()) add('f', "No LAN link: check the cable, the switch port and the link LEDs.");
    else if (mode != NET_WIFI) add('f', "LAN link is up but no address yet: the DHCP server did not answer. Wait, or check that the switch port gives out addresses.");
    return;
  }
  IPAddress me, mask, gw;
  me.fromString(netActiveIp());
  mask.fromString(netActiveMask());
  gw.fromString(netActiveGateway());
  add('o', String("In use: ") + netActiveIface() + "   IP " + netActiveIp() + "   mask " + netActiveMask() + "   gateway " + netActiveGateway() + "   DNS " + netActiveDns());
  if (ethUsingStaticFallback())
    add('w', "This is the STATIC fallback address, not an address from your network: DHCP did not answer in time. On an office network choose \"DHCP\" (not \"DHCP first, static...\"), raise \"Wait for DHCP\", or - if the network only allows known devices - set the LAN MAC address to the one your network knows.");

  // ---- target
  IPAddress target;
  if (!target.fromString(tHost)) {
    add('i', "Looking up the name " + tHost + " ...");
    if (!resolveHost(tHost, target)) { add('f', "DNS lookup failed for \"" + tHost + "\". Use the IP address, or check the DNS server above."); return; }
    add('o', tHost + " = " + target.toString());
  }
  const bool same = (((uint32_t)me) & ((uint32_t)mask)) == (((uint32_t)target) & ((uint32_t)mask));
  if (same) add('o', "The target " + target.toString() + " is on the SAME network as the gateway (direct, no router needed).");
  else if (gw == IPAddress(0, 0, 0, 0)) { add('f', "The target " + target.toString() + " is on ANOTHER network and the gateway has no router (gateway address 0.0.0.0): it cannot reach it. Set a gateway address, or put both on the same network."); return; }
  else add('w', "The target " + target.toString() + " is on ANOTHER network than the gateway: packets go through the router " + netActiveGateway() + ". Your office network must route AND allow them (wired network to Wi-Fi network is often blocked).");

  // ---- TCP
  add('i', "Connecting to " + target.toString() + " port " + String(tPort) + " ...");
  uint32_t ms = 0;
  const int err = tcpConnectTest(target, tPort, 6000, ms);
  if (err == 0) add('o', "TCP connection OK in " + String(ms) + " ms: the server program is reachable.");
  else if (err == ECONNREFUSED) { add('f', "REFUSED after " + String(ms) + " ms: the computer answered but nothing listens on port " + String(tPort) + ". Start the server program (python server.py) and check the port number."); return; }
  else if (err == ETIMEDOUT) {
    add('f', "NO ANSWER after " + String(ms) + " ms. The computer is not answering on port " + String(tPort) + ". Most likely: (1) the Windows firewall on that computer blocks the port (Wi-Fi networks count as Public: add a rule for profile=any), (2) the IP address is wrong - on the computer run ipconfig, (3) the office network blocks traffic between the wired and the Wi-Fi network.");
    return;
  } else if (err == EHOSTUNREACH || err == ENETUNREACH) { add('f', "NO ROUTE (" + String(err) + "): the gateway knows no way to " + target.toString() + ". Check mask and gateway address."); return; }
  else { add('f', "Connection failed, error " + String(err) + " after " + String(ms) + " ms."); return; }

  // ---- optional HTTP test
  if (!tPost) { add('i', "Tick \"also send one test message\" to test the Server path as well."); return; }
  ServerCfg c;
  settingsGetServer(c);
  String scheme = c.scheme.length() ? c.scheme : String("http");
  String path = c.path.length() ? c.path : String("/");
  if (path[0] != '/') path = "/" + path;
  const String url = scheme + "://" + tHost + ":" + String(tPort) + path;
  add('i', "Sending one small test message to " + url + " ...");
  String body = String("{\"schema\":1,\"gateway\":{\"id\":\"") + jsonEscape(String(HOSTNAME) + "-test") + "\",\"fw\":\"" FW_VERSION "\",\"iface\":\"" + netActiveIface() +
                "\",\"ip\":\"" + netActiveIp() + "\"},\"connection_test\":true}";
  String reply;
  const int code = gwHttpRequest(url, true, body.c_str(), body.length(), "application/json", c.hdrName, c.hdrValue, HTTP_TIMEOUT_MS, &reply, 200);
  reply.trim();
  if (reply.length() > 100) reply = reply.substring(0, 100) + "...";
  if (code >= 200 && code < 300) add('o', "Server answered " + String(code) + " (" + httpCodeText(code) + ")" + (reply.length() ? ": " + reply : String("")) + ". The data path works.");
  else add('f', "Server answered / failed with " + String(code) + ": " + httpCodeText(code) + (reply.length() ? "   Reply: " + reply : String("")));
}

static void diagTask(void*) {
  runTest();
  xSemaphoreTake(mux, portMAX_DELAY);
  done = true;
  running = false;
  xSemaphoreGive(mux);
  vTaskDelete(nullptr);
}

void diagBegin() { if (!mux) mux = xSemaphoreCreateMutex(); }

bool diagStart(const String& host, uint16_t port, bool sendPost) {
  diagBegin();
  xSemaphoreTake(mux, portMAX_DELAY);
  if (running) { xSemaphoreGive(mux); return false; }
  running = true;
  done = false;
  lines.clear();
  tHost = host;
  tPort = port;
  tPost = sendPost;
  xSemaphoreGive(mux);
  xTaskCreatePinnedToCore(diagTask, "connTest", 12288, nullptr, 1, nullptr, 0);
  return true;
}

String diagJson() {
  diagBegin();
  xSemaphoreTake(mux, portMAX_DELAY);
  String j = String("{\"running\":") + (running ? "true" : "false") + ",\"done\":" + (done ? "true" : "false") + ",\"lines\":[";
  for (size_t i = 0; i < lines.size(); i++) {
    if (i) j += ",";
    j += String("{\"k\":\"") + lines[i].k + "\",\"t\":\"" + jsonEscape(lines[i].t) + "\"}";
  }
  j += "]}";
  xSemaphoreGive(mux);
  return j;
}
