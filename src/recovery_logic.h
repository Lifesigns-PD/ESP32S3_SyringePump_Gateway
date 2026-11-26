/*
 * recovery_logic.h / recovery_logic.cpp  -  "switch the power off and on 5 times quickly" counter
 * Pure C++ (no Arduino code), unit-tested on a PC: tools/recovery_test.cpp
 */
#pragma once

// prevCount : the counter saved by the previous start (0 if none)
// powerOn   : this start was a power-on / EN reset (a restart by software, a crash or a watchdog is NOT a power cycle)
// limit     : how many quick power-ons in a row trigger the reset (0 = feature off)
// Returns the counter to save now. doReset becomes true when the limit is reached.
int recoveryNextCount(int prevCount, bool powerOn, int limit, bool& doReset);
