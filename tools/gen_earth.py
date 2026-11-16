"""Builds release/USRDIR/earth.565: 4096x2048 equirectangular Earth texture, RGB565 big-endian.
Source: NASA Blue Marble Next Generation (public domain, NASA Earth Observatory), as bundled in basemap-data."""
import os, numpy as np
from PIL import Image, ImageEnhance
Image.MAX_IMAGE_PIXELS = None
import mpl_toolkits
src = None
for p in mpl_toolkits.__path__:
    c = os.path.join(p, "basemap_data", "bmng.jpg")
    if os.path.exists(c): src = c
assert src, "pip install basemap-data"
W, H = 4096, 2048
im = Image.open(src).convert("RGB").resize((W, H), Image.LANCZOS)
im = ImageEnhance.Color(im).enhance(1.12)
im = ImageEnhance.Contrast(im).enhance(1.06)
a = np.asarray(im).astype(np.uint16)
px = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
os.makedirs("release/USRDIR", exist_ok=True)
px.astype(">u2").tofile("release/USRDIR/earth.565")
print("ok", W, H)
