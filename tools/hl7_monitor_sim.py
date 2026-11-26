#!/usr/bin/env python3
"""
HL7 v2 patient-monitor SIMULATOR (MLLP over TCP) - for demos without a real monitor.

It behaves like a bedside monitor: connects to the gateway, sends one ORU^R01 observation message
every second with heart rate, SpO2, NIBP, respiration, temperature ... (slowly moving values) and
waits for the ACK. Open  http://<gateway-ip>/monitor  to watch it.

    python tools/hl7_monitor_sim.py --host 192.168.50.2            # gateway on the LAN
    python tools/hl7_monitor_sim.py --host 192.168.50.2 --alarm    # periodically drives HR high / SpO2 low (shows red tiles)
    python tools/hl7_monitor_sim.py --dry-run                      # just print one message, no network
    python tools/hl7_monitor_sim.py --listen 2575                  # act as a receiver (to test the sim without the gateway)

Needs only Python 3.8+, no packages. HL7 identifiers are LOINC; change TEMPLATE to match your real monitor.
"""
import argparse, random, socket, sys, time
from datetime import datetime

SB, EB, CR = b"\x0b", b"\x1c", b"\x0d"

# (loinc, name, unit, low, high, start, decimals)
VITALS = [
    ("8867-4",  "Heart rate",                 "bpm",  50, 120, 76, 0),
    ("59408-5", "SpO2",                       "%",    90, 100, 97, 0),
    ("9279-1",  "Respiratory rate",           "/min", 8,  25,  16, 0),
    ("8310-5",  "Body temperature",           "Cel",  36.0, 38.0, 36.8, 1),
    ("8480-6",  "Systolic blood pressure",    "mm[Hg]", 90, 150, 120, 0),
    ("8462-4",  "Diastolic blood pressure",   "mm[Hg]", 55, 95,  78, 0),
    ("8478-0",  "Mean blood pressure",        "mm[Hg]", 65, 110, 92, 0),
]


class Monitor:
    def __init__(self, bed, alarm):
        self.val = {v[0]: float(v[5]) for v in VITALS}
        self.bed, self.alarm, self.n = bed, alarm, 0

    def step(self):
        self.n += 1
        for code, name, unit, lo, hi, start, dec in VITALS:
            span = (hi - lo) * 0.03
            x = self.val[code] + random.uniform(-span, span) + (start - self.val[code]) * 0.05   # random walk pulled to the start value
            self.val[code] = min(hi, max(lo, x))
        if self.alarm and (self.n // 20) % 3 == 2:          # every 3rd block of 20 s: HR high + SpO2 low
            self.val["8867-4"] = random.uniform(128, 140)
            self.val["59408-5"] = random.uniform(84, 88)

    def flag(self, code, v):
        if code == "8867-4":  return "HH" if v > 125 else "H" if v > 110 else "L" if v < 50 else "N"
        if code == "59408-5": return "LL" if v < 90 else "L" if v < 94 else "N"
        if code == "8310-5":  return "H" if v > 37.8 else "L" if v < 36.0 else "N"
        return "N"

    def message(self):
        now = datetime.now().strftime("%Y%m%d%H%M%S")
        ctl = "SIM%06d" % self.n
        seg = [
            "MSH|^~\\&|PM-SIM|DEMO-ICU|AGILIA-GW|LIFESIGNS|%s||ORU^R01^ORU_R01|%s|P|2.5|||AL|NE" % (now, ctl),
            "PID|1||DEMO0001^^^DEMO^MR||Patient^Demo||19700101|M",
            "PV1|1|I|ICU^%s^1" % self.bed,
            "OBR|1|||69000^Monitor vital signs^LN|||%s" % now,
        ]
        for i, (code, name, unit, lo, hi, start, dec) in enumerate(VITALS, 1):
            v = self.val[code]
            txt = ("%.*f" % (dec, v))
            seg.append("OBX|%d|NM|%s^%s^LN||%s|%s|%s-%s|%s|||F|||%s" % (i, code, name, txt, unit, lo, hi, self.flag(code, v), now))
        return "\r".join(seg) + "\r"


def frame(msg):
    return SB + msg.encode("ascii", "replace") + EB + CR


def read_frame(sock, timeout=5):
    sock.settimeout(timeout)
    buf = b""
    while True:
        chunk = sock.recv(1024)
        if not chunk:
            return None
        buf += chunk
        if EB in buf:
            return buf.split(SB)[-1].split(EB)[0].decode("ascii", "replace")


def run_sender(a):
    mon = Monitor(a.bed, a.alarm)
    while True:
        try:
            print("connecting to %s:%d ..." % (a.host, a.port))
            s = socket.create_connection((a.host, a.port), timeout=5)
            print("connected")
            while True:
                mon.step()
                s.sendall(frame(mon.message()))
                ack = read_frame(s)
                if ack is None:
                    raise ConnectionError("gateway closed the connection")
                code = ack.split("\r")[1].split("|")[1] if "\r" in ack else "?"
                print("%s  sent #%d  HR=%.0f SpO2=%.0f  ->  ACK %s" % (time.strftime("%H:%M:%S"), mon.n, mon.val["8867-4"], mon.val["59408-5"], code))
                time.sleep(a.interval)
        except (OSError, ConnectionError) as e:
            print("connection problem: %s - retry in 3 s" % e)
            time.sleep(3)
        except KeyboardInterrupt:
            print("bye")
            return


def run_listener(port):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print("test receiver on TCP %d (Ctrl+C to stop)" % port)
    while True:
        c, addr = srv.accept()
        print("client", addr)
        try:
            while True:
                m = read_frame(c, 30)
                if m is None:
                    break
                print(m.replace("\r", "\n"))
                ctl = m.split("\r")[0].split("|")[9]
                c.sendall(frame("MSH|^~\\&|RX|X|PM-SIM|DEMO||ACK^R01|A%s|P|2.5\rMSA|AA|%s\r" % (ctl, ctl)))
        except OSError:
            pass


if __name__ == "__main__":
    p = argparse.ArgumentParser(description="HL7 v2 patient-monitor simulator (MLLP)")
    p.add_argument("--host", default="192.168.50.2", help="gateway IP (default 192.168.50.2)")
    p.add_argument("--port", type=int, default=2575)
    p.add_argument("--interval", type=float, default=1.0, help="seconds between messages")
    p.add_argument("--bed", default="BED01")
    p.add_argument("--alarm", action="store_true", help="periodically simulate tachycardia + low SpO2")
    p.add_argument("--dry-run", action="store_true", help="print one message and exit")
    p.add_argument("--listen", type=int, metavar="PORT", help="run as a receiver instead")
    a = p.parse_args()
    if a.dry_run:
        m = Monitor(a.bed, a.alarm)
        m.step()
        print(m.message().replace("\r", "\n"))
    elif a.listen:
        run_listener(a.listen)
    else:
        run_sender(a)
