#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "fonts.h"

static u32 *backbuf = NULL, *fb = NULL;
static int W = 0, H = 0;
const FontDef *F_SMALL, *F_BODY, *F_BOLD, *F_TITLE;

void gfx_init(u32 w, u32 h)
{
	W = w; H = h; backbuf = fb = (u32 *)malloc((size_t)w * h * 4);
	if (h >= 900) { F_SMALL = &f_s22; F_BODY = &f_r30; F_BOLD = &f_b30; F_TITLE = &f_b42; }
	else          { F_SMALL = &f_s15; F_BODY = &f_r20; F_BOLD = &f_b20; F_TITLE = &f_b28; }
}
u32 *gfx_buffer() { return backbuf; }
void gfx_set_target(u32 *b) { fb = b ? b : backbuf; }

/* All targets are premultiplied ARGB (0xAARRGGBB). Source colour is straight RGB + coverage a.
 * out = src*a + dst*(1-a), alpha channel included. Opaque framebuffers just keep A=255. */
static int CX0 = 0, CY0 = 0, CX1 = 1 << 30, CY1 = 1 << 30;
static inline u32 blend(u32 d, u32 s, int a)
{
	if (a >= 255) return 0xFF000000u | (s & 0xFFFFFF);
	if (a <= 0) return d;
	u32 ia = 255 - a;
	u32 rb = (((s & 0xFF00FF) * a + (d & 0xFF00FF) * ia) >> 8) & 0xFF00FF;
	u32 g  = (((s & 0x00FF00) * a + (d & 0x00FF00) * ia) >> 8) & 0x00FF00;
	u32 al = (u32)a + (((d >> 24) * ia) >> 8); if (al > 255) al = 255;
	return (al << 24) | rb | g;
}
static inline void px(int x, int y, u32 c, int a)
{
	if (x < CX0 || y < CY0 || x >= CX1 || y >= CY1) return;
	if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) { u32 *p = fb + y * W + x; *p = blend(*p, c, a); }
}
void gfx_clip(int x, int y, int w, int h) { CX0 = x; CY0 = y; CX1 = x + w; CY1 = y + h; }
void gfx_noclip() { CX0 = 0; CY0 = 0; CX1 = 1 << 30; CY1 = 1 << 30; }

void gfx_clear(u32 argb) { for (int i = 0; i < W * H; i++) fb[i] = argb; }

static bool clip(int &x, int &y, int &w, int &h)
{
	int x0 = CX0 > 0 ? CX0 : 0, y0 = CY0 > 0 ? CY0 : 0, x1 = CX1 < W ? CX1 : W, y1 = CY1 < H ? CY1 : H;
	if (x < x0) { w -= x0 - x; x = x0; } if (y < y0) { h -= y0 - y; y = y0; }
	if (x + w > x1) w = x1 - x; if (y + h > y1) h = y1 - y;
	return w > 0 && h > 0;
}
void gfx_rect(int x, int y, int w, int h, u32 rgb)
{
	if (!clip(x, y, w, h)) return;
	u32 c = 0xFF000000u | rgb;
	for (int j = 0; j < h; j++) { u32 *p = fb + (y + j) * W + x; for (int i = 0; i < w; i++) p[i] = c; }
}
void gfx_rect_a(int x, int y, int w, int h, u32 rgb, int a)
{
	if (a >= 255) { gfx_rect(x, y, w, h, rgb); return; }
	if (!clip(x, y, w, h)) return;
	for (int j = 0; j < h; j++) { u32 *p = fb + (y + j) * W + x; for (int i = 0; i < w; i++) p[i] = blend(p[i], rgb, a); }
}
static inline int cov(float d) { float c = 0.5f - d; return c <= 0 ? 0 : (c >= 1 ? 255 : (int)(c * 255)); }

void gfx_rrect(int x, int y, int w, int h, int r, u32 rgb, int a)
{
	if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
	if (r <= 0) { gfx_rect_a(x, y, w, h, rgb, a); return; }
	gfx_rect_a(x, y + r, w, h - 2 * r, rgb, a);
	for (int j = 0; j < r; j++) {
		float dy = r - j - 0.5f;
		for (int i = 0; i < w; i++) {
			int cx = i < r ? r : (i >= w - r ? w - r - 1 : -1);
			int ca = a;
			if (cx >= 0) { float dx = (i + 0.5f) - (cx + (i < r ? 0.f : 1.f)); float d = sqrtf(dx * dx + dy * dy) - r; ca = cov(d) * a / 255; }
			px(x + i, y + j, rgb, ca); px(x + i, y + h - 1 - j, rgb, ca);
		}
	}
}
/* rounded rect with vertical gradient (colour and alpha) */
void gfx_rrect_v(int x, int y, int w, int h, int r, u32 c0, int a0, u32 c1, int a1)
{
	if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
	for (int j = 0; j < h; j++) {
		float t = h > 1 ? (float)j / (h - 1) : 0.f;
		int ar = ((c0 >> 16) & 255) + (int)((((int)(c1 >> 16) & 255) - (int)((c0 >> 16) & 255)) * t);
		int ag = ((c0 >> 8) & 255) + (int)((((int)(c1 >> 8) & 255) - (int)((c0 >> 8) & 255)) * t);
		int ab = (c0 & 255) + (int)((((int)c1 & 255) - (int)(c0 & 255)) * t);
		u32 c = ((u32)ar << 16) | ((u32)ag << 8) | (u32)ab;
		int a = a0 + (int)((a1 - a0) * t);
		float dy = j < r ? r - j - 0.5f : (j >= h - r ? j - (h - r) + 0.5f : 0.f);
		for (int i = 0; i < w; i++) {
			int ca = a;
			if (dy > 0.f) {
				float dx = i < r ? r - i - 0.5f : (i >= w - r ? i - (w - r) + 0.5f : 0.f);
				if (dx > 0.f) { float d = sqrtf(dx * dx + dy * dy) - r; ca = cov(d) * a / 255; }
			}
			if (ca) px(x + i, y + j, c, ca);
		}
	}
}
/* rounded rect outline */
void gfx_rrect_stroke(int x, int y, int w, int h, int r, float th, u32 c, int a)
{
	if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
	for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
		float dx = i < r ? r - i - 0.5f : (i >= w - r ? i - (w - r) + 0.5f : 0.f);
		float dy = j < r ? r - j - 0.5f : (j >= h - r ? j - (h - r) + 0.5f : 0.f);
		float d;
		if (dx > 0.f && dy > 0.f) d = r - sqrtf(dx * dx + dy * dy);
		else { float ex = i + 0.5f < w - i - 0.5f ? i + 0.5f : w - i - 0.5f, ey = j + 0.5f < h - j - 0.5f ? j + 0.5f : h - j - 0.5f; d = ex < ey ? ex : ey; }
		int cv = cov(fabsf(d - th / 2) - th / 2);
		if (cv) px(x + i, y + j, c, cv * a / 255);
	}
}
void gfx_circle(float cx, float cy, float r, u32 rgb, int a)
{
	int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1), y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
	for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
		float dx = x + 0.5f - cx, dy = y + 0.5f - cy; int c = cov(sqrtf(dx * dx + dy * dy) - r);
		if (c) px(x, y, rgb, c * a / 255);
	}
}
void gfx_ring(float cx, float cy, float r, float th, u32 rgb, int a) { gfx_arc(cx, cy, r, th, 0, 6.2832f, rgb, a); }
void gfx_arc(float cx, float cy, float r, float th, float a0, float a1, u32 rgb, int a)
{
	float R = r + th / 2 + 1;
	int x0 = (int)floorf(cx - R), x1 = (int)ceilf(cx + R), y0 = (int)floorf(cy - R), y1 = (int)ceilf(cy + R);
	bool full = a1 - a0 >= 6.28f;
	for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
		float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
		int c = cov(fabsf(sqrtf(dx * dx + dy * dy) - r) - th / 2);
		if (!c) continue;
		if (!full) { float t = atan2f(dy, dx); if (t < a0) t += 6.2832f; if (t < a0 || t > a1) { t += 6.2832f; if (t > a1) continue; } }
		px(x, y, rgb, c * a / 255);
	}
}
void gfx_line(float x0, float y0, float x1, float y1, float th, u32 rgb, int a)
{
	float minx = fminf(x0, x1) - th, maxx = fmaxf(x0, x1) + th, miny = fminf(y0, y1) - th, maxy = fmaxf(y0, y1) + th;
	float vx = x1 - x0, vy = y1 - y0, L2 = vx * vx + vy * vy; if (L2 < 1e-6f) L2 = 1e-6f;
	for (int y = (int)miny; y <= (int)maxy; y++) for (int x = (int)minx; x <= (int)maxx; x++) {
		float px_ = x + 0.5f - x0, py_ = y + 0.5f - y0; float t = (px_ * vx + py_ * vy) / L2; t = t < 0 ? 0 : (t > 1 ? 1 : t);
		float dx = px_ - t * vx, dy = py_ - t * vy; int c = cov(sqrtf(dx * dx + dy * dy) - th / 2);
		if (c) px(x, y, rgb, c * a / 255);
	}
}
static float edge(float ax, float ay, float bx, float by, float x, float y) { return (bx - ax) * (y - ay) - (by - ay) * (x - ax); }
void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 rgb, int a)
{
	int mx = (int)fminf(x0, fminf(x1, x2)), Mx = (int)fmaxf(x0, fmaxf(x1, x2)) + 1, my = (int)fminf(y0, fminf(y1, y2)), My = (int)fmaxf(y0, fmaxf(y1, y2)) + 1;
	float ar = edge(x0, y0, x1, y1, x2, y2); if (fabsf(ar) < 1e-3f) return;
	for (int y = my; y <= My; y++) for (int x = mx; x <= Mx; x++) {
		int hits = 0;
		for (int s = 0; s < 4; s++) {
			float sx = x + 0.25f + 0.5f * (s & 1), sy = y + 0.25f + 0.5f * (s >> 1);
			float w0 = edge(x1, y1, x2, y2, sx, sy), w1 = edge(x2, y2, x0, y0, sx, sy), w2 = edge(x0, y0, x1, y1, sx, sy);
			if (ar > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) hits++;
		}
		if (hits) px(x, y, rgb, a * hits / 4);
	}
}
void gfx_star(float cx, float cy, float r, u32 rgb, int a)
{
	float ox[10], oy[10];
	for (int i = 0; i < 10; i++) { float ang = -1.5708f + i * 0.6283f, rr = (i & 1) ? r * 0.45f : r; ox[i] = cx + cosf(ang) * rr; oy[i] = cy + sinf(ang) * rr; }
	for (int i = 0; i < 10; i++) gfx_tri(cx, cy, ox[i], oy[i], ox[(i + 1) % 10], oy[(i + 1) % 10], rgb, a);
}

/* ---------------- text ---------------- */
static int next_char(const unsigned char *s, int *out)
{
	if (s[0] < 0x80) { *out = s[0]; return 1; }
	if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { int cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); *out = cp < 256 ? cp : '?'; return 2; }
	int n = (s[0] & 0xF0) == 0xE0 ? 3 : ((s[0] & 0xF8) == 0xF0 ? 4 : 1);
	int cp = 0;
	if (n == 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
	for (int i = 1; i < n; i++) if (!s[i]) { n = i; break; }
	/* common typographic chars -> Latin-1 */
	if (cp == 0x2019 || cp == 0x2018) *out = '\''; else if (cp == 0x201C || cp == 0x201D) *out = '"';
	else if (cp == 0x2013 || cp == 0x2014) *out = '-'; else if (cp == 0x2022) *out = 0xB7; else *out = '?';
	return n;
}
int gfx_line_h(const FontDef *f) { return f->line; }
static int glyph(const FontDef *f, int x, int base, int c, u32 rgb, int a)
{
	if (c < 32 || c > 255) c = '?';
	const FontGlyph &g = f->g[c - 32];
	const unsigned char *p = f->px + g.off;
	for (int j = 0; j < g.h; j++) for (int i = 0; i < g.w; i++) { int v = p[j * g.w + i]; if (v) px(x + g.bx + i, base + g.by + j, rgb, v * a / 255); }
	return g.adv;
}
int gfx_text(const FontDef *f, int x, int y, const char *utf8, u32 rgb, int a)
{
	const unsigned char *s = (const unsigned char *)utf8; int x0 = x, base = y + f->ascent, c;
	while (*s) { s += next_char(s, &c); x += glyph(f, x, base, c, rgb, a); }
	return x - x0;
}
int gfx_text_w(const FontDef *f, const char *utf8)
{
	const unsigned char *s = (const unsigned char *)utf8; int w = 0, c;
	while (*s) { s += next_char(s, &c); if (c < 32 || c > 255) c = '?'; w += f->g[c - 32].adv; }
	return w;
}
int gfx_text_fit(const FontDef *f, int x, int y, const char *utf8, int maxw, u32 rgb, int a)
{
	if (gfx_text_w(f, utf8) <= maxw) return gfx_text(f, x, y, utf8, rgb, a);
	int ell = gfx_text_w(f, "..."), w = 0, base = y + f->ascent, c;
	const unsigned char *s = (const unsigned char *)utf8;
	while (*s) {
		const unsigned char *n = s + next_char(s, &c); int cc = (c < 32 || c > 255) ? '?' : c;
		int adv = f->g[cc - 32].adv; if (w + adv + ell > maxw) break;
		glyph(f, x + w, base, cc, rgb, a); w += adv; s = n;
	}
	return w + gfx_text(f, x + w, y, "...", rgb, a);
}

/* ---------------- controller glyphs ---------------- */
u32 GFX_BTN_BG = 0x1B2638, GFX_BTN_FG = 0xC9D4E5;
int gfx_button(const char *k, int x, int cy, int sz)
{
	float r = sz / 2.f, cx = x + r, th = sz > 24 ? 2.6f : 2.f;
	if (!k[1]) {
		gfx_circle(cx, cy, r, GFX_BTN_BG, 255);
		float q = r * 0.42f;
		switch (k[0]) {
		case 'X': gfx_line(cx - q, cy - q, cx + q, cy + q, th, 0x8DB4FF, 255); gfx_line(cx - q, cy + q, cx + q, cy - q, th, 0x8DB4FF, 255); break;
		case 'O': gfx_ring(cx, cy, q, th, 0xFF7A85, 255); break;
		case 'T': gfx_line(cx, cy - q, cx - q, cy + q * 0.75f, th, 0x6BE3A8, 255); gfx_line(cx - q, cy + q * 0.75f, cx + q, cy + q * 0.75f, th, 0x6BE3A8, 255); gfx_line(cx + q, cy + q * 0.75f, cx, cy - q, th, 0x6BE3A8, 255); break;
		case 'S': { int s = (int)(q * 2); gfx_rect(cx - q, cy - q, s, (int)th, 0xF4A6DA); gfx_rect(cx - q, cy + q - th, s, (int)th, 0xF4A6DA); gfx_rect(cx - q, cy - q, (int)th, s, 0xF4A6DA); gfx_rect(cx + q - th, cy - q, (int)th, s, 0xF4A6DA); break; }
		}
		return sz;
	}
	int tw = gfx_text_w(F_SMALL, k), w = tw + sz / 2 + 4;
	gfx_rrect(x, cy - sz / 2, w, sz, sz / 2, GFX_BTN_BG, 255);
	gfx_text(F_SMALL, x + (w - tw) / 2, cy - F_SMALL->line / 2, k, GFX_BTN_FG);
	return w;
}

void gfx_present(u32 *vram) { memcpy(vram, backbuf, (size_t)W * H * 4); }
void gfx_target_size(int w, int h) { W = w; H = h; }   /* draw into a buffer of another size (set target first) */
