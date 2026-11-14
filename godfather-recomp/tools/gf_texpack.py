"""v1114T upscaled texture packs for the F10 "Texture upscaling" option (rsx_d3d12_backend.c texpack_*).

usage:
  python gf_texpack.py extract <USRDIR> <work>      every colour DXT texture of the disc -> <work>/src/<KEY>.png (RGB, wrap-padded)
                                                    + <work>/alpha/<KEY>.png + <work>/manifest.json
  python gf_texpack.py build <work> <up_dir> <2|4> <out.bin>
                                                    <up_dir>/<KEY>.png = the 4x AI upscale of src/<KEY>.png (Real-ESRGAN x4plus)
                                                    -> GFTPK1 pack (BC1/BC2/BC3 like the original, full mip chain)
  python gf_texpack.py fake-up <work> <up_dir>      Lanczos 4x stand-in for Real-ESRGAN (pipeline test)
  python gf_texpack.py verify <pack.bin> <work>     decode the pack back, compare with the source (PSNR per texture)

Keys: the TEX_REPLACE key (tools/gf_txd.py csum of level 0 ^ w*2654435761 ^ h<<7 ^ base fmt 0x86/0x87/0x88), i.e. the bytes the
game uploads. Pack: "GFTPK1\\0\\0", u32 version 1, u32 scale, u32 count, u32 0, then count x 32-byte entries sorted by key
{u32 key, u16 w0, h0, u16 W, H, u8 fmt, u8 nlev, u16 flags, u64 offset, u32 size, u32 0}, then the data: per level the BC blocks
row-major (ceil(w/4) x ceil(h/4)). Skipped on purpose: normal / spec maps, colour LUTs, shadows, fonts, HUD / UI, < 16 px."""
import zlib, sys, os, re, json, struct
import numpy as np
from PIL import Image

PAD = 8   # wrap padding around every source image (tiling textures get no seam from the upscaler)

# ------------------------------------------------------------------ .str / RefPack / TXD
def refpack(d, o=0, limit=None):
    fl = d[o]
    if d[o + 1] != 0xFB: raise ValueError('not refpack')
    if fl & 0x80: n = 4; size = int.from_bytes(d[o+2:o+6], 'big')
    else: n = 3; size = int.from_bytes(d[o+2:o+5], 'big')
    if limit: size = min(size, limit)
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
        if off >= ln: out += out[s:s+ln]
        else:
            for i in range(ln): out.append(out[s + i])
    return bytes(out)

def txd_chunks(path):
    d = open(path, 'rb').read(); i = 0
    while True:
        i = d.find(b'\x10\xfb', i)
        if i < 0: break
        try:
            head = refpack(d, i, limit=96)
            if b'rwID_TEXDICTIONARY' in head:
                yield refpack(d, i)
        except Exception:
            pass
        i += 2

def csum(b):
    n = len(b); step = n // 1024 if n > 4096 else 4; h = 2166136261; i = 0
    while i + 3 < n:
        h ^= struct.unpack_from('<I', b, i)[0]; h = (h * 16777619) & 0xFFFFFFFF; i += step
    return h

PAT = re.compile(rb'\x15\x00\x00\x00(....)\x2d\x00\x02\x1c\x01\x00\x00\x00(....)\x2d\x00\x02\x1c', re.S)
def textures(u):
    """(name, w, h, fmt 0x86/0x87/0x88, level-0 bytes) of every DXT texture native in a decompressed TXD chunk"""
    for m in PAT.finditer(u):
        s = m.end()
        name = u[s+8:s+40].split(b'\0')[0].decode('latin1')
        hd = u[s+72:s+92]
        w, h = struct.unpack_from('>HH', hd, 8); dsz = struct.unpack_from('>I', hd, 16)[0]
        if not (4 <= w <= 4096 and 4 <= h <= 4096) or (w & 3) or (h & 3): continue
        l0 = w * h; t = hd[2]
        if t in (0x82, 0xA2, 0x86, 0xA6): fmt = 0x86; nb = l0 // 2
        elif t in (0x83, 0xA3):                       # DXT3, or DXT1 with punch-through alpha (gf_txd.py misread those)
            if dsz >= l0: fmt = 0x87; nb = l0
            else: fmt = 0x86; nb = l0 // 2
        elif t in (0x88, 0xA8): fmt = 0x88; nb = l0
        else: continue                                # uncompressed / palettised: not packed
        data = u[s+92:s+92+nb]
        if len(data) < nb: continue
        yield name, w, h, fmt, data

def tex_key(data, w, h, fmt):
    return csum(data) ^ ((w * 2654435761) & 0xFFFFFFFF) ^ (h << 7) ^ fmt

SKIP = re.compile(r'(_n$|_n_|normal|_nrm|bump|_spec|_s$|spec$|lutcolor|^lut|shadow|shine|font|gothic|hud|weaponwheel|ppip|'
                  r'_glyph|button|icon|texture_not_found|respect_|_mask$|^mask|_lightmap|^lm_|^fx_|flare)', re.I)

# ------------------------------------------------------------------ DXT decode (numpy)
def _c565(c):
    c = c.astype(np.int32)
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)

def _color_blocks(cb, force4):
    c0 = cb[:, 0].astype(np.uint16) | (cb[:, 1].astype(np.uint16) << 8)
    c1 = cb[:, 2].astype(np.uint16) | (cb[:, 3].astype(np.uint16) << 8)
    bits = cb[:, 4].astype(np.uint32) | (cb[:, 5].astype(np.uint32) << 8) | (cb[:, 6].astype(np.uint32) << 16) | (cb[:, 7].astype(np.uint32) << 24)
    a = _c565(c0); b = _c565(c1)
    four = (c0 > c1) | force4
    p2 = np.where(four[:, None], (2 * a + b) // 3, (a + b) // 2)
    p3 = np.where(four[:, None], (a + 2 * b) // 3, 0)
    pal = np.stack([a, b, p2, p3], 1)                                  # N,4,3
    idx = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))[None]) & 3   # N,16
    rgb = np.take_along_axis(pal, idx[..., None].astype(np.int64), 1)  # N,16,3
    alpha = np.where((~four)[:, None] & (idx == 3), 0, 255)
    return rgb, alpha

def dxt_decode(data, w, h, fmt):
    bw, bh = w // 4, h // 4; n = bw * bh
    if fmt == 0x86:
        blk = np.frombuffer(data, np.uint8)[:n * 8].reshape(n, 8)
        rgb, a = _color_blocks(blk, np.zeros(n, bool))
    else:
        blk = np.frombuffer(data, np.uint8)[:n * 16].reshape(n, 16)
        rgb, _ = _color_blocks(blk[:, 8:], np.ones(n, bool))
        if fmt == 0x87:
            ab = blk[:, :8]
            a = np.stack([(ab[:, i // 2] >> (4 * (i & 1))) & 15 for i in range(16)], 1).astype(np.int32) * 17
        else:
            a0 = blk[:, 0].astype(np.int32); a1 = blk[:, 1].astype(np.int32)
            bits = np.zeros(n, np.uint64)
            for i in range(6): bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
            idx = ((bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))[None]) & np.uint64(7)).astype(np.int64)
            i8 = np.arange(8)
            p8 = np.where(i8[None] == 0, a0[:, None], np.where(i8[None] == 1, a1[:, None], ((8 - i8) * a0[:, None] + (i8 - 1) * a1[:, None]) // 7))
            p6 = np.where(i8[None] == 0, a0[:, None], np.where(i8[None] == 1, a1[:, None],
                 np.where(i8[None] == 6, 0, np.where(i8[None] == 7, 255, ((6 - i8) * a0[:, None] + (i8 - 1) * a1[:, None]) // 5))))
            pal = np.where((a0 > a1)[:, None], p8, p6)
            a = np.take_along_axis(pal, idx, 1)
    img = np.concatenate([rgb, a[..., None]], -1).reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4)
    return np.clip(img, 0, 255).astype(np.uint8)

# ------------------------------------------------------------------ BC encode (numpy)
def _to_blocks(img):
    h, w = img.shape[:2]; H = (h + 3) & ~3; W = (w + 3) & ~3
    if H != h or W != w: img = np.pad(img, ((0, H - h), (0, W - w), (0, 0)), mode='edge')
    return img.reshape(H // 4, 4, W // 4, 4, img.shape[2]).transpose(0, 2, 1, 3, 4).reshape(-1, 16, img.shape[2])

def _q565(c):   # float rgb 0..255 -> 565 int
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return ((c[..., 0] * 31 + 127) // 255 << 11) | ((c[..., 1] * 63 + 127) // 255 << 5) | ((c[..., 2] * 31 + 127) // 255)

def _bc_color(px, transparent=None, force4=False):
    """px N,16,3 float -> N,8 uint8 BC1 colour blocks; transparent N,16 bool -> 3-colour mode with index 3 = clear"""
    N = px.shape[0]
    if transparent is None: transparent = np.zeros((N, 16), bool)
    has_t = transparent.any(1) & (not force4)
    w = (~transparent).astype(np.float64)[..., None]; cnt = np.maximum(w.sum(1), 1.0)
    mean = (px * w).sum(1) / cnt
    d = (px - mean[:, None]) * w
    cov = np.einsum('nki,nkj->nij', d, d)
    v = np.ones((N, 3)) / np.sqrt(3)
    for _ in range(8):
        v = np.einsum('nij,nj->ni', cov, v); v /= np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-9)
    t = np.einsum('nki,ni->nk', px - mean[:, None], v)
    tmin = np.where(transparent, np.inf, t).min(1); tmax = np.where(transparent, -np.inf, t).max(1)
    tmin = np.where(np.isfinite(tmin), tmin, 0); tmax = np.where(np.isfinite(tmax), tmax, 0)
    e0 = mean + v * tmax[:, None]; e1 = mean + v * tmin[:, None]
    for it in range(2):
        c0 = _q565(e0); c1 = _q565(e1)
        swap4 = (~has_t) & (c0 < c1); swap3 = has_t & (c0 > c1)
        sw = swap4 | swap3
        c0, c1 = np.where(sw, c1, c0), np.where(sw, c0, c1)
        a = _c565(c0).astype(np.float64); b = _c565(c1).astype(np.float64)
        four = ~has_t
        pal = np.stack([a, b, np.where(four[:, None], (2 * a + b) / 3, (a + b) / 2), np.where(four[:, None], (a + 2 * b) / 3, 1e9)], 1)
        dist = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)       # N,16,4
        idx = dist.argmin(-1)
        idx = np.where(transparent & has_t[:, None], 3, idx)
        eq = c0 == c1
        idx = np.where((eq & four)[:, None], 0, idx)
        if it == 0:   # least-squares endpoint refinement (4-colour blocks)
            wts = np.array([[1, 0], [0, 1], [2 / 3, 1 / 3], [1 / 3, 2 / 3]])[np.minimum(idx, 3)]
            if True:
                al = wts[..., 0]; be = wts[..., 1]; m = (~transparent).astype(np.float64)
                A = np.stack([np.stack([(al*al*m).sum(1), (al*be*m).sum(1)], -1), np.stack([(al*be*m).sum(1), (be*be*m).sum(1)], -1)], 1)
                B0 = (al[..., None] * px * m[..., None]).sum(1); B1 = (be[..., None] * px * m[..., None]).sum(1)
                det = A[:, 0, 0] * A[:, 1, 1] - A[:, 0, 1] ** 2
                ok = (np.abs(det) > 1e-6) & four
                inv = 1.0 / np.where(ok, det, 1.0)
                ne0 = (A[:, 1, 1, None] * B0 - A[:, 0, 1, None] * B1) * inv[:, None]
                ne1 = (A[:, 0, 0, None] * B1 - A[:, 0, 1, None] * B0) * inv[:, None]
                # keep the refined pair only where it is not worse
                e0 = np.where(ok[:, None], np.where(sw[:, None], ne1, ne0), e0)
                e1 = np.where(ok[:, None], np.where(sw[:, None], ne0, ne1), e1)
    bits = (idx.astype(np.uint32) << (2 * np.arange(16, dtype=np.uint32))[None]).sum(1).astype(np.uint32)
    out = np.zeros((N, 8), np.uint8)
    out[:, 0] = c0 & 255; out[:, 1] = c0 >> 8; out[:, 2] = c1 & 255; out[:, 3] = c1 >> 8
    for i in range(4): out[:, 4 + i] = (bits >> (8 * i)) & 255
    return out

CHUNK = 4096   # blocks per encoder pass: bounds the temporaries (~25 MB) whatever the texture size
def bc_encode(img, fmt):
    """img H,W,4 uint8 -> bytes (BC1 for 0x86, BC2 for 0x87, BC3 for 0x88), CHUNK blocks at a time"""
    blk = _to_blocks(img)
    return b''.join(_bc_encode_blocks(blk[i:i + CHUNK].astype(np.float64), fmt) for i in range(0, len(blk), CHUNK))

def _bc_encode_blocks(blk, fmt):
    px = blk[..., :3]; al = blk[..., 3]
    if fmt == 0x86:
        tr = al < 128
        return _bc_color(px, tr).tobytes()
    col = _bc_color(px, None, force4=True)
    N = blk.shape[0]
    if fmt == 0x87:
        a4 = np.clip(np.rint(al / 17.0), 0, 15).astype(np.uint8)
        ab = (a4[:, 0::2] | (a4[:, 1::2] << 4)).astype(np.uint8)
        return np.concatenate([ab, col], 1).tobytes()
    a0 = al.max(1); a1 = al.min(1)
    i8 = np.arange(8)
    pal = np.where(i8[None] == 0, a0[:, None], np.where(i8[None] == 1, a1[:, None], ((8 - i8) * a0[:, None] + (i8 - 1) * a1[:, None]) / 7))
    idx = np.abs(al[:, :, None] - pal[:, None, :]).argmin(-1)
    same = a0 == a1; idx = np.where(same[:, None], 0, idx)
    bits = np.zeros(N, np.uint64)
    for i in range(16): bits |= idx[:, i].astype(np.uint64) << np.uint64(3 * i)
    ab = np.zeros((N, 8), np.uint8); ab[:, 0] = np.rint(a0).astype(np.uint8); ab[:, 1] = np.rint(a1).astype(np.uint8)
    for i in range(6): ab[:, 2 + i] = ((bits >> np.uint64(8 * i)) & np.uint64(255)).astype(np.uint8)
    return np.concatenate([ab, col], 1).tobytes()

# ------------------------------------------------------------------ commands
def cmd_extract(usrdir, work):
    os.makedirs(os.path.join(work, 'src'), exist_ok=True); os.makedirs(os.path.join(work, 'alpha'), exist_ok=True)
    man = {}; skipped = {}; nfiles = 0
    for root, dirs, files in os.walk(usrdir):
        if 'audiostreams' in root or os.sep + 'dlg' in root or '/dlg' in root: continue
        for fn in sorted(files):
            if not fn.lower().endswith('.str'): continue
            p = os.path.join(root, fn); nfiles += 1
            for u in txd_chunks(p):
                for name, w, h, fmt, data in textures(u):
                    key = tex_key(data, w, h, fmt); ks = '%08X' % key
                    if ks in man or ks in skipped: continue
                    if w < 16 or h < 16 or SKIP.search(name): skipped[ks] = name; continue
                    img = dxt_decode(data, w, h, fmt)
                    rgb = np.pad(img[..., :3], ((PAD, PAD), (PAD, PAD), (0, 0)), mode='wrap')
                    Image.fromarray(rgb).save(os.path.join(work, 'src', ks + '.png'))
                    has_a = bool((img[..., 3] < 255).any())
                    if has_a: Image.fromarray(img[..., 3]).save(os.path.join(work, 'alpha', ks + '.png'))
                    man[ks] = {'name': name, 'w': w, 'h': h, 'fmt': fmt, 'alpha': has_a, 'file': os.path.relpath(p, usrdir),
                               'crc': '%08X' % zlib.crc32(data)}   # v1115C: full-data check (keys collide)
            if nfiles % 50 == 0: print('[texpack] %d files, %d textures, %d skipped' % (nfiles, len(man), len(skipped)), flush=True)
    json.dump({'pad': PAD, 'textures': man, 'skipped': skipped}, open(os.path.join(work, 'manifest.json'), 'w'), indent=0)
    print('[texpack] extract done: %d .str files, %d textures to upscale, %d skipped (maps / UI / tiny)' % (nfiles, len(man), len(skipped)))

def cmd_fake_up(work, up):
    os.makedirs(up, exist_ok=True)
    for fn in os.listdir(os.path.join(work, 'src')):
        im = Image.open(os.path.join(work, 'src', fn)); im.resize((im.width * 4, im.height * 4), Image.LANCZOS).save(os.path.join(up, fn))

def mip_chain(img):
    lv = [img]; im = Image.fromarray(img)
    while im.width > 1 or im.height > 1:
        im = im.resize((max(1, im.width // 2), max(1, im.height // 2)), Image.BOX); lv.append(np.asarray(im))
    return lv

MAXDIM = 2048   # no upscaled texture beyond 2048 px a side (a 1024 px texture gets 2x in the 4x pack)
def _build_one(args):
    work, up, pad, ks, e, scale = args
    w, h, fmt = e['w'], e['h'], e['fmt']
    p = os.path.join(up, ks + '.png')
    if not os.path.exists(p): return ks, None
    s = scale
    while s > 1 and max(w, h) * s > MAXDIM: s //= 2
    if s < 2: return ks, None
    try:
        big = np.asarray(Image.open(p).convert('RGB'))
    except Exception:
        return ks, None
    s4 = big.shape[0] // (h + 2 * pad)
    if s4 < 2 or big.shape[1] != (w + 2 * pad) * s4: return ks, None
    rgb = big[pad * s4: pad * s4 + h * s4, pad * s4: pad * s4 + w * s4]
    W, H = w * s, h * s
    if s4 != s: rgb = np.asarray(Image.fromarray(rgb).resize((W, H), Image.LANCZOS))
    if e['alpha']: a = np.asarray(Image.open(os.path.join(work, 'alpha', ks + '.png')).resize((W, H), Image.LANCZOS))
    else: a = np.full((H, W), 255, np.uint8)
    lv = mip_chain(np.concatenate([rgb, a[..., None]], -1))
    return ks, (w, h, W, H, fmt, len(lv), b''.join(bc_encode(l, fmt) for l in lv), int(e.get('crc', '0'), 16), 'crc' in e)

def cmd_build(work, up, scale, out, jobs=0):
    import multiprocessing as mp
    M = json.load(open(os.path.join(work, 'manifest.json'))); pad = M['pad']; T = M['textures']
    keys = sorted(T, key=lambda k: int(k, 16))
    tmp = out + '.data.tmp'; ents = []; off = 0; missing = 0
    jobs = jobs or int(os.environ.get('TEXPACK_JOBS', '0')) or max(1, min(4, (os.cpu_count() or 2) - 1))   # memory: a few hundred MB per worker
    args = [(work, up, pad, ks, T[ks], scale) for ks in keys]
    with open(tmp, 'wb') as fd, mp.Pool(jobs) as pool:
        for n, (ks, r) in enumerate(pool.imap(_build_one, args, chunksize=4)):
            if r is None: missing += 1; continue
            w, h, W, H, fmt, nl, data, crc, hc = r
            ents.append((int(ks, 16), w, h, W, H, fmt, nl, off, len(data), crc, hc)); fd.write(data); off += len(data)
            if n % 250 == 0: print('[texpack] %dx: %d / %d textures, %.0f MB' % (scale, n, len(keys), off / 1e6), flush=True)
    hdr = b'GFTPK1\0\0' + struct.pack('<IIII', 1, scale, len(ents), 0)
    base = len(hdr) + 32 * len(ents)
    with open(out + '.tmp', 'wb') as f:
        f.write(hdr)
        for k, w, h, W, H, fmt, nl, o, sz, crc, hc in ents: f.write(struct.pack('<IHHHHBBHQII', k, w, h, W, H, fmt, nl, 1 if hc else 0, base + o, sz, crc))
        with open(tmp, 'rb') as fd:
            while True:
                b = fd.read(1 << 24)
                if not b: break
                f.write(b)
    os.remove(tmp)
    if os.path.exists(out): os.remove(out)
    os.rename(out + '.tmp', out)
    print('[texpack] %dx pack: %d textures, %.0f MB -> %s (%d skipped: no upscale / too big)' % (scale, len(ents), (base + off) / 1e6, out, missing))

def cmd_verify(pack, work):
    d = open(pack, 'rb').read(); assert d[:6] == b'GFTPK1'
    ver, scale, cnt, _ = struct.unpack_from('<IIII', d, 8)
    M = json.load(open(os.path.join(work, 'manifest.json'))); pad = M['pad']
    ps = []
    for i in range(cnt):
        k, w0, h0, W, H, fm, nl, fl, o, sz, z = struct.unpack_from('<IHHHHBBHQII', d, 24 + 32 * i)
        img = dxt_decode(d[o:o + ((W + 3) // 4) * ((H + 3) // 4) * (8 if fm == 0x86 else 16)], W, H, fm)
        src = np.asarray(Image.open(os.path.join(work, 'src', '%08X.png' % k)))[pad:-pad, pad:-pad]
        small = np.asarray(Image.fromarray(img[..., :3]).resize((w0, h0), Image.BOX)).astype(float)
        mse = ((small - src.astype(float)) ** 2).mean(); ps.append(10 * np.log10(255 ** 2 / max(mse, 1e-3)))
    ps = np.array(ps); print('[texpack] verify %s: %d textures, scale %d, PSNR vs source (downsampled) median %.1f dB, min %.1f' % (pack, cnt, scale, np.median(ps), ps.min()))

if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'extract': cmd_extract(sys.argv[2], sys.argv[3])
    elif c == 'fake-up': cmd_fake_up(sys.argv[2], sys.argv[3])
    elif c == 'build': cmd_build(sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5], int(sys.argv[6]) if len(sys.argv) > 6 else 0)
    elif c == 'verify': cmd_verify(sys.argv[2], sys.argv[3])
    else: print(__doc__)
