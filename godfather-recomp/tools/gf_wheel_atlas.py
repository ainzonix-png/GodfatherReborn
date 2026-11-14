"""v1057W: icon atlas + key table for the overlay weapon wheel (rsx_ui_overlay.c, "Noir Ring").
Reads every <n>_weaponwheel texture from godfather_v4/godfather_global_global.str, computes its TEX_REPLACE key
(the backend tags textures with it) and packs white silhouettes (original alpha) into a 1024x1024 atlas.
Two-variant sheets (normal on top, selected below: 22 28 40 52 64) use the top half.
Output assets/gf_wheel.bin:
  "GFWHEEL1", u32 n, then n x { u32 key, u16 idx, u8 kind, u8 pad, u16 w, u16 h, u16 ax, u16 ay, u16 aw, u16 ah, char name[24] }
  then 1024*1024 RGBA8.   kind: 0 weapon icon, 1 empty slot disc, 2 selected slot disc, 3 R marker, 4 d-pad marker, 5 crest
Names are guesses from the icons -- edit NAMES and rerun (no rebuild needed).
usage: python tools/gf_wheel_atlas.py [game USRDIR]"""
import sys, os, struct
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
import gf_str, gf_txd, gf_tex, numpy as np
root = os.path.dirname(here)
usr = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, '..', "Godfather, The - The Don's Edition (Europe)", 'PS3_GAME', 'USRDIR')
src = os.path.join(usr, 'godfather_v4', 'godfather_global_global.str')
NAMES = {4: 'Tommy Gun', 7: 'Revolver', 10: 'Dynamite', 13: 'Drum Shotgun', 16: 'Pump Shotgun', 19: 'Double-Barrel',
         22: 'Shotgun', 25: 'Tommy Gun', 28: 'Tommy Gun', 31: 'Tommy Gun', 34: 'Revolver', 37: 'Magnum',
         40: 'Revolver', 43: 'Pistol', 46: 'Pistol', 49: 'Pistol', 52: 'Pistol', 55: 'Revolver', 58: 'Revolver',
         61: 'Revolver', 64: 'Revolver', 67: 'Blade', 70: 'Shovel', 73: 'Crowbar', 76: 'Tire Iron', 79: 'Pool Cue',
         82: 'Club', 85: 'Nightstick', 88: 'Baseball Bat', 91: 'Garrote', 94: 'Dynamite', 97: 'Molotov', 117: 'Fists'}
KIND = {103: 1, 106: 2, 114: 3, 115: 4, 100: 5}
PAIRS = {22, 28, 40, 52, 64}
atlas = np.zeros((1024, 1024, 4), np.uint8); ents = []; x = y = rowh = 0; seen = set()
tmp = os.path.join(root, '_tmp'); os.makedirs(tmp, exist_ok=True)
for o, u in gf_str.chunks(src):
    for name, w, h, fmt, hd, data in gf_txd.textures(u):
        if not name.endswith('_weaponwheel'): continue
        idx = int(name.split('_')[0])
        if idx in seen: continue
        seen.add(idx)
        key = gf_txd.csum(data) ^ ((w * 2654435761) & 0xFFFFFFFF) ^ (h << 7) ^ fmt
        kind = KIND.get(idx, 0); ax = ay = aw = ah = 0
        if kind == 0:
            p = os.path.join(tmp, 'wheel_tmp.tex')
            with open(p, 'wb') as f: f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, fmt, len(data), gf_txd.csum(data)) + data)
            img = gf_tex.decode(p)[4]
            if idx in PAIRS: img = img[: img.shape[0] // 2]
            a = img[..., 3]; ys, xs = np.nonzero(a > 8)
            if len(xs):
                img = img[max(ys.min() - 1, 0): ys.max() + 2, max(xs.min() - 1, 0): xs.max() + 2]
            ih, iw = img.shape[:2]
            if x + iw + 2 > 1024: x = 0; y += rowh + 2; rowh = 0
            atlas[y:y + ih, x:x + iw, :3] = 255; atlas[y:y + ih, x:x + iw, 3] = img[..., 3]
            ax, ay, aw, ah = x, y, iw, ih; x += iw + 2; rowh = max(rowh, ih)
        nm = NAMES.get(idx, '').encode('latin1')[:23]
        ents.append(struct.pack('<IHBBHHHHHH24s', key, idx, kind, 0, w, h, ax, ay, aw, ah, nm))
        print(f"{idx:4d} kind={kind} {w}x{h}_f{fmt:02X}_{key:08X}  atlas=({ax},{ay},{aw},{ah}) {NAMES.get(idx, '')}")
out = os.path.join(root, 'assets', 'gf_wheel.bin')
with open(out, 'wb') as f:
    f.write(b'GFWHEEL1' + struct.pack('<I', len(ents)) + b''.join(ents) + atlas.tobytes())
print(len(ents), 'entries ->', out)
