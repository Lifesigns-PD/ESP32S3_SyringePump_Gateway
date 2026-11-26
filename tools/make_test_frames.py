#!/usr/bin/env python3
"""Generates synthetic Agilia frames (hex, one per line) following protocol doc CC2777 rev.6.
Usage: python make_test_frames.py | ./native_test"""
import struct, sys

def crc8(b):
    c = 0
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x8C if c & 1 else c >> 1
    return c

def crc16(b):
    c = 0
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c

def frame(payload, ftype=0x60, no=0, ack=0, crc16_on=True):
    hdr = bytes([ftype, no, ack, len(payload)])
    out = bytes([0x1B]) + hdr + bytes([crc8(hdr)]) + payload
    out += struct.pack(">H", crc16(payload) if crc16_on else 0xFFFF)
    return out

def s(text, n):
    return text.encode("cp1252").ljust(n, b"\0")[:n]

def unit(prefix, un, wt=0, tm=0, dil=0, pl=0, surf=0):
    return (surf << 12) | (prefix << 9) | (un << 6) | (wt << 5) | (tm << 3) | (dil << 1) | pl

ML_H = unit(3, 1, tm=2)            # ml/h
MG_ML = unit(3, 0, dil=1)          # mg/ml
UG = unit(2, 0)

def status_frame(alarm=0, full=True, op=2, step=0x10, sw=0x30, mode=1, drug="Heparin", flow=2500):
    p = bytes([0x80, 0x42]) + s("031A", 4) + s("INJ0123456789ABC", 16)
    p += struct.pack(">IBBI", alarm, 0, 3, 1790000000)
    if not full:
        return frame(p + bytes([1, 4]))
    p += struct.pack(">BBBHB", op, 0x06, step, sw, mode)
    sub = bytes([2]) + struct.pack(">I", flow) + s(drug, 20) + struct.pack(">H", ML_H)
    sub += struct.pack(">IHIHHIB", 1234567, 150, 1000, MG_ML, 50, 20000000, 50) + s("50cc Fresenius", 20)
    sub += struct.pack(">IIH", 4321000, 0, 0) + struct.pack(">II", 0xFFFFFFFF, 0xFFFFFFFF)
    p += bytes([len(sub)]) + sub
    sub5 = bytes([5, 0x00]); p += bytes([len(sub5) - 0]) + sub5 if False else b""
    return frame(p)

def advanced_frame():
    p = bytes([0xDF]) + struct.pack(">I", 123456)
    d = bytes([0x07]) + struct.pack(">i", 5000); p += bytes([len(d)]) + d
    d = bytes([0x0C]) + struct.pack(">I", 7500); p += bytes([len(d)]) + d
    d = bytes([0x10]) + struct.pack(">II", 100000, 2000); p += bytes([len(d)]) + d
    p += bytes([0])
    return frame(p, ftype=0x40, no=5, ack=3)

frames = [
    status_frame(),                                           # running, no alarms
    status_frame(alarm=(1 << 4) | (1 << 20) | (1 << 25), step=0x21, sw=0x37, flow=0),   # occlusion alarm
    status_frame(full=False),                                 # idle (short)
    status_frame(drug="Noradr\xe9naline"),                    # CP1252 accent
    advanced_frame(),
]
noise = bytes([0x00, 0x1B, 0x40, 0x55])                       # junk + fake ESC to test resync
sys.stdout.write((noise + frames[0]).hex() + "\n")
for f in frames[1:]:
    sys.stdout.write(f.hex() + "\n")
# corrupted frame (flip a payload byte) must be rejected
bad = bytearray(frames[0]); bad[10] ^= 0xFF
sys.stdout.write("BAD " + bytes(bad).hex() + "\n")
