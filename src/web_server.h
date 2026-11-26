/*
 * web_server.h / web_server.cpp  -  HTTP server (port 80): pages + /api/... endpoints
 *
 *   Pages   :  /  /settings  /ota
 *   API     :  /api/info  /api/log  /api/latest  /api/sendnow
 *              /api/server (GET, POST)
 *              /api/wifi (GET)  /api/wifi/scan (GET)  /api/wifi/add (POST)  /api/wifi/remove (POST)
 *              /api/net (GET, POST)        network interface: WiFi / LAN cable, LAN address
 *              /api/device (GET, POST)     connected device: detected or typed in
 *              /api/pump (GET, POST)  /api/pump/scan (POST)   pump serial input: pins, baud, pin finder
 *              /api/diag (GET)  /api/diag/run (POST)  /api/lan/scan (POST)   connection test, LAN chip finder
 *              /api/fleet (GET, POST)  /api/fleet/check (POST)  /api/fleet/install (POST)   server-managed update
 *   OTA     :  /update (POST, optional ?tcls=&tmodel=&tserial= target guard)  -> see ota_update.cpp
 *
 * Everything is protected by the login WEB_USER / WEB_PASS (config.h).
 */
#pragma once
#include "gw_compat.h"
#include "gw_web.h"

// the web server object `server` is declared in gw_web.h

void webBegin();          // register routes + start server
void webLoop();           // call from loop()
bool webCheckAuth();      // true if the request is authenticated (otherwise asks the browser for login)
