"""gf_uifont_text.py -- v1045E: rebuild ONLY the text glyphs of assets/ui_font.bin with another TTF/OTF (icons are
copied from the existing file). Godfather's own menus use Trade Gothic Bold Condensed; Nimbus Sans Narrow Bold is the
closest free face. Usage: python3 gf_uifont_text.py <in ui_font.bin> <out ui_font.bin> [font file]"""
import struct, sys
from PIL import Image, ImageDraw, ImageFont
src, dst = sys.argv[1], sys.argv[2]
FONT = sys.argv[3] if len(sys.argv) > 3 else '/usr/share/fonts/opentype/urw-base35/NimbusSansNarrow-Bold.otf'
d = open(src, 'rb').read(); assert d[:8] == b'IFUIFNT1'
AW, AH, nt, TLINE, ni, ILINE, wx, wy = struct.unpack('<8I', d[8:40])
recs = [struct.unpack('<I8h', d[40 + 20 * i:60 + 20 * i]) for i in range(nt + ni)]
old = Image.frombytes('RGBA', (AW, AH), d[40 + 20 * (nt + ni):40 + 20 * (nt + ni) + AW * AH * 4])
atlas = Image.new('RGBA', (AW, AH), (0, 0, 0, 0))
x, y, rowh = 4, 4, 0
def place(img):
    global x, y, rowh
    w, h = img.size
    if x + w + 2 > AW: x = 4; y += rowh + 2; rowh = 0
    atlas.paste(img, (x, y)); px, py = x, y
    x += w + 2; rowh = max(rowh, h); return px, py
white = Image.new('RGBA', (4, 4), (255, 255, 255, 255)); nwx, nwy = place(white); nwx += 1; nwy += 1
f = ImageFont.truetype(FONT, 60)
asc, desc = f.getmetrics()
base = int(TLINE * 0.80)
text = []
for r in recs[:nt]:
    c = r[0]; ch = chr(c)
    adv = f.getlength(ch); bb = f.getbbox(ch)
    if bb[2] <= bb[0] or bb[3] <= bb[1]: text.append((c, 0, 0, 0, 0, 0, 0, int(round(adv)))); continue
    w, h = bb[2] - bb[0] + 2, bb[3] - bb[1] + 2
    im = Image.new('RGBA', (w, h), (255, 255, 255, 0))
    m = Image.new('L', (w, h), 0); ImageDraw.Draw(m).text((1 - bb[0], 1 - bb[1]), ch, font=f, fill=255)
    im.putalpha(m); px, py = place(im)
    text.append((c, px, py, w, h, bb[0] - 1, (base - asc) + bb[1] - 1, int(round(adv))))
icons = []
for r in recs[nt:]:
    c, ox, oy, w, h, xo, yo, adv, _ = r
    px, py = place(old.crop((ox, oy, ox + w, oy + h)))
    icons.append((c, px, py, w, h, xo, yo, adv))
with open(dst, 'wb') as o:
    o.write(b'IFUIFNT1'); o.write(struct.pack('<8I', AW, AH, len(text), TLINE, len(icons), ILINE, nwx, nwy))
    for r in text + icons: o.write(struct.pack('<I8h', *r, 0))
    o.write(atlas.tobytes())
print('font', FONT, 'glyphs', len(text), 'icons', len(icons), 'atlas rows to y', y + rowh)
