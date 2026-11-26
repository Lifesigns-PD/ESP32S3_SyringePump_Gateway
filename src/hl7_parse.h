/*
 * hl7_parse.h / hl7_parse.cpp  -  HL7 v2.x message parser for patient-monitor data (ORU^R01 / ORU^R40)
 *
 * Pure C++ (no ESP-IDF, no Arduino): can be unit-tested on a PC.
 *
 * A message is segments separated by CR (0x0D); fields by '|'; components by '^'; repeats by '~'.
 * Segments used here:
 *   MSH  sender, message type, control id         PID  patient id / name (optional, privacy!)
 *   PV1  bed / location                           OBR  observation request (battery of results)
 *   OBX  one measurement: id^text^system | value | unit | ref range | abnormal flag
 */
#pragma once
#include <string>
#include <vector>

struct Hl7Obs {
  std::string code;        // OBX-3.1  e.g. "8867-4" (LOINC) or "MDC_ECG_HEART_RATE"
  std::string name;        // OBX-3.2  e.g. "Heart rate"
  std::string system;      // OBX-3.3  e.g. "LN" / "MDC"
  std::string value;       // OBX-5    (numeric text, or text)
  std::string unit;        // OBX-6.1  e.g. "bpm"
  std::string range;       // OBX-7    e.g. "50-120"
  std::string flag;        // OBX-8    N / H / L / HH / LL / A
  std::string status;      // OBX-11   F final / P preliminary
  std::string time;        // OBX-14   observation date/time
  std::string valueType;   // OBX-2    NM / ST / SN / ...
  std::string subId;       // OBX-4    sub-id, e.g. "1.1.1.1" (IEEE 11073 hierarchy) - used to tell apart NIBP sys/dia/mean
};

struct Hl7Msg {
  bool        valid = false;
  std::string error;                 // reason when !valid
  std::string sendingApp, sendingFacility;
  std::string msgType;               // MSH-9  e.g. "ORU^R01"
  std::string controlId;             // MSH-10 (echoed in the ACK)
  std::string version;               // MSH-12
  std::string msgTime;               // MSH-7
  std::string patientId, patientName;   // PID-3.1 / PID-5 (only filled when parsing with keepPatient = true)
  std::string bed;                   // PV1-3
  std::vector<Hl7Obs> obs;
};

// Parse one message (MLLP framing already removed). Segment separator CR; LF or CRLF is accepted as well.
Hl7Msg hl7Parse(const std::string& raw, bool keepPatient);

// Build the ACK (MSH + MSA) that answers a message. ok=true -> AA (accepted), else AE (error) with text.
std::string hl7BuildAck(const Hl7Msg& m, bool ok, const char* text);

// Decoded result as JSON (what the gateway uploads and the dashboard shows). `gwId` is the gateway id, `rxMs` its uptime.
std::string hl7ToJson(const Hl7Msg& m, const char* gwId, unsigned long rxMs);

// JSON string escape
std::string hl7JsonEscape(const std::string& s);
