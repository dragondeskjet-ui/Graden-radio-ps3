/* UI + globe renderer tuned for 60 fps on the PS3 PPU:
 *  - globe rendered at half resolution from a per-pixel lookup table (lat/lon offsets + lighting),
 *    rebuilt only when latitude or zoom change; longitude panning is a pure texture offset
 *  - texture mip chain (Blue Marble) picked by zoom, bilinear only when magnifying
 *  - 2x bilinear upscale, both stages split across the two PPU hardware threads
 *  - station dots projected with precomputed unit vectors and drawn with prebuilt AA sprites
 *  - static panels cached in a layer and composited with opaque row spans; only animated bits redrawn */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ui.h"
#include "gfx.h"
#include "player.h"

#define SPACE   0x03060D
#define ATMOS   0x5AA8FF
#define DOT_LO  0x2FD49A
#define DOT_HI  0x9BFFD6
#define ACCENT  0xFFC24B
#define TEXT    0xEEF3FA
#define DIM     0x91A0B8
#define PANEL   0x0C1624
#define PANEL2  0x101D31
#define LINE    0x1E2D45
#define SEL     0x1D3A66
#define ERR     0xFF6B6B
#define FAV     0xFF8FB1
#define KEYC    0x01070D18u   /* transparent marker for the UI layer (top byte 1) */

static int W, H, U, GW, GH;
static int PX, PY, PW, PH, CY, CH;
static float VCX, VCY;
static u32 *maplayer, *uilayer, *glo, *hbuf, *bglo;

/* ---------- parallel ---------- */
static void par_serial(UiJob job, void *arg) { job(0, arg); job(1, arg); }
static void (*g_par)(UiJob, void *) = par_serial;
void ui_set_parallel(void (*par)(UiJob, void *)) { g_par = par ? par : par_serial; }

/* ---------- texture mips (RGB565 native u16) ---------- */
#define NLEV 5
static u16 *tex[NLEV]; static int texw[NLEV], texh[NLEV]; static int nlev = 0;

static inline u32 c565(u16 t) { return ((u32)(t & 0xF800) << 8) | ((u32)(t & 0x07E0) << 5) | ((u32)(t & 0x1F) << 3); }

bool ui_load_earth(const char *path)
{
	FILE *f = fopen(path, "rb"); if (!f) return false;
	int w = 4096, h = 2048;
	u16 *t = (u16 *)malloc((size_t)w * h * 2);
	unsigned char *raw = (unsigned char *)t;
	bool ok = t && fread(raw, 2, (size_t)w * h, f) == (size_t)w * h;
	fclose(f);
	if (!ok) { free(t); return false; }
	for (size_t i = 0; i < (size_t)w * h; i++) t[i] = (u16)((raw[i * 2] << 8) | raw[i * 2 + 1]);   /* BE file -> native */
	tex[0] = t; texw[0] = w; texh[0] = h; nlev = 1;
	while (nlev < NLEV) {
		int pw = texw[nlev - 1], ph = texh[nlev - 1], nw = pw / 2, nh = ph / 2;
		u16 *p = tex[nlev - 1], *n = (u16 *)malloc((size_t)nw * nh * 2);
		if (!n) break;
		for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
			u16 a = p[(2 * y) * pw + 2 * x], b = p[(2 * y) * pw + 2 * x + 1], c = p[(2 * y + 1) * pw + 2 * x], d = p[(2 * y + 1) * pw + 2 * x + 1];
			u32 r = ((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11) + 2) >> 2;
			u32 g = (((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((d >> 5) & 63) + 2) >> 2;
			u32 bl = ((a & 31) + (b & 31) + (c & 31) + (d & 31) + 2) >> 2;
			n[y * nw + x] = (u16)((r << 11) | (g << 5) | bl);
		}
		tex[nlev] = n; texw[nlev] = nw; texh[nlev] = nh; nlev++;
	}
	return true;
}

static void make_fallback_tex()
{
	int w = 512, h = 256; u16 *t = (u16 *)malloc(w * h * 2);
	for (int i = 0; i < w * h; i++) t[i] = 0x0947;   /* plain ocean blue */
	tex[0] = t; texw[0] = w; texh[0] = h; nlev = 1;
}

/* ---------- fast math ---------- */
static inline float fatan2(float y, float x)
{
	float ax = fabsf(x), ay = fabsf(y), mn = ax < ay ? ax : ay, mx = ax < ay ? ay : ax;
	float a = mn / (mx + 1e-20f), s = a * a;
	float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
	if (ay > ax) r = 1.57079637f - r;
	if (x < 0) r = 3.14159274f - r;
	return y < 0 ? -r : r;
}
static inline unsigned hsh(unsigned x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
static inline u32 lerpc(u32 a, u32 b, float t)
{
	int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
	return ((u32)(ar + (int)((br - ar) * t)) << 16) | ((u32)(ag + (int)((bg - ag) * t)) << 8) | (u32)(ab + (int)((bb - ab) * t));
}

/* ---------- globe lookup table (half resolution) ---------- */
static s32 *lut_v, *lut_u;       /* texel row/col offset, 8.8 fixed (level 0 units scaled per level) */
static u16 *lut_sh; static u8 *lut_rim;
static float lut_lat = 1e9f, lut_zoom = -1; static int lut_lev = -1;
static float g_R;                /* full-res globe radius */

float ui_globe_radius(const UiModel *m) { return fminf(W - PX - PW, H) * 0.43f * m->zoom; }
void ui_view_center(float *x, float *y) { *x = VCX; *y = VCY; }

static int pick_level(float Rlo)
{
	/* texels per degree at level L vs low-res pixels per degree at the centre */
	float pxdeg = Rlo * 3.14159f / 180.f;
	int L = 0;
	while (L + 1 < nlev && texw[L] / 360.f > pxdeg * 1.3f) L++;
	return L;
}

struct LutJob { float lat0, Rlo; int lev; };
static void lut_rows(int part, void *arg)
{
	LutJob *j = (LutJob *)arg;
	const float d = 0.0174533f;
	float s0 = sinf(j->lat0 * d), c0 = cosf(j->lat0 * d), R = j->Rlo;
	float vcx = VCX / 2.f, vcy = VCY / 2.f;
	float tw = (float)texw[j->lev], th = (float)texh[j->lev];
	int y0 = part == 0 ? 0 : GH / 2, y1 = part == 0 ? GH / 2 : GH;
	for (int y = y0; y < y1; y++) {
		float ny = (vcy - y - 0.5f) / R;
		for (int x = 0; x < GW; x++) {
			int i = y * GW + x;
			float nx = (x + 0.5f - vcx) / R, r2 = nx * nx + ny * ny;
			if (r2 >= 1.f) {
				lut_sh[i] = 0;
				float r = sqrtf(r2); u32 c = SPACE;
				unsigned hs = hsh((unsigned)x * 7919u + (unsigned)y * 104729u);
				if ((hs & 1023) < 4) c = lerpc(SPACE, 0xBFD0F0, ((hs >> 10) & 255) / 255.f);
				if (r < 1.07f) { float t = 1.f - (r - 1.f) / 0.07f; c = lerpc(c, ATMOS, t * t * 0.6f); }
				bglo[i] = c;
				continue;
			}
			float z = sqrtf(1.f - r2);
			float sy = ny * c0 + z * s0;                       /* sin(lat) */
			float lat = fatan2(sy, sqrtf(1.f - sy * sy));      /* asin */
			float dlon = fatan2(nx, z * c0 - ny * s0);
			lut_v[i] = (s32)(((1.5707963f - lat) / 3.1415927f * th - 0.5f) * 256.f);
			lut_u[i] = (s32)((dlon / 6.2831853f * tw) * 256.f);
			float lam = nx * -0.45f + ny * 0.55f + z * 0.70f; if (lam < 0) lam = 0;
			float k = 0.40f + 0.80f * lam;
			lut_sh[i] = (u16)(k * 256.f) | 1;                  /* never 0 (0 = background) */
			float rim = 1.f - z; rim = rim * rim * rim;
			lut_rim[i] = (u8)(rim * 0.6f * 255.f);
		}
	}
}

/* ---------- globe render from LUT ---------- */
struct RenderJob { int lev; s32 u0; bool bil; };
static void render_rows(int part, void *arg)
{
	RenderJob *j = (RenderJob *)arg;
	const u16 *T = tex[j->lev]; int tw = texw[j->lev], th = texh[j->lev], mask = tw - 1;
	const int ar = (ATMOS >> 16) & 255, ag = (ATMOS >> 8) & 255, ab = ATMOS & 255;
	int y0 = part == 0 ? 0 : GH / 2, y1 = part == 0 ? GH / 2 : GH;
	for (int y = y0; y < y1; y++) {
		int i = y * GW;
		for (int x = 0; x < GW; x++, i++) {
			u32 sh = lut_sh[i];
			if (!sh) { glo[i] = bglo[i]; continue; }
			int r, g, b;
			s32 u = j->u0 + lut_u[i], v = lut_v[i];
			if (!j->bil) {
				int tx = (u >> 8) & mask, ty = v >> 8; if (ty < 0) ty = 0; if (ty >= th) ty = th - 1;
				u16 t = T[ty * tw + tx];
				r = (t >> 8) & 0xF8; g = (t >> 3) & 0xFC; b = (t << 3) & 0xF8;
			} else {
				int tx = u >> 8, ty = v >> 8, fx = u & 255, fy = v & 255;
				int tx1 = (tx + 1) & mask; tx &= mask;
				int ty1 = ty + 1; if (ty < 0) ty = 0; if (ty >= th) ty = th - 1; if (ty1 >= th) ty1 = th - 1; if (ty1 < 0) ty1 = 0;
				u16 a = T[ty * tw + tx], bb = T[ty * tw + tx1], c = T[ty1 * tw + tx], dd = T[ty1 * tw + tx1];
				int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;
				r = (((a >> 8) & 0xF8) * w00 + ((bb >> 8) & 0xF8) * w10 + ((c >> 8) & 0xF8) * w01 + ((dd >> 8) & 0xF8) * w11) >> 16;
				g = (((a >> 3) & 0xFC) * w00 + ((bb >> 3) & 0xFC) * w10 + ((c >> 3) & 0xFC) * w01 + ((dd >> 3) & 0xFC) * w11) >> 16;
				b = (((a << 3) & 0xF8) * w00 + ((bb << 3) & 0xF8) * w10 + ((c << 3) & 0xF8) * w01 + ((dd << 3) & 0xF8) * w11) >> 16;
			}
			r = (r * (int)sh) >> 8; g = (g * (int)sh) >> 8; b = (b * (int)sh) >> 8;
			if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
			int rim = lut_rim[i];
			if (rim) { r += ((ar - r) * rim) >> 8; g += ((ag - g) * rim) >> 8; b += ((ab - b) * rim) >> 8; }
			glo[i] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
		}
	}
}

/* ---------- 2x bilinear upscale: glo (GW x GH) -> maplayer (W x H) ---------- */
static inline u32 mix31(u32 a, u32 b) { return (((a & 0xFCFCFC) >> 2) * 3) + ((b & 0xFCFCFC) >> 2); }  /* 3/4 a + 1/4 b */
static void up_rows(int part, void *)
{
	int y0 = part == 0 ? 0 : GH / 2, y1 = part == 0 ? GH / 2 : GH;
	/* horizontal pass for needed source rows (y0-1 .. y1) */
	int hy0 = y0 > 0 ? y0 - 1 : 0, hy1 = y1 < GH ? y1 + 1 : GH;
	for (int y = hy0; y < hy1; y++) {
		const u32 *s = glo + y * GW; u32 *o = hbuf + y * W;
		for (int x = 0; x < GW; x++) {
			u32 c = s[x], l = s[x > 0 ? x - 1 : 0], r = s[x + 1 < GW ? x + 1 : GW - 1];
			o[2 * x] = mix31(c, l); if (2 * x + 1 < W) o[2 * x + 1] = mix31(c, r);
		}
	}
	for (int y = y0; y < y1; y++) {
		const u32 *c = hbuf + y * W, *u = hbuf + (y > 0 ? y - 1 : 0) * W, *d = hbuf + (y + 1 < GH ? y + 1 : GH - 1) * W;
		u32 *o0 = maplayer + (2 * y) * W, *o1 = maplayer + (2 * y + 1) * W;
		for (int x = 0; x < W; x++) { o0[x] = mix31(c[x], u[x]); if (2 * y + 1 < H) o1[x] = mix31(c[x], d[x]); }
	}
}

/* ---------- places ---------- */
static float *pvx, *pvy, *pvz; static const Place *g_pl; static int g_np;
void ui_set_places(const Place *p, int n)
{
	free(pvx); free(pvy); free(pvz);
	pvx = (float *)malloc(n * 4 + 4); pvy = (float *)malloc(n * 4 + 4); pvz = (float *)malloc(n * 4 + 4);
	const float d = 0.0174533f;
	for (int i = 0; i < n; i++) { float la = p[i].lat * d, lo = p[i].lon * d; pvx[i] = cosf(la) * cosf(lo); pvy[i] = cosf(la) * sinf(lo); pvz[i] = sinf(la); }
	g_pl = p; g_np = n;
}
struct Basis { float ex, ey, nx, ny, nz, cx, cy, cz; };
static Basis basis(const UiModel *m)
{
	const float d = 0.0174533f; float sl = sinf(m->lon * d), cl = cosf(m->lon * d), sp = sinf(m->lat * d), cp = cosf(m->lat * d);
	Basis b = { -sl, cl, -sp * cl, -sp * sl, cp, cp * cl, cp * sl, sp };
	return b;
}
static inline bool proj(const Basis &b, float R, int i, float *x, float *y)
{
	float depth = pvx[i] * b.cx + pvy[i] * b.cy + pvz[i] * b.cz;
	if (depth < 0.03f) return false;
	*x = VCX + R * (pvx[i] * b.ex + pvy[i] * b.ey);
	*y = VCY - R * (pvx[i] * b.nx + pvy[i] * b.ny + pvz[i] * b.nz);
	return true;
}
bool ui_project(const UiModel *m, float lon, float lat, float *x, float *y)
{
	const float d = 0.0174533f; Basis b = basis(m);
	float px = cosf(lat * d) * cosf(lon * d), py = cosf(lat * d) * sinf(lon * d), pz = sinf(lat * d);
	if (px * b.cx + py * b.cy + pz * b.cz < 0.03f) return false;
	float R = ui_globe_radius(m);
	*x = VCX + R * (px * b.ex + py * b.ey); *y = VCY - R * (px * b.nx + py * b.ny + pz * b.nz);
	return true;
}
int ui_pick(const UiModel *m)
{
	if (!g_pl || !m->nplaces) return -1;
	Basis b = basis(m); float R = ui_globe_radius(m), lim = 4.5f * U, best = 1e30f; int hit = -1;
	for (int i = 0; i < g_np; i++) {
		float x, y; if (!proj(b, R, i, &x, &y)) continue;
		float dx = x - VCX, dy = y - VCY; if (dx > lim || dx < -lim || dy > lim || dy < -lim) continue;
		float s = (dx * dx + dy * dy) / (1.f + 0.15f * sqrtf((float)g_pl[i].size));
		if (dx * dx + dy * dy <= lim * lim && s < best) { best = s; hit = i; }
	}
	return hit;
}

/* AA disc sprites: radius = 0.75 + k*0.25 */
#define NSPR 30
static unsigned char *spr[NSPR]; static int sprn[NSPR];
static void make_sprites()
{
	for (int k = 0; k < NSPR; k++) {
		float r = 0.75f + k * 0.25f; int n = 2 * (int)ceilf(r) + 3; sprn[k] = n; spr[k] = (unsigned char *)malloc(n * n);
		float c = n / 2.f;
		for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
			float dx = x + 0.5f - c, dy = y + 0.5f - c, e = 0.5f - (sqrtf(dx * dx + dy * dy) - r);
			spr[k][y * n + x] = (unsigned char)(e <= 0 ? 0 : (e >= 1 ? 255 : e * 255));
		}
	}
}
static inline u32 blend(u32 d, u32 s, int a)
{
	u32 rb = ((((s & 0xFF00FF) * a) + ((d & 0xFF00FF) * (255 - a))) >> 8) & 0xFF00FF;
	u32 g = ((((s & 0x00FF00) * a) + ((d & 0x00FF00) * (255 - a))) >> 8) & 0x00FF00;
	return rb | g;
}
static void stamp(u32 *fb, float cx, float cy, float r, u32 col, int alpha)
{
	int k = (int)((r - 0.75f) / 0.25f + 0.5f); if (k < 0) k = 0; if (k >= NSPR) k = NSPR - 1;
	int n = sprn[k], x0 = (int)cx - n / 2, y0 = (int)cy - n / 2;
	const unsigned char *s = spr[k];
	for (int y = 0; y < n; y++) {
		int yy = y0 + y; if ((unsigned)yy >= (unsigned)H) continue;
		u32 *row = fb + yy * W;
		for (int x = 0; x < n; x++) {
			int xx = x0 + x, a = s[y * n + x]; if (!a || (unsigned)xx >= (unsigned)W) continue;
			row[xx] = blend(row[xx], col, a * alpha >> 8);
		}
	}
}
static void draw_dots(const UiModel *m)
{
	if (!g_pl) return;
	Basis b = basis(m); float R = ui_globe_radius(m);
	float zr = (m->zoom < 2 ? 0.9f : (m->zoom < 8 ? 1.25f : 1.8f)) * (U / 8.f);
	for (int i = 0; i < g_np; i++) {
		float x, y; if (!proj(b, R, i, &x, &y)) continue;
		if (x < -8 || y < -8 || x > W + 8 || y > H + 8) continue;
		int sz = g_pl[i].size; float t = sz >= 40 ? 1.f : sz / 40.f;
		float r = (0.9f + 1.4f * sqrtf(t)) * zr;
		if (t > 0.3f) stamp(maplayer, x, y, r * 2.2f, DOT_LO, 40);
		stamp(maplayer, x, y, r, lerpc(DOT_LO, DOT_HI, t), 170 + (int)(85 * t));
	}
}

static void render_globe(const UiModel *m)
{
	g_R = ui_globe_radius(m);
	float Rlo = g_R / 2.f;
	int lev = pick_level(Rlo);
	if (m->lat != lut_lat || m->zoom != lut_zoom || lev != lut_lev) {
		LutJob j = { m->lat, Rlo, lev };
		g_par(lut_rows, &j);
		lut_lat = m->lat; lut_zoom = m->zoom; lut_lev = lev;
	}
	RenderJob rj;
	rj.lev = lev;
	rj.u0 = (s32)(((m->lon + 180.f) / 360.f * texw[lev]) * 256.f);
	rj.bil = lev == 0 && (texw[0] / 360.f) < Rlo * 3.14159f / 180.f;   /* magnifying */
	g_par(render_rows, &rj);
	g_par(up_rows, NULL);
	draw_dots(m);
}

/* ---------- UI layer (cached) ---------- */
struct Dyn { char kind; float x, y, s; int on; };   /* 'e' equalizer, 's' spinner */
static Dyn dyn[16]; static int ndyn = 0; static bool layer_pass = false;
static u32 *span_y; static u16 *span_x, *span_n; static int nspan = 0, spancap = 0;

static void spinner(float cx, float cy, float r, unsigned f)
{
	if (layer_pass) { if (ndyn < 16) { Dyn d = { 's', cx, cy, r, 0 }; dyn[ndyn++] = d; } return; }
	gfx_ring(cx, cy, r, U * 0.5f, 0x22324D, 255);
	float a0 = fmodf(f * 0.15f, 6.2832f);
	gfx_arc(cx, cy, r, U * 0.5f, a0, a0 + 1.9f, ACCENT, 255);
}
static void eq(int x, int y, int h, unsigned f, bool on)
{
	if (layer_pass) { if (ndyn < 16) { Dyn d = { 'e', (float)x, (float)y, (float)h, on }; dyn[ndyn++] = d; } return; }
	for (int i = 0; i < 4; i++) {
		float v = on ? 0.35f + 0.65f * fabsf(sinf(f * (0.11f + i * 0.037f) + i * 1.3f)) : 0.25f;
		int bh = (int)(h * v);
		gfx_rrect(x + i * (U + U / 2), y + h - bh, U, bh, U / 2, ACCENT, 255);
	}
}
static int hint(int x, int cy, const char *b1, const char *b2, const char *label)
{
	int bs = 3 * U;
	x += gfx_button(b1, x, cy, bs) + (b2 ? U / 2 : U);
	if (b2) x += gfx_button(b2, x, cy, bs) + U;
	return x + gfx_text(F_SMALL, x, cy - gfx_line_h(F_SMALL) / 2, label, DIM) + 3 * U;
}
static void badge(int cx, int cy, int n)
{
	char t[12]; snprintf(t, sizeof t, "%d", n);
	gfx_circle(cx, cy, 3.4f * U, TEXT, 255);
	const FontDef *f = strlen(t) > 3 ? F_SMALL : F_BOLD;
	gfx_text(f, cx - gfx_text_w(f, t) / 2, cy - gfx_line_h(f) / 2, t, PANEL);
}
static void panel_header(const char *title, const char *sub, int count, bool has_badge)
{
	int hh = 14 * U;
	gfx_rrect(PX, PY, PW, hh + 2 * U, 2 * U, PANEL2, 255);
	gfx_rect(PX, PY + hh, PW, 2 * U, PANEL);
	int tx = PX + 3 * U;
	if (has_badge) { badge(PX + 6 * U, PY + hh / 2, count); tx = PX + 11 * U; }
	gfx_text_fit(F_TITLE, tx, PY + hh / 2 - gfx_line_h(F_TITLE) + U / 2, title, PW - (tx - PX) - 3 * U, TEXT);
	gfx_text_fit(F_BODY, tx, PY + hh / 2 + U / 2, sub, PW - (tx - PX) - 3 * U, DIM);
	gfx_rect(PX, PY + hh, PW, 1, LINE);
}
static int list_top_y(bool search) { return PY + (search ? 26 : 19) * U; }
static int rows_for(bool search, bool two_line)
{
	int rh = two_line ? gfx_line_h(F_BODY) + gfx_line_h(F_SMALL) + 2 * U : gfx_line_h(F_BODY) + 2 * U;
	return (PY + PH - 2 * U - list_top_y(search)) / rh;
}
int ui_list_rows() { return rows_for(false, true); }
int ui_list_rows_ex(bool search, bool two_line) { return rows_for(search, two_line); }

typedef void (*RowFn)(const UiModel *, int, int, int, int, bool);
static void panel_list(const UiModel *m, const char *section, int n, int sel, int top, RowFn row, bool loading, const char *err, const char *empty, bool search, bool two)
{
	int x = PX + 3 * U, w = PW - 6 * U;
	int y = PY + 14 * U + 2 * U;
	if (search) {
		int sh = gfx_line_h(F_BODY) + 2 * U;
		gfx_rrect(x, y, w, sh, sh / 2, 0x16243B, 255);
		gfx_ring(x + 2.5f * U, y + sh / 2.f - 1, U * 0.8f, 1.6f, DIM, 255);
		gfx_line(x + 3.1f * U, y + sh / 2.f + U * 0.6f, x + 3.8f * U, y + sh / 2.f + U * 1.3f, 1.8f, DIM, 255);
		const char *q = m->query && m->query[0] ? m->query : "Buscar país...";
		int tw = gfx_text_fit(F_BODY, x + 5 * U, y + U, q, w - 12 * U, m->query && m->query[0] ? TEXT : DIM);
		if (m->kb_open) gfx_rect(x + 5 * U + tw + 2, y + U + 2, 2, gfx_line_h(F_BODY) - 4, ACCENT);
		gfx_button("S", x + w - 4 * U, y + sh / 2, 3 * U);
		y += sh + 2 * U;
	}
	gfx_text(F_BOLD, x, y, section, TEXT);
	y += gfx_line_h(F_BOLD) + U;
	gfx_rect(x, y, w, 1, LINE);
	int ly = list_top_y(search), rh = two ? gfx_line_h(F_BODY) + gfx_line_h(F_SMALL) + 2 * U : gfx_line_h(F_BODY) + 2 * U, rows = rows_for(search, two);
	if (loading) { spinner(PX + PW / 2.f, ly + 8 * U, 2.4f * U, m->frame); const char *t = "Cargando emisoras..."; gfx_text(F_SMALL, PX + (PW - gfx_text_w(F_SMALL, t)) / 2, ly + 12 * U, t, DIM); return; }
	if (err) { gfx_text_fit(F_BODY, x, ly + 2 * U, err, w, ERR); return; }
	if (n == 0) { gfx_text_fit(F_BODY, x, ly + 2 * U, empty, w, DIM); return; }
	for (int i = 0; i < rows && top + i < n; i++) {
		int idx = top + i, ry = ly + i * rh;
		bool s = idx == sel && !m->kb_open;
		if (s) { gfx_rrect(PX + U, ry, PW - 3 * U, rh - U / 2, U, SEL, 255); gfx_rrect(PX + U, ry + U, U / 2, rh - 2 * U - U / 2, U / 4, ACCENT, 255); }
		else if (i) gfx_rect(x, ry - U / 4, w, 1, LINE);
		row(m, idx, x, ry + U, w - 3 * U, s);
	}
	if (n > rows) {
		int th = rows * rh, shh = th * rows / n; if (shh < 3 * U) shh = 3 * U;
		int sy = ly + (th - shh) * top / (n - rows);
		gfx_rrect(PX + PW - U - U / 2, ly, U / 2, th, U / 4, LINE, 255);
		gfx_rrect(PX + PW - U - U / 2, sy, U / 2, shh, U / 4, DIM, 255);
	}
}

static void station_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Station &s = m->stations[i];
	bool playing = m->now_id && !strcmp(m->now_id, s.id) && m->pstate != P_IDLE;
	int ix = x;
	if (playing) { eq(x, y + 2, gfx_line_h(F_BODY) - 6, m->frame, m->pstate == P_PLAYING); ix += 7 * U; }
	bool fav = m->is_fav && m->is_fav(s.id);
	gfx_text_fit(sel ? F_BOLD : F_BODY, ix, y, s.name, w - (ix - x) - 3 * U, playing ? ACCENT : TEXT);
	if (fav) gfx_star(x + w + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.9f, FAV, 255);
	if (s.sub[0]) gfx_text_fit(F_SMALL, ix, y + gfx_line_h(F_BODY), s.sub, w - (ix - x), DIM);
}
static void fav_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Fav &f = m->favs[i];
	bool playing = m->now_id && !strcmp(m->now_id, f.id) && m->pstate != P_IDLE;
	int ix = x;
	if (playing) { eq(x, y + 2, gfx_line_h(F_BODY) - 6, m->frame, m->pstate == P_PLAYING); ix += 7 * U; }
	else { gfx_star(x + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.8f, FAV, 255); ix += 4 * U; }
	gfx_text_fit(sel ? F_BOLD : F_BODY, ix, y, f.name, w - (ix - x), playing ? ACCENT : TEXT);
	gfx_text_fit(F_SMALL, ix, y + gfx_line_h(F_BODY), f.place, w - (ix - x), DIM);
}
static void country_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Country *c = m->clist[i];
	char n[24]; snprintf(n, sizeof n, "%d", c->nstations);
	int nw = gfx_text_w(F_SMALL, n);
	gfx_text_fit(sel ? F_BOLD : F_BODY, x, y, c->name, w - nw - 2 * U, TEXT);
	gfx_text(F_SMALL, x + w - nw + 2 * U, y + (gfx_line_h(F_BODY) - gfx_line_h(F_SMALL)) / 2, n, DIM);
}
static void city_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Place &p = m->places[m->city_idx[i]];
	char n[16]; snprintf(n, sizeof n, "%d", p.size);
	int nw = gfx_text_w(F_SMALL, n);
	gfx_circle(x + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.55f, DOT_HI, 255);
	gfx_text_fit(sel ? F_BOLD : F_BODY, x + 3 * U, y, p.title, w - nw - 5 * U, TEXT);
	gfx_text(F_SMALL, x + w - nw + 2 * U, y + (gfx_line_h(F_BODY) - gfx_line_h(F_SMALL)) / 2, n, DIM);
}

const char *const UI_KB_KEYS[] = { "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","Espacio","Borrar","Listo" };
const int UI_KB_N = 29, UI_KB_COLS = 7;
static void keyboard(const UiModel *m)
{
	int rows = (UI_KB_N - 3 + UI_KB_COLS - 1) / UI_KB_COLS + 1;
	int kw = (PW - 6 * U - (UI_KB_COLS - 1) * U) / UI_KB_COLS, kh = gfx_line_h(F_BOLD) + 2 * U;
	int h = rows * (kh + U) + 3 * U, x0 = PX + 3 * U, y0 = PY + PH - h;
	gfx_rect(PX, y0 - U, PW, h + U - 2 * U, PANEL);
	gfx_rect(PX + 3 * U, y0 - U, PW - 6 * U, 1, LINE);
	for (int k = 0; k < UI_KB_N; k++) {
		int r, c, w = kw;
		if (k < 26) { r = k / UI_KB_COLS; c = k % UI_KB_COLS; }
		else { r = rows - 1; c = 0; w = (PW - 6 * U - 2 * U) / 3; }
		int x = k < 26 ? x0 + c * (kw + U) : x0 + (k - 26) * (w + U), y = y0 + U + r * (kh + U);
		bool s = k == m->kb_sel;
		gfx_rrect(x, y, w, kh, U, s ? ACCENT : 0x1A2840, 255);
		const FontDef *f = k < 26 ? F_BOLD : F_SMALL;
		gfx_text(f, x + (w - gfx_text_w(f, UI_KB_KEYS[k])) / 2, y + (kh - gfx_line_h(f)) / 2, UI_KB_KEYS[k], s ? PANEL : TEXT);
	}
}

static void nearby(const UiModel *m, int y)
{
	int x = PX + 3 * U, w = PW - 6 * U;
	if (m->nplaces <= 0) return;
	const int K = 8; int idx[K]; float dist[K]; int nk = 0;
	float cl0 = cosf(m->lat * 0.0174533f);
	for (int i = 0; i < m->nplaces; i++) {
		if (i == m->hover) continue;
		float dl = m->places[i].lon - m->lon; if (dl > 180) dl -= 360; if (dl < -180) dl += 360;
		float dx = dl * cl0, dy = m->places[i].lat - m->lat, dd = dx * dx + dy * dy;
		if (nk < K) { idx[nk] = i; dist[nk++] = dd; continue; }
		int wi = 0; for (int k = 1; k < K; k++) if (dist[k] > dist[wi]) wi = k;
		if (dd < dist[wi]) { idx[wi] = i; dist[wi] = dd; }
	}
	for (int a = 0; a < nk; a++) for (int b = a + 1; b < nk; b++) if (dist[b] < dist[a]) { float t = dist[a]; dist[a] = dist[b]; dist[b] = t; int q = idx[a]; idx[a] = idx[b]; idx[b] = q; }
	int rh = gfx_line_h(F_BODY) + U;
	int maxn = (PY + PH - 3 * U - y - gfx_line_h(F_BOLD) - 2 * U) / rh; if (maxn > nk) maxn = nk;
	if (maxn <= 0) return;
	gfx_text(F_BOLD, x, y, "Cerca de aquí", TEXT);
	y += gfx_line_h(F_BOLD) + U; gfx_rect(x, y, w, 1, LINE); y += U;
	for (int k = 0; k < maxn; k++) {
		const Place &q = m->places[idx[k]];
		char n[8]; snprintf(n, sizeof n, "%d", q.size); int nw = gfx_text_w(F_SMALL, n);
		gfx_circle(x + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.55f, DOT_HI, 255);
		gfx_text_fit(F_BODY, x + 3 * U, y, q.title, w - 3 * U - nw - 3 * U, TEXT);
		gfx_text(F_SMALL, x + w - nw, y + (gfx_line_h(F_BODY) - gfx_line_h(F_SMALL)) / 2, n, DIM);
		y += rh;
	}
}

static void panel(const UiModel *m)
{
	gfx_rrect(PX, PY, PW, PH, 2 * U, PANEL, 255);
	if (m->view == UV_STATIONS && m->panel_place) {
		const Place *p = m->panel_place;
		panel_header(p->title, p->country, m->loading == 2 ? p->size : m->nst, true);
		char sec[80]; snprintf(sec, sizeof sec, "Emisoras en %s", p->title);
		panel_list(m, sec, m->nst, m->st_sel, m->st_top, station_row, m->loading == 2, m->loading ? NULL : m->error, "Sin emisoras", false, true);
	} else if (m->view == UV_FAVS) {
		char sub[48]; snprintf(sub, sizeof sub, "%d %s", m->nfav, m->nfav == 1 ? "emisora guardada" : "emisoras guardadas");
		panel_header("Favoritos", sub, 0, false);
		panel_list(m, "Tus emisoras", m->nfav, m->fav_sel, m->fav_top, fav_row, false, NULL, "Aún no hay favoritos. Pulsa Cuadrado en una emisora.", false, true);
	} else if (m->view == UV_COUNTRIES) {
		char sub[48]; snprintf(sub, sizeof sub, "%d países", m->nclist);
		panel_header("Buscar por país", sub, 0, false);
		panel_list(m, m->query && m->query[0] ? "Resultados" : "Todos los países", m->nclist, m->c_sel, m->c_top, country_row, m->loading == 1, NULL, m->loading ? "" : "Ningún país coincide", true, false);
		if (m->kb_open) keyboard(m);
	} else if (m->view == UV_CITIES && m->cur_country) {
		char sub[64]; snprintf(sub, sizeof sub, "%d ciudades  \xC2\xB7  %d emisoras", m->cur_country->nplaces, m->cur_country->nstations);
		panel_header(m->cur_country->name, sub, 0, false);
		panel_list(m, "Ciudades", m->ncity, m->city_sel, m->city_top, city_row, false, NULL, "Sin ciudades", false, false);
	} else {
		int x = PX + 3 * U, w = PW - 6 * U;
		if (m->hover >= 0) {
			const Place &p = m->places[m->hover];
			panel_header(p.title, p.country, p.size, true);
			int y = PY + 18 * U;
			char t[64]; snprintf(t, sizeof t, "%d %s en esta ciudad", p.size, p.size == 1 ? "emisora" : "emisoras");
			gfx_text_fit(F_BODY, x, y, t, w, TEXT);
			hint(x, y + gfx_line_h(F_BODY) + 3 * U + U / 2, "X", NULL, "Ver emisoras");
			nearby(m, PY + 30 * U);
		} else {
			panel_header("Explorar", "Gira el globo y apunta a una ciudad", 0, false);
			int y = PY + 18 * U;
			const char *tips[][3] = { { "L1", "R1", "Acercar y alejar" }, { "Select", NULL, "Buscar por país" }, { "L3", NULL, "Ir a un lugar al azar" }, { "Start", NULL, "Favoritos" } };
			for (int i = 0; i < 4; i++) hint(x, y + i * 5 * U + U + U / 2, tips[i][0], tips[i][1], tips[i][2]);
			nearby(m, PY + 40 * U);
		}
		if (m->loading == 1) { int y = PY + PH - 12 * U; gfx_rect(PX + U, y - U, PW - 2 * U, 10 * U, PANEL); spinner(x + 2.5f * U, y + 2.5f * U, 2.f * U, m->frame); gfx_text_fit(F_BODY, x + 7 * U, y + U, "Cargando ciudades...", w - 7 * U, TEXT); }
		if (!m->loading && m->error) {
			int y = PY + PH - 14 * U;
			gfx_rect(PX + U, y - U, PW - 2 * U, 13 * U, PANEL);
			gfx_text_fit(F_BOLD, x, y, "No se pudo cargar el mapa", w, ERR);
			gfx_text_fit(F_SMALL, x, y + gfx_line_h(F_BOLD), m->error, w, DIM);
			hint(x, y + gfx_line_h(F_BOLD) + gfx_line_h(F_SMALL) + 3 * U, "X", NULL, "Reintentar");
		}
	}
}

static void player_card(const UiModel *m)
{
	gfx_rrect(PX, CY, PW, CH, 2 * U, PANEL, 255);
	int x = PX + 3 * U, w = PW - 6 * U, y = CY + 2 * U, st = m->pstate, ic = 5 * U;
	if (st == P_PLAYING) eq(x + w - ic, y + U / 2, ic - U, m->frame, true);
	else if (st == P_BUFFERING || st == P_CONNECTING) spinner(x + w - ic / 2.f, y + ic / 2.f, ic * 0.32f, m->frame);
	else if (st == P_ERROR) gfx_circle(x + w - ic / 2.f, y + ic / 2.f, ic * 0.3f, ERR, 255);
	int tw = w - ic - 2 * U;
	if (st == P_IDLE) {
		gfx_text_fit(F_BOLD, x, y, "Nada sonando", tw, TEXT);
		gfx_text_fit(F_SMALL, x, y + gfx_line_h(F_BOLD), "Elige una ciudad y pulsa X", tw, DIM);
	} else {
		int nw = gfx_text_fit(F_BOLD, x, y, m->now_name ? m->now_name : "", tw - 3 * U, ACCENT);
		if (m->is_fav && m->now_id && m->is_fav(m->now_id)) gfx_star(x + nw + 2 * U, y + gfx_line_h(F_BOLD) / 2.f, U * 0.9f, FAV, 255);
		char l2[200];
		if (st == P_PLAYING) snprintf(l2, sizeof l2, "%s  \xC2\xB7  %s", m->now_place ? m->now_place : "", m->pfmt && m->pfmt[0] ? m->pfmt : "EN VIVO");
		else snprintf(l2, sizeof l2, "%s  \xC2\xB7  %s", m->now_place ? m->now_place : "", m->pmsg ? m->pmsg : "");
		gfx_text_fit(F_SMALL, x, y + gfx_line_h(F_BOLD), l2, tw, st == P_ERROR ? ERR : DIM);
	}
	int cy = CY + CH - 4 * U;
	int bx = hint(x, cy, "T", NULL, "Parar");
	gfx_button("L2", bx, cy, 3 * U); bx += gfx_text_w(F_SMALL, "L2") + 3 * U / 2 + 4 + U;
	int ex = PX + PW - 3 * U - (gfx_text_w(F_SMALL, "R2") + 3 * U / 2 + 4);
	int bw = ex - U - bx;
	gfx_rrect(bx, cy - U / 4, bw, U / 2 + 1, U / 4, 0x26354F, 255);
	gfx_rrect(bx, cy - U / 4, (int)(bw * m->volume), U / 2 + 1, U / 4, ACCENT, 255);
	gfx_circle(bx + bw * m->volume, cy, U * 0.9f, TEXT, 255);
	gfx_button("R2", ex, cy, 3 * U);
}

static void hint_bar(const UiModel *m)
{
	int y = H - 4 * U, x = PX + PW + 4 * U;
	gfx_rrect(x - 2 * U, y - 2 * U - U / 2, W - x - U, 5 * U, 2 * U + U / 2, PANEL, 255);
	if (m->view == UV_MAP) { x = hint(x, y, "X", NULL, "Abrir"); x = hint(x, y, "L1", "R1", "Zoom"); x = hint(x, y, "Select", NULL, "Países"); hint(x, y, "Start", NULL, "Favoritos"); }
	else if (m->view == UV_STATIONS) { x = hint(x, y, "X", NULL, "Reproducir"); x = hint(x, y, "S", NULL, "Favorito"); x = hint(x, y, "L1", "R1", "Página"); hint(x, y, "O", NULL, "Volver"); }
	else if (m->view == UV_COUNTRIES && m->kb_open) { x = hint(x, y, "X", NULL, "Escribir"); x = hint(x, y, "S", NULL, "Borrar"); hint(x, y, "O", NULL, "Cerrar teclado"); }
	else if (m->view == UV_COUNTRIES) { x = hint(x, y, "X", NULL, "Ver ciudades"); x = hint(x, y, "S", NULL, "Teclado"); x = hint(x, y, "L1", "R1", "Página"); hint(x, y, "O", NULL, "Volver"); }
	else if (m->view == UV_CITIES) { x = hint(x, y, "X", NULL, "Emisoras"); x = hint(x, y, "L1", "R1", "Página"); hint(x, y, "O", NULL, "Países"); }
	else { x = hint(x, y, "X", NULL, "Reproducir"); x = hint(x, y, "S", NULL, "Quitar"); hint(x, y, "O", NULL, "Volver"); }
}

static void build_layer(const UiModel *m)
{
	for (int i = 0; i < W * H; i++) uilayer[i] = KEYC;
	gfx_set_target(uilayer);
	layer_pass = true; ndyn = 0;
	panel(m); player_card(m); hint_bar(m);
	layer_pass = false;
	gfx_set_target(NULL);
	/* opaque spans */
	nspan = 0;
	for (int y = 0; y < H; y++) {
		const u32 *r = uilayer + y * W; int x = 0;
		while (x < W) {
			while (x < W && (r[x] >> 24)) x++;
			int s = x;
			while (x < W && !(r[x] >> 24)) x++;
			if (x > s) {
				if (nspan == spancap) { spancap = spancap ? spancap * 2 : 4096; span_y = (u32 *)realloc(span_y, spancap * 4); span_x = (u16 *)realloc(span_x, spancap * 2); span_n = (u16 *)realloc(span_n, spancap * 2); }
				span_y[nspan] = y; span_x[nspan] = s; span_n[nspan] = x - s; nspan++;
			}
		}
	}
}

static void crosshair(const UiModel *m)
{
	bool hot = m->hover >= 0;
	float pulse = 0.5f + 0.5f * sinf(m->frame * 0.08f);
	gfx_ring(VCX, VCY, 5.5f * U + (hot ? pulse * U * 0.6f : 0), 2.2f, hot ? ACCENT : TEXT, hot ? 235 : 160);
	gfx_circle(VCX, VCY, 2.5f, TEXT, 230);
	if (hot) { float x, y; if (ui_project(m, m->places[m->hover].lon, m->places[m->hover].lat, &x, &y)) { gfx_circle(x, y, U * 0.75f, ACCENT, 255); gfx_ring(x, y, U * 1.4f, 1.5f, ACCENT, 200); } }
}

void ui_init(unsigned w, unsigned h)
{
	W = w; H = h; U = h >= 900 ? 12 : 8; GW = (W + 1) / 2; GH = (H + 1) / 2;
	gfx_init(w, h);
	size_t n = (size_t)w * h;
	maplayer = (u32 *)malloc(n * 4); uilayer = (u32 *)malloc(n * 4); hbuf = (u32 *)malloc((size_t)W * GH * 4);
	glo = (u32 *)malloc((size_t)GW * GH * 4); bglo = (u32 *)malloc((size_t)GW * GH * 4);
	lut_v = (s32 *)malloc((size_t)GW * GH * 4); lut_u = (s32 *)malloc((size_t)GW * GH * 4);
	lut_sh = (u16 *)malloc((size_t)GW * GH * 2); lut_rim = (u8 *)malloc((size_t)GW * GH);
	PX = 3 * U; PY = 3 * U; PW = W * 30 / 100; if (PW < 54 * U) PW = 54 * U;
	CH = 17 * U; CY = H - CH - 3 * U; PH = CY - 2 * U - PY;
	VCX = PX + PW + (W - PX - PW) / 2.f; VCY = H / 2.f;
	make_sprites();
}

void ui_render(UiModel *m)
{
	if (!nlev) make_fallback_tex();
	if (m->map_dirty) { render_globe(m); m->map_dirty = false; }
	u32 *bb = gfx_buffer();
	memcpy(bb, maplayer, (size_t)W * H * 4);
	if (m->ui_dirty) { build_layer(m); m->ui_dirty = false; }
	for (int i = 0; i < nspan; i++) memcpy(bb + span_y[i] * W + span_x[i], uilayer + span_y[i] * W + span_x[i], span_n[i] * 4);
	/* dynamic layer */
	if (m->view == UV_MAP) crosshair(m);
	for (int i = 0; i < ndyn; i++) { if (dyn[i].kind == 'e') eq((int)dyn[i].x, (int)dyn[i].y, (int)dyn[i].s, m->frame, dyn[i].on); else spinner(dyn[i].x, dyn[i].y, dyn[i].s, m->frame); }
	char t[80]; snprintf(t, sizeof t, "%.2f\xC2\xB0 %c   %.2f\xC2\xB0 %c   x%.1f   %d fps", fabsf(m->lat), m->lat >= 0 ? 'N' : 'S', fabsf(m->lon), m->lon >= 0 ? 'E' : 'O', m->zoom, (int)(m->fps + 0.5f));
	int tw = gfx_text_w(F_SMALL, t) + 4 * U;
	gfx_rrect(W - tw - 3 * U, 3 * U, tw, gfx_line_h(F_SMALL) + 2 * U, 2 * U, PANEL, 210);
	gfx_text(F_SMALL, W - tw - U, 4 * U, t, DIM);
	if (!m->tls) { int cw = gfx_text_w(F_SMALL, "TLS OFF") + 4 * U; gfx_rrect(W - cw - 3 * U, 7 * U + gfx_line_h(F_SMALL), cw, gfx_line_h(F_SMALL) + 2 * U, 2 * U, 0x4A1E28, 230); gfx_text(F_SMALL, W - cw - U, 8 * U + gfx_line_h(F_SMALL), "TLS OFF", ERR); }
	if (m->toast && m->toast_t > 0) {
		int a = m->toast_t > 20 ? 245 : m->toast_t * 12;
		int w2 = gfx_text_w(F_BODY, m->toast) + 6 * U, th = gfx_line_h(F_BODY) + 2 * U;
		int x = (int)VCX - w2 / 2, y = 3 * U;
		gfx_rrect(x, y, w2, th, th / 2, 0x26385A, a);
		gfx_text(F_BODY, x + 3 * U, y + U, m->toast, TEXT, a);
	}
}

bool ui_animating(const UiModel *m)
{
	return m->pstate == P_PLAYING || m->pstate == P_BUFFERING || m->pstate == P_CONNECTING || m->loading || m->toast_t > 0 || (m->view == UV_MAP && m->hover >= 0);
}
