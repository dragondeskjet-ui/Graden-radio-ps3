// Host-only software implementation of gpu.h, mirroring the RSX shaders, used to render screenshots on a PC.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gpu.h"

static int W, H; static u32 *frame;
struct TexS { u32 *px; int w, h; } TX[TEX_COUNT];
static u16 *earth; static int ew = 4096, eh = 2048;

u32 *gpu_host_frame() { return frame; }
bool gpu_init(int w, int h) { W = w; H = h; frame = (u32 *)malloc((size_t)w * h * 4); return true; }
bool gpu_load_earth(const char *path)
{
	FILE *f = fopen(path, "rb"); if (!f) return false;
	earth = (u16 *)malloc((size_t)ew * eh * 2);
	unsigned char *raw = (unsigned char *)earth; size_t n = fread(raw, 2, (size_t)ew * eh, f); fclose(f);
	for (size_t i = 0; i < n; i++) earth[i] = (u16)((raw[i * 2] << 8) | raw[i * 2 + 1]);
	return n == (size_t)ew * eh;
}
void gpu_upload(int t, const u32 *argb, int w, int h)
{
	if (!TX[t].px || TX[t].w != w || TX[t].h != h) { free(TX[t].px); TX[t].px = (u32 *)malloc((size_t)w * h * 4); TX[t].w = w; TX[t].h = h; }
	memcpy(TX[t].px, argb, (size_t)w * h * 4);
}
static inline void over(u32 &d, float r, float g, float b, float a)
{
	float dr = ((d >> 16) & 255) / 255.f, dg = ((d >> 8) & 255) / 255.f, db = (d & 255) / 255.f;
	dr = r + dr * (1 - a); dg = g + dg * (1 - a); db = b + db * (1 - a);
	int R = (int)(fminf(dr, 1.f) * 255), G = (int)(fminf(dg, 1.f) * 255), B = (int)(fminf(db, 1.f) * 255);
	d = 0xFF000000u | (R << 16) | (G << 8) | B;
}
static void sample(const TexS &t, float u, float v, bool lin, float *o)
{
	float x = u * t.w - 0.5f, y = v * t.h - 0.5f;
	if (!lin) { int ix = (int)(u * t.w), iy = (int)(v * t.h); if (ix < 0) ix = 0; if (iy < 0) iy = 0; if (ix >= t.w) ix = t.w - 1; if (iy >= t.h) iy = t.h - 1; u32 c = t.px[iy * t.w + ix]; o[0] = ((c >> 16) & 255) / 255.f; o[1] = ((c >> 8) & 255) / 255.f; o[2] = (c & 255) / 255.f; o[3] = (c >> 24) / 255.f; return; }
	int x0 = (int)floorf(x), y0 = (int)floorf(y); float fx = x - x0, fy = y - y0;
	float acc[4] = { 0, 0, 0, 0 };
	for (int k = 0; k < 4; k++) {
		int xx = x0 + (k & 1), yy = y0 + (k >> 1); if (xx < 0) xx = 0; if (yy < 0) yy = 0; if (xx >= t.w) xx = t.w - 1; if (yy >= t.h) yy = t.h - 1;
		float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy); u32 c = t.px[yy * t.w + xx];
		acc[0] += wgt * ((c >> 16) & 255); acc[1] += wgt * ((c >> 8) & 255); acc[2] += wgt * (c & 255); acc[3] += wgt * (c >> 24);
	}
	for (int k = 0; k < 4; k++) o[k] = acc[k] / 255.f;
}
static void quad(int tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, const float *col, float rot)
{
	const TexS &t = TX[tex]; if (!t.px) return;
	float cx = x + w / 2, cy = y + h / 2, cs = cosf(-rot), sn = sinf(-rot), ext = (fabsf(rot) > 0 ? 0.7072f * (w + h) : 0.5f * fmaxf(w, h)) + 1;
	int X0 = (int)(cx - ext), X1 = (int)(cx + ext), Y0 = (int)(cy - ext), Y1 = (int)(cy + ext);
	if (!rot) { X0 = (int)floorf(x); X1 = (int)ceilf(x + w); Y0 = (int)floorf(y); Y1 = (int)ceilf(y + h); }
	bool lin = tex == TEX_ATLAS;
	for (int py = Y0; py < Y1; py++) { if (py < 0 || py >= H) continue;
		for (int px = X0; px < X1; px++) { if (px < 0 || px >= W) continue;
			float dx = px + 0.5f - cx, dy = py + 0.5f - cy, lx = dx * cs - dy * sn, ly = dx * sn + dy * cs;
			float s = lx / w + 0.5f, tt = ly / h + 0.5f; if (s < 0 || s >= 1 || tt < 0 || tt >= 1) continue;
			float c[4]; sample(t, u0 + (u1 - u0) * s, v0 + (v1 - v0) * tt, lin, c);
			over(frame[py * W + px], c[0] * col[0], c[1] * col[1], c[2] * col[2], c[3] * col[3]);
		}
	}
}
static void sprites(const Sprite *s, int n) { for (int i = 0; i < n; i++) { float c[4] = { s[i].r, s[i].g, s[i].b, s[i].a }; quad(s[i].tex, s[i].x, s[i].y, s[i].w, s[i].h, s[i].u0, s[i].v0, s[i].u1, s[i].v1, c, s[i].rot); } }

void gpu_frame(const GlobeView &g, const Sprite *bg, int nbg, const Sprite *dots, int ndots, const Sprite *fg, int nfg)
{
	for (int i = 0; i < W * H; i++) frame[i] = 0xFF03060D;
	sprites(bg, nbg);
	const float d = 3.14159265f / 180.f;
	float sl = sinf(g.lon * d), cl = cosf(g.lon * d), sp = sinf(g.lat * d), cp = cosf(g.lat * d);
	float ex = -sl, ey = cl, nx = -sp * cl, ny = -sp * sl, nz = cp, vx = cp * cl, vy = cp * sl, vz = sp;
	int y0 = (int)(g.cy - g.R) - 1, y1 = (int)(g.cy + g.R) + 1, x0 = (int)(g.cx - g.R) - 1, x1 = (int)(g.cx + g.R) + 1;
	for (int py = y0; py <= y1; py++) { if (py < 0 || py >= H) continue;
		for (int px = x0; px <= x1; px++) { if (px < 0 || px >= W) continue;
			float sx = (px + 0.5f - g.cx) / g.R, sy = (g.cy - py - 0.5f) / g.R, r2 = sx * sx + sy * sy;
			if (r2 >= 1) continue;
			float dz = sqrtf(1 - r2);
			/* world point = sx*e + sy*n + dz*v */
			float wx = sx * ex + sy * nx + dz * vx, wy = sx * ey + sy * ny + dz * vy, wz = sy * nz + dz * vz;
			float lat = asinf(fmaxf(-1.f, fminf(1.f, wz))), lon = atan2f(wy, wx);
			float u = (lon / d + 180.f) / 360.f, v = (90.f - lat / d) / 180.f;
			float rr = 0.0f, gg = 0.03f, bb = 0.2f;
			if (earth) {
				float fx = u * ew - 0.5f, fy = v * eh - 0.5f; int ix = (int)floorf(fx), iy = (int)floorf(fy); float ax = fx - ix, ay = fy - iy;
				rr = gg = bb = 0;
				for (int k = 0; k < 4; k++) { int xx = ((ix + (k & 1)) % ew + ew) % ew, yy = iy + (k >> 1); if (yy < 0) yy = 0; if (yy >= eh) yy = eh - 1;
					u16 t = earth[yy * ew + xx]; float wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
					rr += wgt * ((t >> 11) << 3) / 255.f; gg += wgt * (((t >> 5) & 63) << 2) / 255.f; bb += wgt * ((t & 31) << 3) / 255.f; }
			}
			float L[3] = { -0.4776f, 0.5838f, 0.6567f }, k = 0.40f + 0.80f * fmaxf(sx * L[0] + sy * L[1] + dz * L[2], 0.f);
			float rim = 1 - dz; rim = rim * rim * rim * 0.6f;
			rr = rr * k * (1 - rim) + 0.353f * rim; gg = gg * k * (1 - rim) + 0.659f * rim; bb = bb * k * (1 - rim) + 1.f * rim;
			frame[py * W + px] = 0xFF000000u | ((int)(fminf(rr, 1) * 255) << 16) | ((int)(fminf(gg, 1) * 255) << 8) | (int)(fminf(bb, 1) * 255);
		}
	}
	sprites(dots, ndots);
	sprites(fg, nfg);
}
