#!/usr/bin/env python3
"""
Push one firmware.bin to several gateways, but only to those connected to the device you mean.

    python ota_push.py firmware.bin --hosts 192.168.50.2 192.168.50.3 --model "Volumat*"
    python ota_push.py firmware.bin --hosts-file gateways.txt --serial "018090/*" --dry-run
    python ota_push.py firmware.bin --hosts 10.1.2.3 --class syringe_pump --user admin --password admin123

For every gateway the tool
  1. asks http://<host>/api/device what the gateway is connected to (model, serial, class),
  2. skips the gateway if that does not match your filters (shown as SKIP, nothing is sent),
  3. otherwise uploads the file with the same filters as a guard (?tcls=&tmodel=&tserial=). The GATEWAY checks them
     again and answers HTTP 409 "REFUSED" if its device does not match - so a wrong list or a device that was swapped
     a second ago cannot get the wrong firmware.
Filters: --class / --model / --serial. '*' = any text, '?' = one character, case ignored. No filter = update all.
Python 3.8+, standard library only.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid


def glob_match(pattern, value):
    if not pattern:
        return True
    rx = "".join(".*" if c == "*" else "." if c == "?" else re.escape(c) for c in pattern)
    return re.fullmatch(rx, value or "", re.I | re.S) is not None


def request(url, user, password, data=None, headers=None, timeout=15):
    req = urllib.request.Request(url, data=data, headers=headers or {}, method="POST" if data is not None else "GET")
    req.add_header("Authorization", "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError) as e:
        return None, str(getattr(e, "reason", e))


def multipart(field, filename, payload):
    boundary = "----agilia" + uuid.uuid4().hex
    head = ('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n" % (boundary, field, filename)).encode()
    tail = ("\r\n--%s--\r\n" % boundary).encode()
    return head + payload + tail, "multipart/form-data; boundary=" + boundary


def push_one(host, args, image):
    base = host if "://" in host else "http://" + host
    code, body = request(base + "/api/device", args.user, args.password)
    if code != 200:
        return "ERROR", "cannot read /api/device (%s %s)" % (code, body[:80])
    try:
        eff = json.loads(body)["effective"]
    except (ValueError, KeyError):
        return "ERROR", "unexpected answer from /api/device"
    what = "%s serial %s (%s)" % (eff.get("model") or "?", eff.get("serial") or "-", eff.get("cls") or "?")
    wanted = args.cls or args.model or args.serial
    if wanted:
        if not eff.get("known") or not eff.get("live"):
            return "SKIP", "device not known / not live on this gateway"
        for pat, val, label in ((args.cls, eff.get("cls"), "class"), (args.model, eff.get("model"), "model"), (args.serial, eff.get("serial"), "serial")):
            if not glob_match(pat, val or ""):
                return "SKIP", "%s - %s does not match '%s'" % (what, label, pat)
    if args.dry_run:
        return "WOULD UPDATE", what
    q = []
    for k, v in (("tcls", args.cls), ("tmodel", args.model), ("tserial", args.serial)):
        if v:
            q.append("%s=%s" % (k, urllib.parse.quote(v, safe="")))
    data, ctype = multipart("firmware", os.path.basename(args.firmware), image)
    code, body = request(base + "/update" + ("?" + "&".join(q) if q else ""), args.user, args.password, data=data,
                         headers={"Content-Type": ctype}, timeout=args.timeout)
    if code == 200:
        return "UPDATED", "%s: %s" % (what, body.strip())
    if code == 409:
        return "REFUSED", "%s: %s" % (what, body.strip())
    return "ERROR", "%s: HTTP %s %s" % (what, code, body.strip()[:120])


def main():
    ap = argparse.ArgumentParser(description="Push firmware to gateways, filtered by device class / model / serial number")
    ap.add_argument("firmware", help="firmware.bin (.pio/build/<environment>/firmware.bin, e.g. wroom1u_4mb)")
    ap.add_argument("--hosts", nargs="*", default=[], help="gateway IP addresses or names")
    ap.add_argument("--hosts-file", help="text file, one gateway per line")
    ap.add_argument("--class", dest="cls", default="", help="syringe_pump | infusion_pump | patient_monitor | other")
    ap.add_argument("--model", default="", help="for example 'Volumat*'")
    ap.add_argument("--serial", default="", help="for example '018090/*'")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin123", help="WEB_PASS from config.h")
    ap.add_argument("--timeout", type=int, default=180, help="seconds to wait for one upload")
    ap.add_argument("--dry-run", action="store_true", help="only show which gateways would be updated")
    args = ap.parse_args()

    hosts = list(args.hosts)
    if args.hosts_file:
        with open(args.hosts_file, encoding="utf-8") as f:
            hosts += [l.strip() for l in f if l.strip() and not l.lstrip().startswith("#")]
    if not hosts:
        ap.error("give --hosts or --hosts-file")
    try:
        with open(args.firmware, "rb") as f:
            image = f.read()
    except OSError as e:
        sys.exit("cannot read firmware: %s" % e)
    if not image or image[0] != 0xE9:
        sys.exit("%s does not look like an ESP32 firmware image (first byte should be 0xE9)" % args.firmware)
    print("Firmware %s, %d bytes, sha256 %s" % (os.path.basename(args.firmware), len(image), hashlib.sha256(image).hexdigest()))
    print("Filter: class='%s' model='%s' serial='%s'%s\n" % (args.cls, args.model, args.serial, "   (dry run)" if args.dry_run else ""))

    counts = {}
    for h in hosts:
        status, text = push_one(h, args, image)
        counts[status] = counts.get(status, 0) + 1
        print("%-14s %-18s %s" % (status, h, text))
    print("\n" + ", ".join("%d %s" % (n, k.lower()) for k, n in sorted(counts.items())))
    sys.exit(0 if not counts.get("ERROR") else 1)


if __name__ == "__main__":
    main()
