"""Godfather .str (SToc) reader: RefPack (0x10FB) chunks -> raw data. Used to find HUD/weapon textures.
usage: python gf_str.py find <file.str ...>  (library: refpack(), chunks(path))"""
import struct, sys, os
def refpack(d, o=0):
    fl = d[o]; assert d[o+1] == 0xFB, hex(d[o+1])
    if fl & 0x80: n = 4; size = int.from_bytes(d[o+2:o+6], 'big')
    else: n = 3; size = int.from_bytes(d[o+2:o+5], 'big')
    p = o + 2 + n + (n if fl & 1 else 0)
    out = bytearray()
    while len(out) < size:
        b0 = d[p]
        if b0 < 0x80:
            b1 = d[p+1]; p += 2; lit = b0 & 3; out += d[p:p+lit]; p += lit
            ln = ((b0 >> 2) & 7) + 3; off = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = d[p+1], d[p+2]; p += 3; lit = b1 >> 6; out += d[p:p+lit]; p += lit
            ln = (b0 & 0x3F) + 4; off = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = d[p+1], d[p+2], d[p+3]; p += 4; lit = b0 & 3; out += d[p:p+lit]; p += lit
            ln = ((b0 & 0x0C) << 6) + b3 + 5; off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            lit = ((b0 & 0x1F) + 1) << 2; p += 1; out += d[p:p+lit]; p += lit; continue
        else:
            lit = b0 & 3; p += 1; out += d[p:p+lit]; p += lit; break
        s = len(out) - off
        for i in range(ln): out.append(out[s + i])
    return bytes(out)
def chunks(path):
    d = open(path, 'rb').read(); i = 0
    while True:
        i = d.find(b'\x10\xfb', i)
        if i < 0: break
        try:
            u = refpack(d, i); yield i, u
        except Exception: pass
        i += 2
