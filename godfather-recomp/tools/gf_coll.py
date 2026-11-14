"""v1057C: world collision from the level files. Reads Havok 4.1 packfiles (EA "Neo" CollisionModel objects:
EAStorageMeshShape / hkSimpleMeshShape inside hkMoppBvTreeShape) out of .str RefPack chunks and keeps the meshes that
are already in world space (street / terrain / zone models). Props and building assets in local space need their
instance placements (not decoded yet) and are only counted.
usage: python tools/gf_coll.py out.npz file.str [file.str ...]"""
import sys, os, struct, re, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import gf_str
MAG = b'\x57\xe0\xe0\x57\x10\xc0\xc0\x10'
def be(b, o): return struct.unpack_from('>I', b, o)[0]
def packfile(u, base):
    nsec = be(u, base + 0x14); secs = []; o = base + 0x40
    for s in range(nsec):
        tag = u[o:o + 19].split(b'\0')[0]; v = struct.unpack_from('>7I', u, o + 20); secs.append((tag, base + v[0]) + v[1:]); o += 48
    cn = next(s for s in secs if s[0] == b'__classnames__'); D = next(s for s in secs if s[0] == b'__data__')
    a = D[1]; data = u[a:a + D[2]]; loc = {}
    for p in range(a + D[2], a + D[3] - 7, 8):
        s_, d_ = struct.unpack_from('>2I', u, p)
        if s_ != 0xFFFFFFFF: loc[s_] = d_
    objs = []
    for p in range(a + D[4], a + D[5] - 11, 12):
        s_, sec, co = struct.unpack_from('>3I', u, p)
        if s_ != 0xFFFFFFFF: objs.append((s_, u[cn[1] + co:cn[1] + co + 64].split(b'\0')[0].decode('latin1')))
    return data, loc, sorted(objs)
def meshes(path):
    for o, u in gf_str.chunks(path):
        for m in re.finditer(re.escape(MAG), u):
            try: data, loc, objs = packfile(u, m.start())
            except Exception: continue
            names = re.findall(rb'Neo\|[\x20-\x7e]{6,}', data); name = names[0].decode() if names else '?'
            for off, cls in objs:
                if cls not in ('EAStorageMeshShape', 'hkSimpleMeshShape'): continue
                fx = sorted(k for k in loc if off <= k < off + 0x40)
                if len(fx) < 2: continue
                vp, nv = loc[fx[0]], be(data, fx[0] + 4); tp, nt = loc[fx[1]], be(data, fx[1] + 4)
                if not (0 < nv < 200000 and 0 < nt < 400000) or vp + nv * 16 > len(data) or tp + nt * 12 > len(data): continue
                V = np.frombuffer(data, '>f4', nv * 4, vp).reshape(nv, 4)[:, :3].astype(np.float32)
                T = np.frombuffer(data, '>i4', nt * 3, tp).reshape(nt, 3).astype(np.int32)
                if T.min() < 0 or T.max() >= nv: continue
                yield name, cls, V, T
if __name__ == '__main__':
    out = sys.argv[1]; Vs = []; Ts = []; nv = 0; kept = skipped = 0; names = {}
    for f in sys.argv[2:]:
        for name, cls, V, T in meshes(f):
            ext = V.max(0) - V.min(0); ctr = np.abs((V.max(0) + V.min(0)) / 2)
            world = ('|terrain|' in name) or ('|Zone' in name) or ('|st_' in name) or ctr.max() > 120.0
            if not world or 'Animated' in name: skipped += 1; k = (name.split('|') + ['?', '?'])[1]; names[k] = names.get(k, 0) + 1; continue
            Vs.append(V); Ts.append(T + nv); nv += len(V); kept += 1
    V = np.concatenate(Vs) if Vs else np.zeros((0, 3), np.float32); T = np.concatenate(Ts) if Ts else np.zeros((0, 3), np.int32)
    np.savez_compressed(out, V=V, T=T)
    print(f"kept {kept} world meshes ({len(V)} verts, {len(T)} tris), skipped {skipped} local-space models")
    print("local-space models by asset:", sorted(names.items(), key=lambda x: -x[1])[:25])
