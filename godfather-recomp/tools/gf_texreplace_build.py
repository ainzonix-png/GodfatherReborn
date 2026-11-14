"""Build texreplace/*.tex (TEX_REPLACE, rsx_d3d12_backend.c: textures swapped in when the game uploads the originals)
from the game files plus the port's own art in texreplace_src/.

  weapon wheel   every <n>_weaponwheel texture of godfather_v4/godfather_global_global.str, re-coloured to the light
                 grey silhouette style (tools/gf_wheel_icons.py v1056W: grey = clamp(170 + 0.45 * luminance, 175, 255),
                 original alpha). Hand-made replacements from texreplace_src/: pistol 52, dynamite 10, fist 117,
                 slot 103 / selected slot 106, R marker 114, d-pad 115. The family crest (100) stays the game's.
  main-menu logo "4_frontend" of frontend/frontend.str (512x256 DXT3): the game's own "The Godfather" logo, brightened,
                 with its "THE DON'S EDITION" strip (rows 210..255) replaced by texreplace_src/logo_reborn_strip.png.

Each file is named <w>x<h>_f<fmt>_<key>.tex after the original texture (the key the backend computes at upload).
usage: python tools/gf_texreplace_build.py <game USRDIR> <out dir (texreplace)>"""
import os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
import gf_str, gf_txd, gf_tex
root = os.path.dirname(here); SRC = os.path.join(root, 'texreplace_src')

HAND = {52: 'wheel_pistol', 10: 'wheel_dynamite', 117: 'wheel_fist', 103: 'wheel_slot', 106: 'wheel_slot_sel',
        114: 'wheel_rmark', 115: 'wheel_dpad'}
SKIP = {100}
LOGO_STRIP_Y = 210; LOGO_GAIN = 232.0 / 218.0

def dxt3_textures(path):
    """(name, w, h, level-0 bytes) of every DXT3 texture in a .str file"""
    for o, u in gf_str.chunks(path):
        for m in gf_txd.PAT.finditer(u):
            s = m.end(); name = u[s + 8:s + 40].split(b'\0')[0].decode('latin1'); hd = u[s + 72:s + 92]
            w, h = struct.unpack_from('>HH', hd, 8)
            if not (4 <= w <= 4096 and 4 <= h <= 4096) or hd[2] not in (0x03, 0x83): continue
            data = u[s + 92:s + 92 + w * h]
            if len(data) == w * h: yield name, w, h, data

def write_orig(tmp, w, h, data):
    key = gf_txd.csum(data) ^ ((w * 2654435761) & 0xFFFFFFFF) ^ (h << 7) ^ 0x87
    p = os.path.join(tmp, f"{w}x{h}_f87_{key:08X}.tex")
    with open(p, 'wb') as f: f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, 0x87, len(data), gf_txd.csum(data)) + data)
    return p

def encode(png, orig, outdir):
    subprocess.run([sys.executable, os.path.join(here, 'gf_texenc.py'), png, orig, outdir], check=True, stdout=subprocess.DEVNULL)

def main(usr, outdir):
    os.makedirs(outdir, exist_ok=True); tmp = tempfile.mkdtemp(prefix='gf_texrepl_'); n = 0; seen = set()
    for name, w, h, data in dxt3_textures(os.path.join(usr, 'godfather_v4', 'godfather_global_global.str')):
        if not name.endswith('_weaponwheel'): continue
        idx = int(name.split('_')[0])
        if idx in SKIP or idx in seen: continue
        seen.add(idx); orig = write_orig(tmp, w, h, data)
        if idx in HAND:
            png = os.path.join(SRC, HAND[idx] + '.png')
            if not os.path.isfile(png): print(f"  {name}: {png} missing, keeping the game's icon"); continue
        else:
            img = gf_tex.decode(orig)[4]
            lum = img[..., :3].astype(float) @ [0.299, 0.587, 0.114]
            g = np.clip(170 + 0.45 * lum, 175, 255)
            png = os.path.join(tmp, f'wheel_{idx}.png')
            Image.fromarray(np.dstack([g, g, g, img[..., 3]]).astype(np.uint8), 'RGBA').save(png)
        encode(png, orig, outdir); n += 1
    print(f"{n} weapon wheel icon(s)")
    strip = os.path.join(SRC, 'logo_reborn_strip.png')
    logo = next(((w, h, d) for name, w, h, d in dxt3_textures(os.path.join(usr, 'frontend', 'frontend.str'))
                 if name == '4_frontend' and (w, h) == (512, 256)), None)
    if logo and os.path.isfile(strip):
        w, h, data = logo; orig = write_orig(tmp, w, h, data)
        img = gf_tex.decode(orig)[4].astype(np.float64)
        img[..., :3] = np.clip(img[..., :3] * LOGO_GAIN, 0, 255)
        s = np.asarray(Image.open(strip).convert('RGBA'), dtype=np.float64)
        if s.shape != (h - LOGO_STRIP_Y, w, 4): sys.exit(f"{strip}: expected {w}x{h - LOGO_STRIP_Y}")
        img[LOGO_STRIP_Y:] = s
        png = os.path.join(tmp, 'logo.png'); Image.fromarray(img.astype(np.uint8), 'RGBA').save(png)
        encode(png, orig, outdir); print("main-menu logo")
    else:
        print("main-menu logo: not replaced (texture or texreplace_src/logo_reborn_strip.png not found)")

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
