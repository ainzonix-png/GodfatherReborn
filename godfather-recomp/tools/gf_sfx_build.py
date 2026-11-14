"""Build assets/gf_sfx_bodyhit.bin: the game's own bullet-into-body sound for the ragdoll corpses (gf_dialogue.c
gf_sfx_body_hit). Source: godfather_v4/GlobalFolder/audio_global/GAM.str (RefPack -> RWS sound bank GAM.sbk); its
'tads' table lists 440 samples (16-byte entries: index, 0313BABE, offset from the 'tads' tag, FEEDFEED). Sample 226 =
EA-XAS v1 mono 12 kHz (8-byte header: codec 4, rate, sample count; then blocks of 76-byte frames, as in the .snu
streams). Output: "GFSFX1\\0\\0", u32 rate (48000), u32 n, float32[n] mono (linear 4x upsample).
usage: python tools/gf_sfx_build.py <game USRDIR> <out.bin> [sample index, default 226]"""
import os, struct, sys
import numpy as np
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
import gf_str

COEF = [(0, 0), (240, 0), (460, -208), (392, -220)]
def s16(v):
    v &= 0xFFFF
    return v - 65536 if v >= 32768 else v

def decode_xas(d, p, ns):
    """EA-XAS v1 mono blocks at d[p:] -> list of int16 samples (same arithmetic as gf_dialogue.c decode_snu)."""
    pcm = []
    while p + 8 <= len(d) and len(pcm) < ns:
        fl = d[p]; sz = struct.unpack_from('>I', d, p)[0] & 0xFFFFFF
        if sz < 8 or p + sz > len(d): break
        for k in range((sz - 8) // 76):
            fr = d[p + 8 + 76 * k: p + 8 + 76 * (k + 1)]
            for sf in range(4):
                hd = struct.unpack_from('<I', fr, 4 * sf)[0]
                ci = hd & 3; sh = (hd >> 16) & 0xF; h2 = s16(hd & 0xFFF0); h1 = s16((hd >> 16) & 0xFFF0)
                pcm.append(h2); pcm.append(h1)
                for j in range(15):
                    b = fr[0x10 + sf + 4 * j]
                    for nb in ((b >> 4) & 0xF, b & 0xF):
                        if nb > 7: nb -= 16
                        s = ((nb << 12) >> sh) + ((COEF[ci][0] * h1 + COEF[ci][1] * h2 + 128) >> 8)
                        s = max(-32768, min(32767, s)); pcm.append(s); h2 = h1; h1 = s
        p += sz
        if fl & 0x80: break
    return pcm[:ns]

def main(usr, out, index=226):
    src = os.path.join(usr, 'godfather_v4', 'GlobalFolder', 'audio_global', 'GAM.str')
    bank = max((u for o, u in gf_str.chunks(src)), key=len)
    t = bank.find(b'tads')
    if t < 0: sys.exit(f"no 'tads' sample table in {src}")
    n = struct.unpack_from('>I', bank, t + 4)[0]
    if not 0 <= index < n: sys.exit(f"sample {index} out of range (bank has {n})")
    idx, magic, off, tail = struct.unpack_from('>4I', bank, t + 8 + 16 * index)
    if magic != 0x0313BABE: sys.exit("unexpected sample table layout")
    p = t + off
    codec = bank[p] & 0xF; rate = struct.unpack_from('>H', bank, p + 2)[0]; ns = struct.unpack_from('>I', bank, p + 4)[0] & 0x0FFFFFFF
    if codec != 4 or not 8000 <= rate <= 48000: sys.exit(f"sample {index}: unsupported (codec {codec}, rate {rate})")
    pcm = np.array(decode_xas(bank, p + 8, ns), dtype=np.float64) / 32768.0
    m = int(len(pcm) * 48000 / rate)
    x = np.arange(m) * (rate / 48000.0)
    y = np.interp(x, np.arange(len(pcm)), pcm).astype('<f4')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(b'GFSFX1\0\0' + struct.pack('<2I', 48000, len(y)) + y.tobytes())
    print(f"sample {index}: {len(pcm)} samples at {rate} Hz -> {len(y)} at 48000 Hz -> {out}")

if __name__ == '__main__':
    if len(sys.argv) not in (3, 4): sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 226)
