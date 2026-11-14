"""v1043X texture replacement encoder: PNG -> GFTEX1 .tex (DXT1 f86 / DXT3 f87 / DXT5 f88, linear guest block layout,
exactly what tools/gf_tex.py decodes). The output is named after the ORIGINAL dumped texture (texdump/<w>x<h>_f<fmt>_<key>.tex),
so rsx_d3d12_backend.c (TEX_REPLACE) swaps it in when the game uploads that texture.
usage: python gf_texenc.py <image.png> <original texdump .tex> <out dir>"""
import sys, os, struct, numpy as np
from PIL import Image

def to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return ((c[..., 0] * 31 + 127) // 255) << 11 | ((c[..., 1] * 63 + 127) // 255) << 5 | ((c[..., 2] * 31 + 127) // 255)
def from565(v):
    return np.stack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], -1).astype(np.float64)

def color_block(px, w):
    """px 16x3 float, w 16 weights (alpha) -> 8 bytes, 4-colour mode (c0 > c1)."""
    ww = w + 1e-3
    mean = (px * ww[:, None]).sum(0) / ww.sum()
    d = px - mean
    cov = (d * ww[:, None]).T @ d
    try: axis = np.linalg.eigh(cov)[1][:, -1]
    except Exception: axis = np.array([1.0, 1.0, 1.0])
    t = d @ axis
    lo, hi = mean + axis * t.min(), mean + axis * t.max()
    best = None
    for it in range(3):
        c0, c1 = to565(hi), to565(lo)
        if c0 < c1: c0, c1 = c1, c0
        if c0 == c1:
            if c0 > 0: c1 = c0 - 1
            else: c0 = 1
        a, b = from565(np.int32(c0)), from565(np.int32(c1))
        pal = np.stack([a, b, (2 * a + b) / 3, (a + 2 * b) / 3])
        dist = ((px[:, None, :] - pal[None]) ** 2).sum(-1)
        idx = dist.argmin(1)
        err = (dist[np.arange(16), idx] * ww).sum()
        if best is None or err < best[0]: best = (err, c0, c1, idx)
        # least-squares refit of the endpoints for these indices
        wt = np.array([1.0, 0.0, 2 / 3, 1 / 3])[idx]
        A = np.stack([wt, 1 - wt], 1) * np.sqrt(ww)[:, None]
        B = px * np.sqrt(ww)[:, None]
        sol, *_ = np.linalg.lstsq(A, B, rcond=None)
        hi, lo = sol[0], sol[1]
    _, c0, c1, idx = best
    bits = 0
    for i in range(16): bits |= int(idx[i]) << (2 * i)
    return struct.pack('<HHI', int(c0), int(c1), bits)

def encode(img, base):
    h, w = img.shape[:2]
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = img[by:by + 4, bx:bx + 4].reshape(16, 4).astype(np.float64)
            a = blk[:, 3]
            if base == 0x87:
                q = np.clip(np.rint(a / 17), 0, 15).astype(int)
                ab = bytearray(8)
                for i in range(16): ab[i // 2] |= q[i] << (4 * (i & 1))
                out += ab
            elif base == 0x88:
                a0, a1 = int(a.max()), int(a.min())
                if a0 == a1: a0 = min(255, a0 + 1) if a0 < 255 else a0; a1 = a0 - 1 if a0 == a1 else a1
                pal = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
                idx = np.abs(a[:, None] - np.array(pal)[None]).argmin(1)
                bits = 0
                for i in range(16): bits |= int(idx[i]) << (3 * i)
                out += bytes([a0, a1]) + bits.to_bytes(6, 'little')
            out += color_block(blk[:, :3], a / 255.0 if base != 0x86 else np.ones(16))
    return bytes(out)

def main():
    src, orig, outdir = sys.argv[1:4]
    od = open(orig, 'rb').read(); assert od[:6] == b'GFTEX1'
    w, h, fmt, nb, cs = struct.unpack('<5I', od[8:28])
    base = fmt & 0x9F
    assert base in (0x86, 0x87, 0x88), hex(fmt)
    im = Image.open(src).convert('RGBA')
    # premultiplied resize (no dark fringes from transparent pixels), then un-premultiply
    a = np.asarray(im).astype(np.float64)
    pm = a.copy(); pm[..., :3] *= a[..., 3:4] / 255.0
    chans = [Image.fromarray(pm[..., i].astype(np.float32), 'F').resize((w, h), Image.LANCZOS) for i in range(4)]
    r = np.stack([np.asarray(c) for c in chans], -1)
    r[..., 3] = np.clip(r[..., 3], 0, 255)
    al = np.maximum(r[..., 3:4], 1e-6)
    rgb = np.where(r[..., 3:4] > 0.5, r[..., :3] * 255.0 / al, 0)
    img = np.concatenate([np.clip(rgb, 0, 255), r[..., 3:4]], -1)
    data = encode(img, base)
    assert len(data) == nb, (len(data), nb)
    os.makedirs(outdir, exist_ok=True)
    name = os.path.basename(orig)
    with open(os.path.join(outdir, name), 'wb') as f:
        f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, fmt, nb, cs) + data)
    print('wrote', os.path.join(outdir, name), w, h, hex(fmt), nb)

if __name__ == '__main__': main()
