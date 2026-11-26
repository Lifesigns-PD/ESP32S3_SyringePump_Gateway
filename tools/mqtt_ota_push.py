#!/usr/bin/env python3
"""
mqtt_ota_push.py - send an MQTT firmware-update command to a gateway and watch its progress.

  pip install paho-mqtt
  python tools/mqtt_ota_push.py --broker 192.168.50.1 --gw all --file firmware.bin --version 1.3.1 --url http://192.168.50.1:8080/firmware.bin

  --gw      gateway id as printed on its console ("gw-aabbccddeeff") or "all"
  --file    the firmware.bin (used to compute sha256 + size; it is not uploaded by this script)
  --url     where the GATEWAY downloads it from (serve the file with:  python -m http.server 8080  in its folder)
  --status  only ask the gateway(s) to publish their "online" info and print what comes back
Optional: --user/--password, --model/--serial/--class (target patterns, * and ?), --force, --id (job id), --prefix
"""
import argparse, hashlib, json, os, sys, time
import paho.mqtt.client as mqtt

ap = argparse.ArgumentParser()
ap.add_argument("--broker", required=True)
ap.add_argument("--port", type=int, default=1883)
ap.add_argument("--user"); ap.add_argument("--password")
ap.add_argument("--prefix", default="agilia-gw")
ap.add_argument("--gw", default="all")
ap.add_argument("--file"); ap.add_argument("--version"); ap.add_argument("--url")
ap.add_argument("--force", action="store_true")
ap.add_argument("--model", default=""); ap.add_argument("--serial", default=""); ap.add_argument("--cls", default="")
ap.add_argument("--id", default="job-%d" % int(time.time()))
ap.add_argument("--status", action="store_true")
ap.add_argument("--wait", type=int, default=180, help="seconds to watch the status topic")
ap.add_argument("--sha256", help="override the computed sha256 (to test the mismatch case)")
ap.add_argument("--size", type=int, help="override the computed size (to test the size-mismatch case)")
a = ap.parse_args()

if a.status:
    cmd = {"cmd": "status", "id": a.id}
else:
    if not (a.file and a.version and a.url):
        sys.exit("--file, --version and --url are required for an update")
    data = open(a.file, "rb").read()
    cmd = {"cmd": "update", "id": a.id, "version": a.version, "url": a.url,
           "sha256": a.sha256 or hashlib.sha256(data).hexdigest(), "size": a.size or len(data), "force": a.force,
           "class": a.cls, "model": a.model, "serial": a.serial}

try:
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
except AttributeError:
    c = mqtt.Client()
if a.user: c.username_pw_set(a.user, a.password)

done = {"v": False}
def on_connect(cl, ud, *args):
    cl.subscribe(f"{a.prefix}/+/ota/status", 1)
    cl.subscribe(f"{a.prefix}/+/status", 1)
    topic = f"{a.prefix}/{a.gw}/ota/cmd"
    cl.publish(topic, json.dumps(cmd), qos=1, retain=False)     # never retained
    print(f">> {topic}\n   {json.dumps(cmd)}")
def on_message(cl, ud, m):
    print(f"<< {m.topic}  {m.payload.decode(errors='replace')}")
    try:
        j = json.loads(m.payload)
    except Exception:
        return
    if j.get("id") == a.id and j.get("state") in ("rejected", "failed", "ok", "bad"):
        done["v"] = True
c.on_connect = on_connect
c.on_message = on_message
c.connect(a.broker, a.port, 30)
c.loop_start()
t0 = time.time()
while time.time() - t0 < a.wait and not done["v"]:
    time.sleep(0.5)
c.loop_stop()
