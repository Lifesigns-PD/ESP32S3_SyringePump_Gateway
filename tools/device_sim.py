#!/usr/bin/env python3
"""PC simulator for the INFUSION pump and the PCA pump (no hardware needed).

It connects to the gateway over TCP and sends the line protocol of src/dev_proto.h:
  infusion pump -> gateway TCP port 2601      PCA pump -> gateway TCP port 2602
The two pumps run as two independent "servers" (threads), each with its own connection, like two real devices on two UART lines.

  python tools/device_sim.py --host 172.16.22.185                 both pumps
  python tools/device_sim.py --host 172.16.22.185 --device inf    only the infusion pump
  python tools/device_sim.py --host 172.16.22.185 --alarm         add alarms (occlusion, low battery, PCA over-limit)
  python tools/device_sim.py --dry-run                            print the lines, no network
  python tools/device_sim.py --listen 2601 --device inf           act as a gateway stand-in on this PC (self test)
"""
import argparse, socket, threading, time, sys

PORTS = {"inf": 2601, "pca": 2602}
SPEED = 60          # demo speed: 1 real second = 1 simulated minute, so volumes visibly move


def checksum(body):
    c = 0
    for ch in body.encode("ascii"):
        c ^= ch
    return "%02X" % c


def line(body):
    return "$%s*%s\r\n" % (body, checksum(body))


def fmt(x):
    s = ("%.2f" % x).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


class Infusion:
    tag = "inf"

    def __init__(self, alarm):
        self.alarm_mode, self.t0 = alarm, time.time()
        self.vtbi, self.rate, self.infused = 100.0, 25.0, 0.0
        self.last = time.time()
        self.state, self.alarm, self.pressure = "RUNNING", "NONE", 120
        self.reset_next = False

    def step(self):
        now = time.time()
        dt, self.last = (now - self.last) * SPEED, now
        if self.reset_next:                                 # new bag after COMPLETE was reported
            self.infused, self.reset_next = 0.0, False
        cycle = (now - self.t0) % 90
        if self.alarm_mode and 40 <= cycle < 55:
            self.state, self.alarm = "ALARM", "OCCLUSION"
            self.pressure = min(600, 250 + int((cycle - 40) * 25))
        elif self.alarm_mode and 70 <= cycle < 80:
            self.state, self.alarm, self.pressure = "RUNNING", "LOW_BATTERY", 120
        else:
            self.state, self.alarm, self.pressure = "RUNNING", "NONE", 115 + int(cycle) % 10
        if self.state == "RUNNING":
            self.infused = min(self.vtbi, self.infused + self.rate * dt / 3600.0)
        if self.infused >= self.vtbi:
            self.state, self.alarm, self.reset_next = "COMPLETE", "END_OF_INFUSION", True
        rate = 0 if self.state in ("ALARM", "COMPLETE") else self.rate
        return line("INF,INF-0001,%s,%s,%s,%s,%s,%d,Heparin" % (
            self.state, self.alarm, fmt(rate), fmt(self.vtbi), fmt(self.infused), self.pressure))


class Pca:
    tag = "pca"

    def __init__(self, alarm):
        self.alarm_mode, self.t0 = alarm, time.time()
        self.bolus, self.lockout, self.basal, self.limit = 1.0, 6, 0.5, 10.0     # mg, min (shortened below), mg/h, mg/h
        self.given, self.demands, self.delivered = 0.0, 0, 0
        self.last, self.last_bolus = time.time(), 0.0
        self.next_demand = time.time() + 8

    def step(self):
        now = time.time()
        dt, self.last = (now - self.last) * SPEED, now
        self.given += self.basal * dt / 3600.0
        state, alarm = "RUNNING", "NONE"
        if now >= self.next_demand:                         # patient presses the button
            self.demands += 1
            self.next_demand = now + 15 + (self.demands % 3) * 5
            # the lockout is run 1 s per simulated minute so the demo shows both a delivered and a refused demand
            if now - self.last_bolus >= self.lockout * 3:
                self.given += self.bolus
                self.delivered += 1
                self.last_bolus = now
        if self.alarm_mode:
            cycle = (now - self.t0) % 80
            if 50 <= cycle < 62:
                state, alarm = "ALARM", "HOURLY_LIMIT"
            elif 20 <= cycle < 28:
                state, alarm = "PAUSED", "NONE"
        return line("PCA,PCA-0001,%s,%s,Morphine,1,%s,%d,%s,%s,%s,%d,%d" % (
            state, alarm, fmt(self.bolus), self.lockout, fmt(0 if state != "RUNNING" else self.basal),
            fmt(self.limit), fmt(self.given), self.demands, self.delivered))


def run_device(dev, host, port, interval, dry, stop):
    tag = dev.tag
    sock = None
    while not stop.is_set():
        text = dev.step()
        if dry:
            print("[%s] %s" % (tag, text.strip()), flush=True)
        else:
            try:
                if sock is None:
                    sock = socket.create_connection((host, port), timeout=5)
                    sock.settimeout(2)
                    print("[%s] connected to %s:%d" % (tag, host, port), flush=True)
                sock.sendall(text.encode("ascii"))
                try:
                    reply = sock.recv(256).decode("ascii", "replace").strip()
                except socket.timeout:
                    reply = "(no reply)"
                print("[%s] sent %s  ->  %s" % (tag, text.strip(), reply), flush=True)
            except OSError as e:
                print("[%s] connection problem: %s - retry in 3 s" % (tag, e), flush=True)
                if sock:
                    sock.close()
                sock = None
                stop.wait(3)
                continue
        stop.wait(interval)
    if sock:
        sock.close()


def run_device_serial(dev, com, baud, interval, stop):
    """Send the lines out of a Windows USB-TTL adapter (TX pin -> ESP32 RX pin). No reply comes back: the gateway never transmits."""
    import serial                                           # pip install pyserial
    tag = dev.tag
    ser = None
    while not stop.is_set():
        try:
            if ser is None:
                ser = serial.Serial(com, baud, bytesize=8, parity="N", stopbits=1, timeout=1)
                print("[%s] opened %s at %d 8N1 (adapter TX -> ESP32 RX)" % (tag, com, baud), flush=True)
            text = dev.step()
            ser.write(text.encode("ascii"))
            ser.flush()
            print("[%s] sent on %s: %s" % (tag, com, text.strip()), flush=True)
        except (serial.SerialException, OSError) as e:
            print("[%s] serial problem: %s - retry in 3 s" % (tag, e), flush=True)
            if ser:
                try:
                    ser.close()
                except Exception:
                    pass
            ser = None
            stop.wait(3)
            continue
        stop.wait(interval)
    if ser:
        ser.close()


def listen_mode(port, tag):
    """Self test: accept the simulator on this PC and answer OK, like the gateway does."""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print("[%s] fake gateway listening on %d" % (tag, port), flush=True)
    c, a = srv.accept()
    while True:
        data = c.recv(512)
        if not data:
            break
        print("[gateway] got:", data.decode().strip(), flush=True)
        c.sendall(b"OK\r\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1", help="gateway IP address")
    ap.add_argument("--device", choices=["inf", "pca", "both"], default="both")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between lines")
    ap.add_argument("--alarm", action="store_true", help="include alarms")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--listen", type=int, metavar="PORT", help="self test: act as the gateway on this port")
    ap.add_argument("--com-inf", metavar="COMx", help="send the infusion pump over this USB-TTL adapter (UART mode, no TCP)")
    ap.add_argument("--com-pca", metavar="COMx", help="send the PCA pump over this USB-TTL adapter (UART mode, no TCP)")
    ap.add_argument("--baud", type=int, default=115200, help="UART baud rate (default 115200, must match config.h)")
    ap.add_argument("--port-inf", type=int, default=PORTS["inf"])
    ap.add_argument("--port-pca", type=int, default=PORTS["pca"])
    a = ap.parse_args()
    if a.listen:
        listen_mode(a.listen, a.device)
        return
    stop = threading.Event()
    if a.com_inf or a.com_pca:                              # UART mode: only the pumps that were given a COM port
        try:
            import serial                                   # noqa: F401
        except ImportError:
            sys.exit("UART mode needs pyserial. Install it once with:  python -m pip install pyserial")
        devs = []
        th = []
        for dev, com in ((Infusion(a.alarm), a.com_inf), (Pca(a.alarm), a.com_pca)):
            if com:
                devs.append((dev, com))
                th.append(threading.Thread(target=run_device_serial, args=(dev, com, a.baud, a.interval, stop), daemon=True))
    else:
        devs = []
        if a.device in ("inf", "both"):
            devs.append((Infusion(a.alarm), a.port_inf))
        if a.device in ("pca", "both"):
            devs.append((Pca(a.alarm), a.port_pca))
        th = [threading.Thread(target=run_device, args=(d, a.host, p, a.interval, a.dry_run, stop), daemon=True) for d, p in devs]
    for t in th:
        t.start()
    print("simulating:", ", ".join(d.tag for d, _ in devs), "- Ctrl+C to stop", flush=True)
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        stop.set()
        for t in th:
            t.join(2)


if __name__ == "__main__":
    main()
