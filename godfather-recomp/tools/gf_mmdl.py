"""v1057C: EA "MMdl" MetaModel scene blocks inside Godfather .str chunks (decoded 2026-11-04 from st_bowery/sammys_alley).
Block: "MMdl", u32 ver(0xA), u32 size, ..., header words at +0x2C.. hold table offsets (relative to the block):
  +0x40 = instance table (20 bytes each: typeHash, typeNameOff, nameOff, attrListOff, (nAttr<<16)|flags), count = +0x54.
  attr record (24 bytes): dataOff, attrNameOff, partNameOff, isRef, typeHash, size.
  XFORM attr data = 4x4 BE float row-major (translation in row 3). isRef attr data = {u32 instHash, u32 assetHash, u32 typeHash, 0}.
Assets are found by their hash: the asset's own header holds assetHash three times in a row (e.g. ~0xD4 bytes before its
Havok packfile)."""
import struct, re, numpy as np
def be(u, o): return struct.unpack_from('>I', u, o)[0]
def mmdls(u):
    for m in re.finditer(b'MMdl\x00\x00\x00\x0a', u):
        B = m.start()
        try: yield B, list(instances(u, B))
        except Exception: continue
def instances(u, B):
    size = be(u, B + 8); end = B + size
    s = lambda o: u[B + o:B + o + 64].split(b'\0')[0].decode('latin1')
    tab = be(u, B + 0x40); n = be(u, B + 0x54)
    if not (0 < tab < size and 0 < n < 5000): return
    for i in range(n):
        th, tn, nm, lo, cnt = struct.unpack_from('>5I', u, B + tab + 20 * i)
        na = cnt >> 16; attrs = []
        for j in range(na):
            ro = be(u, B + lo + 4 * j); d, a, pn, k, h, sz = struct.unpack_from('>6I', u, B + ro)
            an = s(a)
            if an == 'XFORM': attrs.append(('XFORM', np.frombuffer(u, '>f4', 16, B + d).reshape(4, 4).astype(np.float64)))
            elif k == 1 or an in ('CollisionModel', 'CMD_SetModel', 'fileName', 'Model'): attrs.append((an, be(u, B + d + 4)))   # BuildingCollisionPart refs have isRef 0
            else: attrs.append((an, None))
        yield s(nm), s(tn), attrs
def asset_offsets(u):
    """assetHash -> offset of its header (a hash repeated 3x)"""
    out = {}
    a = np.frombuffer(u[:len(u) // 4 * 4], '>u4')
    for off in (0, 1, 2, 3):
        a = np.frombuffer(u[off:off + (len(u) - off) // 4 * 4], '>u4')
        eq = (a[:-2] == a[1:-1]) & (a[1:-1] == a[2:]) & (a[:-2] > 0x00FFFFFF) & (a[:-2] != 0xFFFFFFFF) & (a[:-2] != 0x5F5F5F5F)
        for i in np.nonzero(eq)[0]:
            out.setdefault(int(a[i]), off + 4 * int(i))
    return out
