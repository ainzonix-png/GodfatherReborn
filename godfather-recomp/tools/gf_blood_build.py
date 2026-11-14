"""Build assets/gf_blood.bin (blood decal atlas for rsx_ui_overlay.c gfd_*) from the game's own textures.
Source: godfather_v4/godfather_global_global.str (RefPack SToc -> RenderWare textures, tools/gf_txd.py): the alpha
shapes of bloodspray (top left), bloodsplats3 (top right) and bloodpool (bottom row, both cells) as a 2x2 atlas of
128x128 cells in one flat dark red each. Output: "GFBLOOD1" + 256*256 RGBA8.
usage: python tools/gf_blood_build.py <game USRDIR> <out.bin>"""
import os, struct, sys, tempfile
import numpy as np
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
import gf_str, gf_txd, gf_tex

# cell (col, row) -> texture name, flat colour, alpha gain
CELLS = [((0, 0), 'bloodspray', (37, 3, 2), 0.95), ((1, 0), 'bloodsplats3', (35, 3, 2), 0.90),
         ((0, 1), 'bloodpool', (29, 2, 2), 1.0), ((1, 1), 'bloodpool', (29, 2, 2), 1.0)]

def main(usr, out):
    src = os.path.join(usr, 'godfather_v4', 'godfather_global_global.str')
    want = {c[1] for c in CELLS}; alpha = {}
    for o, u in gf_str.chunks(src):
        for name, w, h, fmt, hd, data in gf_txd.textures(u):
            if name not in want or name in alpha: continue
            nb = w * h   # all three are DXT3 (16 bytes per 4x4 block); gf_txd's size guess is too large for two of them
            p = os.path.join(tempfile.gettempdir(), 'gf_blood_tmp.tex')
            with open(p, 'wb') as f: f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, 0x87, nb, 0) + data[:nb])
            a = gf_tex.decode(p)[4][..., 3]
            alpha[name] = np.asarray(Image.fromarray(a, 'L').resize((128, 128), Image.LANCZOS), dtype=np.float64)
        if len(alpha) == len(want): break
    missing = want - set(alpha)
    if missing: sys.exit(f"textures not found in {src}: {sorted(missing)}")
    atlas = np.zeros((256, 256, 4), np.uint8)
    for (cx, cy), name, rgb, gain in CELLS:
        cell = atlas[cy * 128:(cy + 1) * 128, cx * 128:(cx + 1) * 128]
        cell[..., :3] = rgb
        cell[..., 3] = np.clip(np.rint(alpha[name] * gain), 0, 255).astype(np.uint8)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'wb') as f: f.write(b'GFBLOOD1' + atlas.tobytes())
    print(f"blood atlas -> {out}")

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
