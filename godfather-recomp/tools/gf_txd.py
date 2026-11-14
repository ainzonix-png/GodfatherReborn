"""List RenderWare texture natives (PS3) inside Godfather .str chunks and compute the TEX_REPLACE key of each.
usage: python gf_txd.py <file.str> [outdir]  -> prints name, size, fmt, key; with outdir writes GFTEX1 .tex files
(<w>x<h>_f<fmt>_<key>.tex, same as TEX_DUMP) so gf_texenc.py can build replacements without dumping in game."""
import sys, os, struct, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import gf_str
def csum(b):
    n = len(b); step = n // 1024 if n > 4096 else 4; h = 2166136261; i = 0
    while i + 3 < n:
        h ^= struct.unpack_from('<I', b, i)[0]; h = (h * 16777619) & 0xFFFFFFFF; i += step
    return h
PAT = re.compile(rb'\x15\x00\x00\x00(....)\x2d\x00\x02\x1c\x01\x00\x00\x00(....)\x2d\x00\x02\x1c', re.S)
def textures(u):
    for m in PAT.finditer(u):
        s = m.end()
        name = u[s+8:s+40].split(b'\0')[0].decode('latin1')
        hd = u[s+72:s+92]
        w, h = struct.unpack_from('>HH', hd, 8); dsz = struct.unpack_from('>I', hd, 16)[0]
        if not (4 <= w <= 4096 and 4 <= h <= 4096): continue
        l0 = w * h
        if dsz >= l0 and hd[2] in (0x83, 0x88, 0xA3, 0xA8) : fmt = 0x87 if hd[2] & 0xF == 3 else 0x88; nb = l0
        elif hd[2] in (0x82, 0xA2, 0x86, 0xA6): fmt = 0x86; nb = l0 // 2
        else: fmt = hd[2]; nb = min(dsz, l0 * 4)
        data = u[s+92:s+92+nb]
        if len(data) < nb: continue
        yield name, w, h, fmt, hd, data
if __name__ == '__main__':
    od = sys.argv[2] if len(sys.argv) > 2 else None
    if od: os.makedirs(od, exist_ok=True)
    d = open(sys.argv[1], 'rb').read()
    for o, u in gf_str.chunks(sys.argv[1]):
        for name, w, h, fmt, hd, data in textures(u):
            key = csum(data) ^ ((w * 2654435761) & 0xFFFFFFFF) ^ (h << 7) ^ fmt
            fn = f"{w}x{h}_f{fmt:02X}_{key:08X}.tex"
            print(f"{o:#09x} {name:32s} {fn}  hd={hd[:8].hex()}")
            if od:
                with open(os.path.join(od, fn), 'wb') as f:
                    f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, fmt, len(data), csum(data)) + data)
