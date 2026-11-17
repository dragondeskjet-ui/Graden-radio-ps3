/* UI: glass-style panels drawn by the PPU into a cached premultiplied-ARGB layer (only when something changes);
 * the GPU composites it over the globe every frame together with a few animated sprites. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ui.h"
#include "log.h"
#include "gfx.h"
#include "gpu.h"
#include "player.h"

static int W, H, U;
static int PX, PY, PW, PH, CY, CH;    /* left panel and player card */
static float VCX, VCY;                /* globe centre */
static u32 *layer, *hudbuf, *toastbuf, *atlas, *stars;
static int HUD_W, HUD_H, TOAST_W, TOAST_H;

/* ---------------- themes ---------------- */
struct Theme { const char *name; u32 top; int top_a; u32 bot; int bot_a; int gloss_a; int border_a; u32 text, dim, line; int line_a; u32 btn_bg, btn_fg; bool light; };
static Theme TH[UI_NTHEMES] = {
	{ "Cristal oscuro", 0x111C30, 178, 0x0A1322, 205, 34, 80, 0xEEF3FA, 0xA9B6CA, 0xFFFFFF, 34, 0x24324D, 0xD8E0EC, false },
	{ "Cristal claro",  0xFBFDFF, 190, 0xE6EDF6, 210, 150, 235, 0x0F1928, 0x4B5970, 0x0F1928, 36, 0xD9E2EE, 0x1F2B3D, true },
	{ "Aero",           0xBDEBFF, 200, 0x5BB9EB, 214, 200, 240, 0x062A42, 0x1C4F70, 0x062A42, 46, 0xE8F8FF, 0x0B3A5A, true },
	{ "Personalizado",  0x000000, 185, 0x000000, 208, 60, 120, 0xFFFFFF, 0xCCCCCC, 0xFFFFFF, 34, 0x000000, 0xFFFFFF, false },
};
static Theme T; static u32 ACC;   /* active theme + accent */
const char *ui_theme_name(int t) { return TH[t % UI_NTHEMES].name; }

static u32 rgb(int r, int g, int b) { r = r < 0 ? 0 : (r > 255 ? 255 : r); g = g < 0 ? 0 : (g > 255 ? 255 : g); b = b < 0 ? 0 : (b > 255 ? 255 : b); return (r << 16) | (g << 8) | b; }
static u32 scale(u32 c, float k) { return rgb((int)(((c >> 16) & 255) * k), (int)(((c >> 8) & 255) * k), (int)((c & 255) * k)); }
static u32 mixc(u32 a, u32 b, float t) { return rgb((int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t), (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t), (int)((a & 255) * (1 - t) + (b & 255) * t)); }
static float luma(u32 c) { return (0.299f * ((c >> 16) & 255) + 0.587f * ((c >> 8) & 255) + 0.114f * (c & 255)) / 255.f; }

static void apply_theme(const UiModel *m)
{
	T = TH[m->theme % UI_NTHEMES];
	u32 user = rgb(m->ar, m->ag, m->ab);
	if (m->theme == 3) {
		T.top = mixc(user, 0xFFFFFF, 0.12f); T.bot = scale(user, 0.72f);
		T.light = luma(user) > 0.55f;
		T.text = T.light ? 0x101820 : 0xF4F7FB; T.dim = T.light ? 0x3A4656 : 0xCBD3DE; T.line = T.text;
		T.btn_bg = T.light ? scale(user, 0.82f) : mixc(user, 0xFFFFFF, 0.22f); T.btn_fg = T.text;
		ACC = T.light ? scale(user, 0.45f) : mixc(user, 0xFFFFFF, 0.55f);
	} else {
		ACC = user;
		/* keep the accent readable on light themes */
		if (T.light && luma(ACC) > 0.6f) ACC = scale(ACC, 0.55f);
	}
	GFX_BTN_BG = T.btn_bg; GFX_BTN_FG = T.btn_fg;
}

/* ---------------- geometry ---------------- */
static float globe_R(const UiModel *m) { return fminf(W - PX - PW, H) * 0.43f * m->zoom; }

static float *pvx, *pvy, *pvz; static const Place *g_pl; static int g_np; static Sprite *dotspr;
struct Basis { float ex, ey, nx, ny, nz, cx, cy, cz; };
static Basis basis(const UiModel *m)
{
	const float d = 0.0174533f; float sl = sinf(m->lon * d), cl = cosf(m->lon * d), sp = sinf(m->lat * d), cp = cosf(m->lat * d);
	Basis b = { -sl, cl, -sp * cl, -sp * sl, cp, cp * cl, cp * sl, sp };
	return b;
}
bool ui_project(const UiModel *m, float lon, float lat, float *x, float *y)
{
	const float d = 0.0174533f; Basis b = basis(m);
	float px = cosf(lat * d) * cosf(lon * d), py = cosf(lat * d) * sinf(lon * d), pz = sinf(lat * d);
	if (px * b.cx + py * b.cy + pz * b.cz < 0.03f) return false;
	float R = globe_R(m);
	*x = VCX + R * (px * b.ex + py * b.ey); *y = VCY - R * (px * b.nx + py * b.ny + pz * b.nz);
	return true;
}
/* level of detail: only the biggest cities show when zoomed out; smaller ones appear as you zoom in */
static int *ord = NULL;                                   /* place indices, biggest first */
static const Place *sort_src = NULL;
static int cmp_ord(const void *a, const void *b) { return (int)sort_src[*(const int *)b].size - (int)sort_src[*(const int *)a].size; }
static int lod_min(float zoom) { return zoom < 1.5f ? 40 : zoom < 2.5f ? 25 : zoom < 4.f ? 12 : zoom < 7.f ? 6 : zoom < 12.f ? 3 : 1; }
#define MAX_DOTS 3000

int ui_pick(const UiModel *m)
{
	if (!g_pl || !m->nplaces) return -1;
	Basis b = basis(m); float R = globe_R(m), lim = 4.5f * U, best = 1e30f; int hit = -1;
	int mn = lod_min(m->zoom);
	for (int oi = 0; oi < g_np; oi++) {
		int i = ord[oi]; if (g_pl[i].size < mn) break;
		float depth = pvx[i] * b.cx + pvy[i] * b.cy + pvz[i] * b.cz; if (depth < 0.03f) continue;
		float dx = R * (pvx[i] * b.ex + pvy[i] * b.ey), dy = -R * (pvx[i] * b.nx + pvy[i] * b.ny + pvz[i] * b.nz);
		if (dx > lim || dx < -lim || dy > lim || dy < -lim) continue;
		float dd = dx * dx + dy * dy; if (dd > lim * lim) continue;
		float s = dd / (1.f + 0.15f * sqrtf((float)g_pl[i].size));
		if (s < best) { best = s; hit = i; }
	}
	return hit;
}

/* ---------------- atlas (256x256): white, disc, glow, ring, arc, atmosphere ---------------- */
#define AT 256
static float UVW[4], UVDISC[4], UVGLOW[4], UVRING[4], UVARC[4], UVATM[4];
static void uvr(float *o, int x, int y, int w, int h) { o[0] = (x + 0.5f) / AT; o[1] = (y + 0.5f) / AT; o[2] = (x + w - 0.5f) / AT; o[3] = (y + h - 0.5f) / AT; }
static void make_atlas()
{
	atlas = (u32 *)calloc(AT * AT, 4);
	for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) atlas[y * AT + x] = 0xFFFFFFFF;
	uvr(UVW, 3, 3, 2, 2);
	for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
		float dx = x + 0.5f - 32, dy = y + 0.5f - 32, r = sqrtf(dx * dx + dy * dy);
		auto put = [&](int ox, int oy, float a) { int v = (int)(a * 255.f + 0.5f); if (v < 0) v = 0; if (v > 255) v = 255; atlas[(oy + y) * AT + ox + x] = ((u32)v << 24) | ((u32)v << 16) | ((u32)v << 8) | (u32)v; };
		float disc = 30.5f - r; disc = disc < 0 ? 0 : (disc > 1 ? 1 : disc);
		put(64, 0, disc);
		float g = r < 31 ? expf(-r * r / (2 * 11.f * 11.f)) : 0; put(128, 0, g);
		float ring = 1.6f - fabsf(r - 28.f); ring = ring < 0 ? 0 : (ring > 1 ? 1 : ring); put(192, 0, ring);
		float ang = atan2f(dy, dx); float arc = (2.2f - fabsf(r - 27.f)); arc = arc < 0 ? 0 : (arc > 1 ? 1 : arc);
		float sweep = ang > -2.0f && ang < 0.0f ? 1.f : (ang >= 0.f && ang < 0.25f ? 1 - ang / 0.25f : 0.f);
		put(0, 64, arc * sweep);
	}
	for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) {
		float dx = (x + 0.5f - 64) / 64.f, dy = (y + 0.5f - 64) / 64.f, r = sqrtf(dx * dx + dy * dy);
		const float re = 1.f / 1.12f;
		float a = r < re ? 1.f : (r < 1.f ? powf(1.f - (r - re) / (1.f - re), 2.2f) : 0.f);
		int v = (int)(a * 255.f);
		atlas[(64 + y) * AT + 64 + x] = ((u32)v << 24) | ((u32)v << 16) | ((u32)v << 8) | (u32)v;
	}
	uvr(UVDISC, 64, 0, 64, 64); uvr(UVGLOW, 128, 0, 64, 64); uvr(UVRING, 192, 0, 64, 64); uvr(UVARC, 0, 64, 64, 64); uvr(UVATM, 64, 64, 128, 128);
	gpu_upload(TEX_ATLAS, atlas, AT, AT);
}
static unsigned hsh(unsigned x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
static void make_stars()
{
	stars = (u32 *)malloc((size_t)W * H * 4);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
		/* faint vertical gradient + sparse stars */
		u32 c = mixc(0x02040A, 0x060D1C, (float)y / H);
		unsigned h = hsh(x * 7919u + y * 104729u);
		if ((h & 2047) < 3) c = mixc(c, 0xDDE6FF, ((h >> 11) & 255) / 255.f);
		stars[y * W + x] = 0xFF000000u | c;
	}
	gpu_upload(TEX_STARS, stars, W, H);
#ifndef HOST_BUILD
	free(stars); stars = NULL;
#endif
}

void ui_set_places(const Place *p, int n)
{
	free(pvx); free(pvy); free(pvz);
	pvx = (float *)malloc(n * 4 + 4); pvy = (float *)malloc(n * 4 + 4); pvz = (float *)malloc(n * 4 + 4);
	const float d = 0.0174533f;
	for (int i = 0; i < n; i++) { float la = p[i].lat * d, lo = p[i].lon * d; pvx[i] = cosf(la) * cosf(lo); pvy[i] = cosf(la) * sinf(lo); pvz[i] = sinf(la); }
	g_pl = p; g_np = n;
	free(ord); ord = (int *)malloc(sizeof(int) * (n + 1));
	for (int i = 0; i < n; i++) ord[i] = i;
	sort_src = p; qsort(ord, n, sizeof(int), cmp_ord);
	free(dotspr); dotspr = (Sprite *)malloc(sizeof(Sprite) * (2 * MAX_DOTS + 2));
}

/* ---------------- widgets ---------------- */
struct Dyn { char kind; float x, y, s; int on; };
static Dyn dyn[24]; static int ndyn;

static void eq(int x, int y, int h, bool on) { if (ndyn < 24) { Dyn d = { 'e', (float)x, (float)y, (float)h, on }; dyn[ndyn++] = d; } }
static void spinner(float cx, float cy, float r) { if (ndyn < 24) { Dyn d = { 's', cx, cy, r, 0 }; dyn[ndyn++] = d; } }

static void glass(int x, int y, int w, int h, int r)
{
	/* soft shadow */
	for (int i = 3; i >= 1; i--) gfx_rrect(x - i + 2, y - i + 4, w + 2 * i - 4, h + 2 * i - 4, r + i, 0x000000, T.light ? 14 : 22);
	gfx_rrect_v(x, y, w, h, r, T.top, T.top_a, T.bot, T.bot_a);
	/* specular sheen on the upper part */
	int gh = (int)(h * (T.light ? 0.48f : 0.40f)); if (gh > 22 * U) gh = 22 * U;
	gfx_rrect_v(x + 1, y + 1, w - 2, gh, r - 1, 0xFFFFFF, T.gloss_a, 0xFFFFFF, 0);
	gfx_rrect_stroke(x, y, w, h, r, 1.3f, 0xFFFFFF, T.border_a);
}
static void sep(int x, int y, int w) { gfx_rect_a(x, y, w, 1, T.line, T.line_a); }

static int hint(int x, int cy, const char *b1, const char *b2, const char *label, bool measure = false)
{
	int bs = 3 * U;
	if (measure) { int w = (b1[1] ? gfx_text_w(F_SMALL, b1) + bs / 2 + 4 : bs) + (b2 ? U / 2 + gfx_text_w(F_SMALL, b2) + bs / 2 + 4 + U : U); return x + w + gfx_text_w(F_SMALL, label) + 3 * U; }
	x += gfx_button(b1, x, cy, bs) + (b2 ? U / 2 : U);
	if (b2) x += gfx_button(b2, x, cy, bs) + U;
	return x + gfx_text(F_SMALL, x, cy - gfx_line_h(F_SMALL) / 2, label, T.dim) + 3 * U;
}
static void badge(int cx, int cy, int n)
{
	char t[12]; snprintf(t, sizeof t, "%d", n);
	gfx_circle(cx, cy, 3.4f * U, ACC, 255);
	gfx_ring(cx, cy, 3.4f * U, 1.2f, 0xFFFFFF, 120);
	const FontDef *f = strlen(t) > 3 ? F_SMALL : F_BOLD;
	u32 tc = luma(ACC) > 0.6f ? 0x101820 : 0xFFFFFF;
	gfx_text(f, cx - gfx_text_w(f, t) / 2, cy - gfx_line_h(f) / 2, t, tc);
}

/* ---------------- panel layout ---------------- */
static int HDR_H;                     /* panel header height */
static int list_top(bool search) { return PY + HDR_H + (search ? 2 * U + gfx_line_h(F_BODY) + 2 * U : 0) + 2 * U + gfx_line_h(F_BOLD) + 2 * U; }
static int kb_height() { int kh = gfx_line_h(F_BOLD) + 2 * U; return 5 * (kh + U) + 2 * U; }
static int row_h(bool two) { return two ? gfx_line_h(F_BODY) + gfx_line_h(F_SMALL) + 2 * U : gfx_line_h(F_BODY) + 2 * U; }
static int rows_avail(bool search, bool two, bool kb)
{
	int bottom = PY + PH - 2 * U - (kb ? kb_height() : 0);
	int r = (bottom - list_top(search)) / row_h(two);
	return r < 1 ? 1 : r;
}
int ui_rows(const UiModel *m)
{
	switch (m->view) {
	case UV_COUNTRIES: return rows_avail(true, false, m->kb_open);
	case UV_CITIES: case UV_MENU: return rows_avail(false, false, false);
	default: return rows_avail(false, true, false);
	}
}

static void panel_header(const char *title, const char *sub, int count, bool has_badge)
{
	int tx = PX + 3 * U;
	if (has_badge) { badge(PX + 6 * U, PY + HDR_H / 2, count); tx = PX + 11 * U; }
	int maxw = PW - (tx - PX) - 3 * U;
	gfx_text_fit(F_TITLE, tx, PY + HDR_H / 2 - gfx_line_h(F_TITLE) + U / 2, title, maxw, T.text);
	gfx_text_fit(F_BODY, tx, PY + HDR_H / 2 + U / 2, sub, maxw, T.dim);
	sep(PX + 2 * U, PY + HDR_H, PW - 4 * U);
}

typedef void (*RowFn)(const UiModel *, int, int, int, int, bool);
static void panel_list(const UiModel *m, const char *section, int n, int sel, int top, RowFn row,
                       bool loading, const char *err, const char *empty, bool search, bool two)
{
	int x = PX + 3 * U, w = PW - 6 * U, y = PY + HDR_H + 2 * U;
	if (search) {
		int sh = gfx_line_h(F_BODY) + 2 * U;
		gfx_rrect(x, y, w, sh, sh / 2, T.light ? 0xFFFFFF : 0x000000, T.light ? 170 : 90);
		gfx_rrect_stroke(x, y, w, sh, sh / 2, 1.f, m->kb_open ? ACC : T.line, m->kb_open ? 255 : 60);
		gfx_ring(x + 2.5f * U, y + sh / 2.f - 1, U * 0.8f, 1.6f, T.dim, 255);
		gfx_line(x + 3.1f * U, y + sh / 2.f + U * 0.6f, x + 3.8f * U, y + sh / 2.f + U * 1.3f, 1.8f, T.dim, 255);
		bool has = m->query && m->query[0];
		int tw = gfx_text_fit(F_BODY, x + 5 * U, y + U, has ? m->query : "Buscar país...", w - 11 * U, has ? T.text : T.dim);
		if (m->kb_open) gfx_rect(x + 5 * U + (has ? tw : 0) + 2, y + U + 2, 2, gfx_line_h(F_BODY) - 4, ACC);
		gfx_button("S", x + w - 4 * U, y + sh / 2, 3 * U);
		y += sh + 2 * U;
	}
	gfx_text(F_BOLD, x, y, section, T.text);
	int ly = list_top(search), rh = row_h(two), rows = rows_avail(search, two, m->kb_open);
	sep(x, ly - U, w);
	if (loading) { spinner(PX + PW / 2.f, ly + 7 * U, 2.4f * U); const char *t = "Cargando..."; gfx_text(F_SMALL, PX + (PW - gfx_text_w(F_SMALL, t)) / 2, ly + 11 * U, t, T.dim); return; }
	if (err) { gfx_text_fit(F_BODY, x, ly + U, err, w, 0xFF5D6C); return; }
	if (n == 0) { gfx_text_fit(F_BODY, x, ly + U, empty, w, T.dim); return; }
	gfx_clip(PX, ly, PW, rows * rh);
	for (int i = 0; i < rows && top + i < n; i++) {
		int idx = top + i, ry = ly + i * rh;
		bool s = idx == sel && !m->kb_open;
		if (s) { gfx_rrect(PX + U, ry + 2, PW - 3 * U, rh - 4, U, ACC, T.light ? 70 : 85); gfx_rrect_stroke(PX + U, ry + 2, PW - 3 * U, rh - 4, U, 1.f, ACC, 200); }
		else if (i) sep(x, ry, w);
		row(m, idx, x, ry + U, w - 3 * U, s);
	}
	gfx_noclip();
	if (n > rows) {
		int th = rows * rh, shh = th * rows / n; if (shh < 3 * U) shh = 3 * U;
		int sy = ly + (th - shh) * top / (n - rows);
		gfx_rrect(PX + PW - U - U / 2, ly, U / 2, th, U / 4, T.line, 40);
		gfx_rrect(PX + PW - U - U / 2, sy, U / 2, shh, U / 4, ACC, 255);
	}
}

static void station_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Station &s = m->stations[i];
	bool playing = m->now_id && !strcmp(m->now_id, s.id) && m->pstate != P_IDLE;
	int ix = x;
	if (playing) { eq(x, y + 2, gfx_line_h(F_BODY) - 6, m->pstate == P_PLAYING); ix += 7 * U; }
	bool fav = m->is_fav && m->is_fav(s.id);
	gfx_text_fit(sel ? F_BOLD : F_BODY, ix, y, s.name, w - (ix - x) - 3 * U, playing ? ACC : T.text);
	if (fav) gfx_star(x + w + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.9f, 0xFF7FA8, 255);
	if (s.sub[0]) gfx_text_fit(F_SMALL, ix, y + gfx_line_h(F_BODY), s.sub, w - (ix - x), T.dim);
}
static void fav_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Fav &f = m->favs[i];
	bool playing = m->now_id && !strcmp(m->now_id, f.id) && m->pstate != P_IDLE;
	int ix = x;
	if (playing) { eq(x, y + 2, gfx_line_h(F_BODY) - 6, m->pstate == P_PLAYING); ix += 7 * U; }
	else { gfx_star(x + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.8f, 0xFF7FA8, 255); ix += 4 * U; }
	gfx_text_fit(sel ? F_BOLD : F_BODY, ix, y, f.name, w - (ix - x), playing ? ACC : T.text);
	gfx_text_fit(F_SMALL, ix, y + gfx_line_h(F_BODY), f.place, w - (ix - x), T.dim);
}
static void right_num(int x, int y, int w, int n)
{
	char t[16]; snprintf(t, sizeof t, "%d", n);
	int nw = gfx_text_w(F_SMALL, t);
	gfx_text(F_SMALL, x + w - nw + 2 * U, y + (gfx_line_h(F_BODY) - gfx_line_h(F_SMALL)) / 2, t, T.dim);
}
static void country_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Country *c = m->clist[i];
	gfx_text_fit(sel ? F_BOLD : F_BODY, x, y, c->name, w - 6 * U, T.text);
	right_num(x, y, w, c->nstations);
}
static void city_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Place &p = m->places[m->city_idx[i]];
	gfx_circle(x + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.55f, ACC, 255);
	gfx_text_fit(sel ? F_BOLD : F_BODY, x + 3 * U, y, p.title, w - 9 * U, T.text);
	right_num(x, y, w, p.size);
}
static const UiModel *g_menu_m;
static void menu_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	static const char *labels[UI_MENU_N] = { "Favoritos", "Buscar por país", "Tema", "Color: rojo", "Color: verde", "Color: azul", "Verificación TLS", "Ir a un lugar al azar" };
	const FontDef *f = sel ? F_BOLD : F_BODY;
	bool has_value = i == MENU_THEME || i == MENU_R || i == MENU_G || i == MENU_B || i == MENU_TLS;
	gfx_text_fit(f, x, y, labels[i], has_value ? w / 2 : w, T.text);
	int vx = x + w / 2, vw = w / 2;
	if (i == MENU_THEME) {
		const char *t = ui_theme_name(m->theme); int tw = gfx_text_w(F_BODY, t);
		gfx_text(F_BODY, x + w - tw - 2 * U, y, t, ACC);
		if (sel) { gfx_text(F_BODY, x + w - tw - 4 * U, y, "<", T.dim); gfx_text(F_BODY, x + w, y, ">", T.dim); }
	} else if (i == MENU_R || i == MENU_G || i == MENU_B) {
		int v = i == MENU_R ? m->ar : (i == MENU_G ? m->ag : m->ab);
		u32 c = i == MENU_R ? 0xFF4D5E : (i == MENU_G ? 0x3DDC84 : 0x4C8DFF);
		int bx = vx, bw = vw - 7 * U, cy = y + gfx_line_h(F_BODY) / 2;
		gfx_rrect(bx, cy - U / 3, bw, U * 2 / 3 + 1, U / 3, T.line, 50);
		gfx_rrect(bx, cy - U / 3, bw * v / 255, U * 2 / 3 + 1, U / 3, c, 255);
		gfx_circle(bx + bw * v / 255.f, cy, U * 0.9f, 0xFFFFFF, 255);
		right_num(x, y, w, v);
	} else if (i == MENU_TLS) {
		const char *t = m->tls ? "Sí" : "No"; int tw = gfx_text_w(F_BODY, t);
		gfx_text(F_BODY, x + w - tw + 2 * U, y, t, m->tls ? ACC : 0xFF5D6C);
	}
	(void)g_menu_m;
}

const char *const UI_KB_KEYS[] = { "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","Espacio","Borrar","Listo" };
const int UI_KB_N = 29, UI_KB_COLS = 7;
static void keyboard(const UiModel *m)
{
	int kw = (PW - 6 * U - (UI_KB_COLS - 1) * U) / UI_KB_COLS, kh = gfx_line_h(F_BOLD) + 2 * U;
	int y0 = PY + PH - kb_height(), x0 = PX + 3 * U;
	sep(x0, y0, PW - 6 * U);
	for (int k = 0; k < UI_KB_N; k++) {
		int r, c, w = kw, x;
		if (k < 26) { r = k / UI_KB_COLS; c = k % UI_KB_COLS; x = x0 + c * (kw + U); }
		else { r = 4; w = (PW - 6 * U - 2 * U) / 3; x = x0 + (k - 26) * (w + U); }
		int y = y0 + 2 * U + r * (kh + U);
		bool s = k == m->kb_sel;
		if (s) gfx_rrect(x, y, w, kh, U, ACC, 255);
		else { gfx_rrect(x, y, w, kh, U, T.light ? 0xFFFFFF : 0x000000, T.light ? 150 : 70); gfx_rrect_stroke(x, y, w, kh, U, 1.f, 0xFFFFFF, T.light ? 160 : 40); }
		const FontDef *f = k < 26 ? F_BOLD : F_SMALL;
		u32 tc = s ? (luma(ACC) > 0.6f ? 0x101820 : 0xFFFFFF) : T.text;
		gfx_text(f, x + (w - gfx_text_w(f, UI_KB_KEYS[k])) / 2, y + (kh - gfx_line_h(f)) / 2, UI_KB_KEYS[k], tc);
	}
}

static void nearby(const UiModel *m, int y, int bottom)
{
	int x = PX + 3 * U, w = PW - 6 * U;
	if (m->nplaces <= 0) return;
	const int K = 10; int idx[K]; float dist[K]; int nk = 0;
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
	int maxn = (bottom - y - gfx_line_h(F_BOLD) - 2 * U) / rh; if (maxn > nk) maxn = nk;
	if (maxn <= 0) return;
	gfx_text(F_BOLD, x, y, "Cerca de aquí", T.text);
	y += gfx_line_h(F_BOLD) + U; sep(x, y, w); y += U;
	for (int k = 0; k < maxn; k++) {
		const Place &q = m->places[idx[k]];
		gfx_circle(x + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.55f, ACC, 255);
		gfx_text_fit(F_BODY, x + 3 * U, y, q.title, w - 9 * U, T.text);
		right_num(x, y, w - 2 * U, q.size);
		y += rh;
	}
}

static void panel(const UiModel *m)
{
	glass(PX, PY, PW, PH, 2 * U + U / 2);
	gfx_clip(PX, PY, PW, PH);
	int x = PX + 3 * U, w = PW - 6 * U;
	switch (m->view) {
	case UV_STATIONS: if (m->panel_place) {
		const Place *p = m->panel_place;
		panel_header(p->title, p->country, m->loading == 2 ? p->size : m->nst, true);
		char sec[80]; snprintf(sec, sizeof sec, "Emisoras en %s", p->title);
		panel_list(m, sec, m->nst, m->st_sel, m->st_top, station_row, m->loading == 2, m->loading ? NULL : m->error, "Sin emisoras", false, true);
	} break;
	case UV_FAVS: {
		char sub[48]; snprintf(sub, sizeof sub, "%d %s", m->nfav, m->nfav == 1 ? "emisora guardada" : "emisoras guardadas");
		panel_header("Favoritos", sub, 0, false);
		panel_list(m, "Tus emisoras", m->nfav, m->fav_sel, m->fav_top, fav_row, false, NULL, "Aún no hay favoritos. Pulsa Cuadrado en una emisora.", false, true);
	} break;
	case UV_COUNTRIES: {
		char sub[48]; snprintf(sub, sizeof sub, "%d países", m->nclist);
		panel_header("Buscar por país", sub, 0, false);
		panel_list(m, m->query && m->query[0] ? "Resultados" : "Todos los países", m->nclist, m->c_sel, m->c_top, country_row, m->loading == 1, NULL, "Ningún país coincide", true, false);
		if (m->kb_open) keyboard(m);
	} break;
	case UV_CITIES: if (m->cur_country) {
		char sub[64]; snprintf(sub, sizeof sub, "%d ciudades  \xC2\xB7  %d emisoras", m->cur_country->nplaces, m->cur_country->nstations);
		panel_header(m->cur_country->name, sub, 0, false);
		panel_list(m, "Ciudades", m->ncity, m->city_sel, m->city_top, city_row, false, NULL, "Sin ciudades", false, false);
	} break;
	case UV_MENU: {
		panel_header("Menú", "Ajustes y accesos", 0, false);
		g_menu_m = m;
		panel_list(m, "Opciones", UI_MENU_N, m->menu_sel, 0, menu_row, false, NULL, "", false, false);
	} break;
	default: {
		int bottom = PY + PH - 2 * U, y;
		if (m->hover >= 0) {
			const Place &p = m->places[m->hover];
			panel_header(p.title, p.country, p.size, true);
			y = PY + HDR_H + 3 * U;
			char t[64]; snprintf(t, sizeof t, "%d %s en esta ciudad", p.size, p.size == 1 ? "emisora" : "emisoras");
			gfx_text_fit(F_BODY, x, y, t, w, T.text); y += gfx_line_h(F_BODY) + 3 * U;
			hint(x, y, "X", NULL, "Ver emisoras"); y += 4 * U;
		} else {
			panel_header("Explorar", "Gira el globo y apunta a una ciudad", 0, false);
			y = PY + HDR_H + 4 * U;
			const char *tips[][3] = { { "L1", "R1", "Acercar y alejar" }, { "Select", NULL, "Buscar por país" }, { "Start", NULL, "Menú y temas" } };
			for (int i = 0; i < 3; i++) { hint(x, y, tips[i][0], tips[i][1], tips[i][2]); y += 5 * U; }
		}
		int status_h = (m->loading == 1 || (!m->loading && m->error)) ? 12 * U : 0;
		nearby(m, y + U, bottom - status_h);
		if (m->loading == 1) { int sy = bottom - 9 * U; sep(x, sy - U, w); spinner(x + 2.5f * U, sy + 3 * U, 2.f * U); gfx_text_fit(F_BODY, x + 7 * U, sy + 3 * U - gfx_line_h(F_BODY) / 2, "Cargando ciudades...", w - 7 * U, T.text); }
		if (!m->loading && m->error) {
			int sy = bottom - 11 * U; sep(x, sy - U, w);
			gfx_text_fit(F_BOLD, x, sy, "No se pudo cargar el mapa", w, 0xFF5D6C);
			gfx_text_fit(F_SMALL, x, sy + gfx_line_h(F_BOLD), m->error, w, T.dim);
			hint(x, sy + gfx_line_h(F_BOLD) + gfx_line_h(F_SMALL) + 2 * U, "X", NULL, "Reintentar");
		}
	} break;
	}
	gfx_noclip();
}

static void player_card(const UiModel *m)
{
	glass(PX, CY, PW, CH, 2 * U + U / 2);
	gfx_clip(PX, CY, PW, CH);
	int x = PX + 3 * U, w = PW - 6 * U, y = CY + 2 * U, st = m->pstate, ic = 5 * U;
	if (st == P_PLAYING) eq(x + w - ic, y + U / 2, ic - U, true);
	else if (st == P_BUFFERING || st == P_CONNECTING) spinner(x + w - ic / 2.f, y + ic / 2.f, ic * 0.32f);
	else if (st == P_ERROR) gfx_circle(x + w - ic / 2.f, y + ic / 2.f, ic * 0.3f, 0xFF5D6C, 255);
	int tw = w - ic - 2 * U;
	if (st == P_IDLE) {
		gfx_text_fit(F_BOLD, x, y, "Nada sonando", tw, T.text);
		gfx_text_fit(F_SMALL, x, y + gfx_line_h(F_BOLD), "Elige una ciudad y pulsa X", tw, T.dim);
	} else {
		int nw = gfx_text_fit(F_BOLD, x, y, m->now_name ? m->now_name : "", tw - 3 * U, ACC);
		if (m->is_fav && m->now_id && m->is_fav(m->now_id)) gfx_star(x + nw + 2 * U, y + gfx_line_h(F_BOLD) / 2.f, U * 0.9f, 0xFF7FA8, 255);
		char l2[200];
		if (st == P_PLAYING) snprintf(l2, sizeof l2, "%s  \xC2\xB7  %s", m->now_place ? m->now_place : "", m->pfmt && m->pfmt[0] ? m->pfmt : "EN VIVO");
		else snprintf(l2, sizeof l2, "%s  \xC2\xB7  %s", m->now_place ? m->now_place : "", m->pmsg ? m->pmsg : "");
		gfx_text_fit(F_SMALL, x, y + gfx_line_h(F_BOLD), l2, tw, st == P_ERROR ? 0xFF5D6C : T.dim);
	}
	int cy = CY + CH - 4 * U;
	int bx = hint(x, cy, "T", NULL, "Parar");
	bx += gfx_button("L2", bx, cy, 3 * U) + U;
	int ew = gfx_text_w(F_SMALL, "R2") + 3 * U / 2 + 4, ex = PX + PW - 3 * U - ew;
	int bw = ex - U - bx;
	if (bw > 4 * U) {
		gfx_rrect(bx, cy - U / 4, bw, U / 2 + 1, U / 4, T.line, 50);
		gfx_rrect(bx, cy - U / 4, (int)(bw * m->volume), U / 2 + 1, U / 4, ACC, 255);
		gfx_circle(bx + bw * m->volume, cy, U * 0.9f, 0xFFFFFF, 255);
	}
	gfx_button("R2", ex, cy, 3 * U);
	gfx_noclip();
}

static void hint_bar(const UiModel *m)
{
	struct HintItem { const char *a, *b, *l; };
	HintItem hs[6]; int n = 0;
	switch (m->view) {
	case UV_MAP:       hs[n++] = { "X", 0, "Abrir" }; hs[n++] = { "L1", "R1", "Zoom" }; hs[n++] = { "Select", 0, "Países" }; hs[n++] = { "Start", 0, "Menú" }; hs[n++] = { "L3", 0, "Azar" }; break;
	case UV_STATIONS:  hs[n++] = { "X", 0, "Reproducir" }; hs[n++] = { "S", 0, "Favorito" }; hs[n++] = { "O", 0, "Volver" }; hs[n++] = { "L1", "R1", "Página" }; break;
	case UV_COUNTRIES: if (m->kb_open) { hs[n++] = { "X", 0, "Escribir" }; hs[n++] = { "S", 0, "Borrar" }; hs[n++] = { "O", 0, "Cerrar" }; }
	                   else { hs[n++] = { "X", 0, "Ciudades" }; hs[n++] = { "S", 0, "Teclado" }; hs[n++] = { "O", 0, "Volver" }; hs[n++] = { "L1", "R1", "Página" }; } break;
	case UV_CITIES:    hs[n++] = { "X", 0, "Emisoras" }; hs[n++] = { "O", 0, "Países" }; hs[n++] = { "L1", "R1", "Página" }; break;
	case UV_MENU:      hs[n++] = { "X", 0, "Elegir" }; hs[n++] = { "< >", 0, "Cambiar" }; hs[n++] = { "O", 0, "Cerrar" }; break;
	default:           hs[n++] = { "X", 0, "Reproducir" }; hs[n++] = { "S", 0, "Quitar" }; hs[n++] = { "O", 0, "Volver" }; break;
	}
	int avail = W - (PX + PW + 4 * U) - 3 * U - 4 * U;
	/* drop trailing hints that do not fit */
	while (n > 0) { int ww = 0; for (int i = 0; i < n; i++) ww = hint(ww, 0, hs[i].a, hs[i].b, hs[i].l, true); if (ww <= avail) break; n--; }
	if (!n) return;
	int ww = 0; for (int i = 0; i < n; i++) ww = hint(ww, 0, hs[i].a, hs[i].b, hs[i].l, true);
	int bh = 5 * U, cx = (int)VCX, x = cx - ww / 2, y = H - 3 * U - bh;
	glass(x - 2 * U, y, ww + U, bh, bh / 2);
	for (int i = 0; i < n; i++) x = hint(x, y + bh / 2, hs[i].a, hs[i].b, hs[i].l);
}

static void build_layer(const UiModel *m)
{
	apply_theme(m);
	memset(layer, 0, (size_t)W * H * 4);
	gfx_set_target(layer);
	ndyn = 0;
	panel(m); player_card(m); hint_bar(m);
	gfx_set_target(NULL);
	gpu_upload(TEX_UI, layer, W, H);
}

/* HUD chip (coordinates, zoom, fps, TLS) -> small texture, re-rendered only when the text changes */
static char hud_last[128]; static int hud_w;
static void build_hud(const UiModel *m)
{
	int la = (int)(fabsf(m->lat) * 100 + 0.5f), lo = (int)(fabsf(m->lon) * 100 + 0.5f), zm = (int)(m->zoom * 10 + 0.5f);
	char t[128]; snprintf(t, sizeof t, "%d.%02d\xC2\xB0 %c   %d.%02d\xC2\xB0 %c   x%d.%d   %d fps%s", la / 100, la % 100, m->lat >= 0 ? 'N' : 'S', lo / 100, lo % 100, m->lon >= 0 ? 'E' : 'O', zm / 10, zm % 10, (int)(m->fps + 0.5f), m->tls ? "" : "   TLS OFF");
	if (!strcmp(t, hud_last) && !m->ui_dirty) return;
	strcpy(hud_last, t);
	memset(hudbuf, 0, (size_t)HUD_W * HUD_H * 4);
	gfx_set_target(hudbuf); gfx_target_size(HUD_W, HUD_H);
	int tw = gfx_text_w(F_SMALL, t) + 4 * U; if (tw > HUD_W - 8) tw = HUD_W - 8;
	hud_w = tw;
	int x = HUD_W - tw - 4, h = gfx_line_h(F_SMALL) + 2 * U;
	glass(x, 2, tw, h, h / 2);
	gfx_text(F_SMALL, x + 2 * U, 2 + U, t, m->tls ? T.dim : 0xFF5D6C);
	gfx_target_size(W, H); gfx_set_target(NULL);
	gpu_upload(TEX_HUD, hudbuf, HUD_W, HUD_H);
}
static char toast_last[128];
static void build_toast(const UiModel *m)
{
	if (!m->toast || (!strcmp(m->toast, toast_last) && !m->ui_dirty)) return;
	snprintf(toast_last, sizeof toast_last, "%s", m->toast);
	memset(toastbuf, 0, (size_t)TOAST_W * TOAST_H * 4);
	gfx_set_target(toastbuf); gfx_target_size(TOAST_W, TOAST_H);
	int tw = gfx_text_w(F_BODY, m->toast) + 6 * U; if (tw > TOAST_W - 8) tw = TOAST_W - 8;
	int h = gfx_line_h(F_BODY) + 2 * U, x = (TOAST_W - tw) / 2;
	glass(x, 4, tw, h, h / 2);
	gfx_text_fit(F_BODY, x + 3 * U, 4 + U, m->toast, tw - 6 * U, T.text);
	gfx_target_size(W, H); gfx_set_target(NULL);
	gpu_upload(TEX_TOAST, toastbuf, TOAST_W, TOAST_H);
}

/* ---------------- sprites ---------------- */
static Sprite fg[160]; static int nfg;
static void spr(float x, float y, float w, float h, const float *uv, u32 c, float a, float rot = 0, int tex = TEX_ATLAS)
{
	if (nfg >= 160) return;
	Sprite &s = fg[nfg++];
	s.x = x; s.y = y; s.w = w; s.h = h;
	if (uv) { s.u0 = uv[0]; s.v0 = uv[1]; s.u1 = uv[2]; s.v1 = uv[3]; } else { s.u0 = 0; s.v0 = 0; s.u1 = 1; s.v1 = 1; }
	s.r = ((c >> 16) & 255) / 255.f * a; s.g = ((c >> 8) & 255) / 255.f * a; s.b = (c & 255) / 255.f * a; s.a = a;
	s.rot = rot; s.tex = tex;
}

void ui_init(unsigned w, unsigned h)
{
	W = w; H = h; U = h >= 900 ? 12 : 8;
	gfx_init(w, h);
	layer = (u32 *)malloc((size_t)w * h * 4);
	PX = 3 * U; PY = 3 * U; PW = W * 30 / 100; if (PW < 54 * U) PW = 54 * U;
	CH = 17 * U; CY = H - CH - 3 * U; PH = CY - 2 * U - PY;
	HDR_H = 14 * U;
	VCX = PX + PW + (W - PX - PW) / 2.f; VCY = H / 2.f;
	HUD_W = W / 2; HUD_H = gfx_line_h(F_SMALL) + 2 * U + 8; hudbuf = (u32 *)malloc((size_t)HUD_W * HUD_H * 4);
	TOAST_W = W / 2; TOAST_H = gfx_line_h(F_BODY) + 2 * U + 10; toastbuf = (u32 *)malloc((size_t)TOAST_W * TOAST_H * 4);
	make_atlas(); make_stars();
}

void ui_frame(UiModel *m)
{
	RG_STAGE(10); if (m->ui_dirty) build_layer(m);
	RG_STAGE(11); build_hud(m);
	RG_STAGE(12); build_toast(m);
	m->ui_dirty = false;

	float R = globe_R(m);
	Sprite bg[2]; int nbg = 0;
	{ Sprite &s = bg[nbg++]; s.x = 0; s.y = 0; s.w = W; s.h = H; s.u0 = 0; s.v0 = 0; s.u1 = 1; s.v1 = 1; s.r = s.g = s.b = s.a = 1; s.rot = 0; s.tex = TEX_STARS; }
	{ Sprite &s = bg[nbg++]; float hr = R * 1.12f; s.x = VCX - hr; s.y = VCY - hr; s.w = s.h = 2 * hr; s.u0 = UVATM[0]; s.v0 = UVATM[1]; s.u1 = UVATM[2]; s.v1 = UVATM[3];
	  float a = 0.75f; s.r = 0.353f * a; s.g = 0.659f * a; s.b = 1.f * a; s.a = a * 0.0f; s.rot = 0; s.tex = TEX_ATLAS; }   /* additive glow (alpha 0) */

	nfg = 0;
	/* crosshair + hover marker under the panels */
	if (m->view == UV_MAP) {
		bool hot = m->hover >= 0;
		float pulse = 0.5f + 0.5f * sinf(m->frame * 0.08f);
		float r = 5.5f * U + (hot ? pulse * U * 0.6f : 0);
		spr(VCX - r * 32 / 28.f, VCY - r * 32 / 28.f, r * 64 / 28.f, r * 64 / 28.f, UVRING, hot ? ACC : 0xFFFFFF, hot ? 0.95f : 0.6f);
		spr(VCX - 3, VCY - 3, 6, 6, UVDISC, 0xFFFFFF, 0.9f);
		float x, y;
		if (hot && ui_project(m, m->places[m->hover].lon, m->places[m->hover].lat, &x, &y)) {
			spr(x - U, y - U, 2 * U, 2 * U, UVDISC, ACC, 1.f);
			float rr = U * 1.6f; spr(x - rr * 32 / 28.f, y - rr * 32 / 28.f, rr * 64 / 28.f, rr * 64 / 28.f, UVRING, ACC, 0.8f);
		}
	}
	spr(0, 0, W, H, NULL, 0xFFFFFF, 1.f, 0, TEX_UI);
	/* animated widgets from the last layer build */
	for (int i = 0; i < ndyn; i++) {
		const Dyn &d = dyn[i];
		if (d.kind == 'e') {
			for (int k = 0; k < 4; k++) {
				float v = d.on ? 0.35f + 0.65f * fabsf(sinf(m->frame * (0.11f + k * 0.037f) + k * 1.3f)) : 0.25f;
				float bh = d.s * v;
				spr(d.x + k * (U + U / 2), d.y + d.s - bh, U, bh, UVW, ACC, 1.f);
			}
		} else {
			float r = d.s * 64 / 54.f;
			spr(d.x - r, d.y - r, 2 * r, 2 * r, UVRING, T.dim, 0.35f);
			spr(d.x - r, d.y - r, 2 * r, 2 * r, UVARC, ACC, 1.f, m->frame * 0.15f);
		}
	}
	spr(W - HUD_W - 3 * U + 4, 3 * U - 2, HUD_W, HUD_H, NULL, 0xFFFFFF, 1.f, 0, TEX_HUD);
	if (m->toast && m->toast_t > 0) {
		float a = m->toast_t > 20 ? 1.f : m->toast_t / 20.f;
		spr(VCX - TOAST_W / 2.f, 3 * U + HUD_H + U, TOAST_W, TOAST_H, NULL, 0xFFFFFF, a, 0, TEX_TOAST);
	}

	RG_STAGE(13);
	/* station dots: projected on the PPU (cheap with precomputed unit vectors), drawn as GPU sprites */
	int nd = 0;
	if (g_pl && dotspr && m->nplaces) {
		Basis b = basis(m);
		float ds = (m->zoom < 2 ? 0.9f : (m->zoom < 8 ? 1.25f : 1.8f)) * (U / 8.f);
		int mn = lod_min(m->zoom), shown = 0;
		for (int pass = 0; pass < 2; pass++) for (int oi = 0; oi < g_np; oi++) {
			int i = ord[oi], sz = g_pl[i].size;
			if (sz < mn) break;                       /* sorted: everything after is smaller */
			if (pass == 0 && sz < 12) break;
			if (pass == 1 && shown >= MAX_DOTS) break;
			float depth = pvx[i] * b.cx + pvy[i] * b.cy + pvz[i] * b.cz; if (depth < 0.03f) continue;
			float x = VCX + R * (pvx[i] * b.ex + pvy[i] * b.ey), y = VCY - R * (pvx[i] * b.nx + pvy[i] * b.ny + pvz[i] * b.nz);
			if (x < -12 || y < -12 || x > W + 12 || y > H + 12) continue;
			float t = sz >= 40 ? 1.f : sz / 40.f, rad = 0.9f + 1.4f * sqrtf(t);
			float hs = (pass == 0 ? rad * 2.4f + 1.f : rad + 1.f) * ds;
			const float *uv = pass == 0 ? UVGLOW : UVDISC;
			float a = pass == 0 ? 0.16f : 0.65f + 0.35f * t;
			if (pass == 1) shown++; else if (nd >= MAX_DOTS) break;
			Sprite &q = dotspr[nd++];
			q.x = x - hs; q.y = y - hs; q.w = q.h = 2 * hs;
			q.u0 = uv[0]; q.v0 = uv[1]; q.u1 = uv[2]; q.v1 = uv[3];
			q.r = (0.18f + 0.43f * t) * a; q.g = (0.83f + 0.17f * t) * a; q.b = (0.60f + 0.24f * t) * a; q.a = a;
			q.rot = 0; q.tex = TEX_ATLAS;
		}
	}
	GlobeView g; g.lon = m->lon; g.lat = m->lat; g.R = R; g.cx = VCX; g.cy = VCY; g.show_globe = true; g.dot_scale = 1;
	RG_STAGE(14);
	gpu_frame(g, bg, nbg, dotspr, nd, fg, nfg);
	RG_STAGE(15);
}
