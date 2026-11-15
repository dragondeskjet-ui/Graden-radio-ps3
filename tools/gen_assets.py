"""Generates source/landmask.h (RLE land mask, from global-land-mask / GLOBE, public domain)
and source/fonts.h (antialiased DejaVu Sans glyphs, Latin-1)."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from global_land_mask import globe

# ---- land mask: 1440 x 720 (0.25 deg), rows from lat +90 down, cols from lon -180 ----
W, H = 1440, 720
lats = 90 - (np.arange(H) + 0.5) * 180 / H
lons = -180 + (np.arange(W) + 0.5) * 360 / W
lon_g, lat_g = np.meshgrid(lons, lats)
mask = globe.is_land(lat_g, lon_g)
runs, row_off = [], []
for y in range(H):
    row_off.append(len(runs))
    r = mask[y]; cur = False; n = 0
    for v in r:
        if v == cur: n += 1
        else:
            while n > 65535: runs += [65535, 0]; n -= 65535
            runs.append(n); cur = v; n = 1
    runs.append(n)
row_off.append(len(runs))
with open("source/landmask.h", "w") as f:
    f.write("// Land mask %dx%d, RLE runs alternating water/land starting with water.\n" % (W, H))
    f.write("// Source: global-land-mask (GLOBE dataset, NOAA, public domain).\n")
    f.write("#define LAND_W %d\n#define LAND_H %d\n" % (W, H))
    f.write("static const unsigned int land_row[%d] = {%s};\n" % (len(row_off), ",".join(map(str, row_off))))
    f.write("static const unsigned short land_runs[%d] = {" % len(runs))
    for i in range(0, len(runs), 40): f.write(",".join(map(str, runs[i:i+40])) + ",\n")
    f.write("};\n")
print("land runs", len(runs))

# ---- fonts ----
def gen(path, size, name, out):
    font = ImageFont.truetype(path, size)
    asc, desc = font.getmetrics()
    glyphs, data = [], []
    for c in range(32, 256):
        ch = chr(c)
        if 127 <= c < 160: ch = "?"
        bbox = font.getbbox(ch, anchor="ls")
        adv = int(round(font.getlength(ch)))
        x0, y0, x1, y1 = bbox
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        off = len(data)
        if w and h:
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            data += list(im.tobytes())
        glyphs.append((off, w, h, x0, y0, adv))
    out.write("static const unsigned char %s_px[%d] = {" % (name, max(1, len(data))))
    for i in range(0, len(data), 48): out.write(",".join(map(str, data[i:i+48])) + ",\n")
    out.write("};\nstatic const FontGlyph %s_g[224] = {\n" % name)
    for g in glyphs: out.write("{%d,%d,%d,%d,%d,%d},\n" % g)
    out.write("};\nstatic const FontDef %s = { %s_px, %s_g, %d, %d };\n" % (name, name, name, asc, asc + desc))
    return len(data)

R = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
with open("source/fonts.h", "w") as f:
    f.write("// Generated from DejaVu Sans (Bitstream Vera derived, free license). Latin-1 32..255, 8-bit alpha.\n")
    f.write("#ifndef FONTS_H\n#define FONTS_H\n")
    f.write("struct FontGlyph { unsigned int off; unsigned char w, h; signed char bx, by; unsigned char adv; };\n")
    f.write("struct FontDef { const unsigned char *px; const FontGlyph *g; int ascent; int line; };\n")
    tot = 0
    for size, path, nm in [(15, R, "f_s15"), (20, R, "f_r20"), (20, B, "f_b20"), (28, B, "f_b28"),
                           (22, R, "f_s22"), (30, R, "f_r30"), (30, B, "f_b30"), (42, B, "f_b42")]:
        tot += gen(path, size, nm, f)
    f.write("#endif\n")
print("font bytes", tot)
