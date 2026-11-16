"""Builds release/USRDIR/earth.565: 1440x720 equirectangular Earth texture, RGB565 big-endian.
Colors: Natural Earth I (public domain, via cartopy) with GLOBE land mask (public domain) for crisp coasts,
graded toward a satellite look."""
import os, numpy as np, cartopy
from PIL import Image, ImageFilter
from global_land_mask import globe
W, H = 1440, 720
src = os.path.join(os.path.dirname(cartopy.__file__), "data/raster/natural_earth/50-natural-earth-1-downsampled.png")
ne = np.asarray(Image.open(src).convert("RGB").resize((W, H), Image.BICUBIC)).astype(np.float32) / 255
lats = 90 - (np.arange(H) + 0.5) * 180 / H
lons = -180 + (np.arange(W) + 0.5) * 360 / W
lg, la = np.meshgrid(lons, lats)
land = globe.is_land(la, lg).astype(np.float32)
land = np.asarray(Image.fromarray((land * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6))).astype(np.float32) / 255
# land: boost saturation, darken slightly, warm deserts; poles brighter (ice)
g = ne.mean(axis=2, keepdims=True)
landc = np.clip(g + (ne - g) * 1.45, 0, 1) * 0.82
# ice sheets (Greenland, Antarctica): keep bright white instead of darkening
icew = np.clip((g - 0.78) * 6, 0, 1)
landc = landc * (1 - icew) + np.clip(ne * 1.02, 0, 1) * icew
# ocean: deep navy with shelf tint from NE water brightness (shallower = lighter)
shelf = np.clip((ne[..., 2:3] - 0.55) * 2.2, 0, 1) * np.clip((72 - np.abs(la)) / 18, 0, 1)[..., None]
ocean = np.array([0.03, 0.09, 0.20]) * (1 - shelf) + np.array([0.07, 0.24, 0.38]) * shelf
img = landc * land[..., None] + ocean * (1 - land[..., None])
img = np.clip(img, 0, 1)
r = (img[..., 0] * 31 + 0.5).astype(np.uint16); gg = (img[..., 1] * 63 + 0.5).astype(np.uint16); b = (img[..., 2] * 31 + 0.5).astype(np.uint16)
px = (r << 11) | (gg << 5) | b
os.makedirs("release/USRDIR", exist_ok=True)
px.astype(">u2").tofile("release/USRDIR/earth.565")
Image.fromarray((img * 255).astype(np.uint8)).resize((720, 360)).save("/tmp/w/earth_preview.png")
print("ok", px.shape)
