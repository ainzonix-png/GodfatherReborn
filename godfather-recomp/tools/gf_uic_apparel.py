"""v1111ZC: dump the apparel UIC tables (frontend.str RefPack chunk) -> name / price / partSlider / partIndex / sets.
usage: python gf_uic_apparel.py <chunk.bin>"""
import struct, sys, re
d = open(sys.argv[1], 'rb').read()
def i32(o): return struct.unpack('>i', d[o:o+4])[0]
for m in re.finditer(rb'x:\\assets\\shared\\ui\\menus\\apparel\\tables\\([a-z_]+)\.uic\x00', d):
    name = m.group(1).decode(); p = m.end()
    q = d.find(b'\xff\xff\xff\xff', p)
    if q < 0 or q - p > 64: continue
    rows, cols, nstr = i32(q - 12), i32(q - 8), i32(q - 4)
    cells = [i32(q + 4 * k) for k in range(cols + rows * cols)]
    o = q + 4 * len(cells); strs = []
    for _ in range(nstr):
        n = i32(o); strs.append(d[o+4:o+4+n].decode('latin-1')); o += 4 + n; o = (o + 3) & ~3
    val = lambda c: strs[-c - 1] if c < 0 else c
    hdr = [val(c) for c in cells[:cols]]
    print('== %s (%d rows)' % (name, rows))
    for r in range(rows):
        row = [val(c) for c in cells[cols + r * cols: cols + (r + 1) * cols]]
        print('  ' + ' | '.join('%s=%s' % (h, v) for h, v in zip(hdr, row)))
