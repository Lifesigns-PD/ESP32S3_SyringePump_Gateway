#!/usr/bin/env python3
"""Tests ota_push.py against fake gateways that behave like the firmware (identity from /api/device, 409 guard on /update).
    python test_ota_push.py
"""
import base64
import hashlib
import http.server
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ota_push import glob_match  # same wildcard rules as the firmware (fleet_logic.cpp, tested in fleet_test.cpp)

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))


class FakeGateway(http.server.BaseHTTPRequestHandler):
    identities = {}      # port -> dict(known, live, cls, model, serial, swapped_to=None)
    uploads = []

    def log_message(self, *a):
        pass

    def _ident(self):
        return FakeGateway.identities[self.server.server_port]

    def _auth(self):
        want = "Basic " + base64.b64encode(b"admin:secret").decode()
        if self.headers.get("Authorization") != want:
            self.send_response(401); self.send_header("Content-Length", "0"); self.end_headers()
            return False
        return True

    def _reply(self, code, text, ctype="text/plain"):
        data = text.encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(data))); self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self._auth():
            return
        if self.path == "/api/device":
            i = self._ident()
            self._reply(200, json.dumps({"effective": {"known": i["known"], "live": i["live"], "manual": False, "cls": i["cls"], "model": i["model"], "serial": i["serial"]}}), "application/json")
        else:
            self._reply(404, "no")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        if not self._auth():
            return
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        i = self._ident()
        real = i.get("swapped_to") or i          # the device that is connected at upload time
        for key, val in (("tcls", real["cls"]), ("tmodel", real["model"]), ("tserial", real["serial"])):
            pat = q.get(key, [""])[0]
            if pat and not glob_match(pat, val):
                self._reply(409, "Update REFUSED: this gateway is connected to %s serial %s: %s does not match" % (real["model"], real["serial"], key))
                return
        m = re.search(rb"\r\n\r\n(.*)\r\n--[^\r\n]+--\r\n$", body, re.S)
        FakeGateway.uploads.append((self.server.server_port, u.query, hashlib.sha256(m.group(1)).hexdigest() if m else None))
        self._reply(200, "Update OK, sha256 x, rebooting")


def start(ident):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeGateway)
    FakeGateway.identities[srv.server_port] = ident
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def run(args):
    p = subprocess.run([sys.executable, os.path.join(HERE, "ota_push.py")] + args + ["--user", "admin", "--password", "secret"],
                       capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


def main():
    vol = start(dict(known=True, live=True, cls="infusion_pump", model="Volumat Agilia", serial="VOL-001"))
    inj = start(dict(known=True, live=True, cls="syringe_pump", model="Injectomat Agilia", serial="018090/22083105X"))
    mon = start(dict(known=False, live=False, cls="", model="", serial=""))
    swap = start(dict(known=True, live=True, cls="infusion_pump", model="Volumat Agilia", serial="VOL-002",
                      swapped_to=dict(cls="syringe_pump", model="Injectomat Agilia", serial="018090/1")))
    hosts = ["127.0.0.1:%d" % s.server_port for s in (vol, inj, mon)]
    fw = os.path.join(tempfile.mkdtemp(), "firmware.bin")
    payload = b"\xe9" + os.urandom(5000)
    open(fw, "wb").write(payload)
    sha = hashlib.sha256(payload).hexdigest()

    print("Filter by model")
    FakeGateway.uploads.clear()
    rc, out = run([fw, "--hosts"] + hosts + ["--model", "Volumat*"])
    check("Volumat gateway updated", "UPDATED" in out and ("127.0.0.1:%d" % vol.server_port) in out.split("UPDATED")[1].split("\n")[0])
    check("syringe pump gateway skipped, nothing sent", "SKIP" in out and len(FakeGateway.uploads) == 1)
    check("gateway with unknown device skipped", "not known / not live" in out)
    check("upload carried the guard and the exact file", FakeGateway.uploads and "tmodel=Volumat%2A" in FakeGateway.uploads[0][1] and FakeGateway.uploads[0][2] == sha)

    print("Filter by serial")
    FakeGateway.uploads.clear()
    rc, out = run([fw, "--hosts"] + hosts + ["--serial", "018090/*"])
    check("only the serial-matching gateway is updated", len(FakeGateway.uploads) == 1 and FakeGateway.uploads[0][0] == inj.server_port)

    print("Dry run and no filter")
    FakeGateway.uploads.clear()
    rc, out = run([fw, "--hosts"] + hosts[:2] + ["--model", "Volumat*", "--dry-run"])
    check("dry run sends nothing", "WOULD UPDATE" in out and not FakeGateway.uploads)
    rc, out = run([fw, "--hosts"] + hosts[:2])
    check("no filter updates every gateway", len(FakeGateway.uploads) == 2)

    print("The gateway guards itself")
    FakeGateway.uploads.clear()
    rc, out = run([fw, "--hosts", "127.0.0.1:%d" % swap.server_port, "--model", "Volumat*"])
    check("device swapped after the check: gateway answers REFUSED, nothing installed", "REFUSED" in out and not FakeGateway.uploads)

    print("Bad input")
    bad = os.path.join(os.path.dirname(fw), "bad.bin"); open(bad, "wb").write(b"not firmware")
    rc, out = run([bad, "--hosts", hosts[0]])
    check("a file that is not an ESP32 image is refused before anything is sent", rc != 0 and "does not look like" in out)
    p = subprocess.run([sys.executable, os.path.join(HERE, "ota_push.py"), fw, "--hosts", hosts[0], "--user", "admin", "--password", "wrong"], capture_output=True, text=True)
    check("wrong password is reported as an error", "ERROR" in p.stdout and p.returncode == 1)
    rc, out = run([fw, "--hosts", "127.0.0.1:1"])
    check("unreachable gateway is reported, others unaffected", "ERROR" in out)

    print("%d/%d checks passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
