"""v1056W: weapon-wheel icons for EVERY weapon, straight from the game files (no in-game dumping).
Reads godfather_v4/godfather_global_global.str (RefPack SToc -> RenderWare texture dictionaries), takes every
<n>_weaponwheel texture, re-colours it like the v1052W set (light grey silhouette, original alpha shape:
grey = clamp(170 + 0.45 * luminance, 175, 255)) and writes texreplace_src/wheel_<n>.png + texreplace/<key>.tex.
Hand-made art (pistol 52, dynamite 10, fist 117, slots 103/106, R 114, d-pad 115) is never overwritten.
usage: python tools/gf_wheel_icons.py [game USRDIR]"""
import sys, os, subprocess
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
import gf_str, gf_txd, gf_tex, numpy as np
from PIL import Image
root = os.path.dirname(here)
usr = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, '..', "Godfather, The - The Don's Edition (Europe)", 'PS3_GAME', 'USRDIR')
src = os.path.join(usr, 'godfather_v4', 'godfather_global_global.str')
KEEP = {10, 52, 100, 103, 106, 114, 115, 117}   # hand-made already / not a weapon (100 = family crest)
os.makedirs(os.path.join(root, 'wheel_all'), exist_ok=True); n = 0
for o, u in gf_str.chunks(src):
    for name, w, h, fmt, hd, data in gf_txd.textures(u):
        if not name.endswith('_weaponwheel'): continue
        idx = int(name.split('_')[0])
        key = gf_txd.csum(data) ^ ((w * 2654435761) & 0xFFFFFFFF) ^ (h << 7) ^ fmt
        fn = f"{w}x{h}_f{fmt:02X}_{key:08X}.tex"; orig = os.path.join(root, 'wheel_all', fn)
        with open(orig, 'wb') as f:
            import struct; f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, fmt, len(data), gf_txd.csum(data)) + data)
        if idx in KEEP: continue
        _, _, _, _, img = gf_tex.decode(orig)
        lum = img[..., :3].astype(float) @ [0.299, 0.587, 0.114]
        g = np.clip(170 + 0.45 * lum, 175, 255)
        out = np.dstack([g, g, g, img[..., 3]]).astype(np.uint8)
        png = os.path.join(root, 'texreplace_src', f'wheel_{idx}.png'); Image.fromarray(out, 'RGBA').save(png)
        subprocess.run([sys.executable, os.path.join(here, 'gf_texenc.py'), png, orig, os.path.join(root, 'texreplace')], check=True,
                       stdout=subprocess.DEVNULL)
        n += 1; print(f"{name:20s} {fn}")
print(n, "weapon icon(s) written to texreplace/")
