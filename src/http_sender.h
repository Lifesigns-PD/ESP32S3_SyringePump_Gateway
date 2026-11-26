/*
 * http_sender.h / http_sender.cpp  -  POSTs the JSON to the configured server
 *
 * Own FreeRTOS task + queue, so a slow/unreachable server never blocks UART reading.
 * Target comes from settings (Settings page -> Server card): scheme, host/IP, port, path, header.
 */
#pragma once
#include "gw_compat.h"
#include <string>

void     senderBegin();
void     senderEnqueue(const std::string& json);   // ignored if server host is empty
uint32_t senderOkCount();
uint32_t senderFailCount();
uint32_t senderDroppedCount();
int      senderLastCode();                         // last HTTP code (negative = local error)
uint32_t senderLastResultAgeS();                   // seconds since the last attempt finished (0xFFFFFFFF = none yet)
