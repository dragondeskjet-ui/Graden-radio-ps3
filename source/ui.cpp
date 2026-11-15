#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ui.h"
#include "gfx.h"
#include "player.h"
#include "landmask.h"

/* palette */
#define OCEAN_T   0x07101D
#define OCEAN_B   0x0B1830
#define LAND      0x16263A
#define COAST     0x2B4565
#define GRID      0x2A3B55
#define DOT_LO    0x2FA37E
#define DOT_HI    0x7CF0C2
#define ACCENT    0xFFC24B
#define TEXT      0xEEF3FA
#define DIM       0x91A0B8
#define FAINT     0x5A6A84
#define PANEL     0x0C1624
#define CARD      0x13203A
#define SEL       0x1D3A66
#define ERR       0xFF6B6B
#define FAV       0xFF8FB1

static int W, H, U;               /* U = layout unit (8 px at 720p) */
static unsigned char *land = NULL; /* expanded mask */
static u32 *maplayer = NULL;

void ui_init(unsigned w, unsigned h)
{
	W = w; H = h; U = h >= 900 ? 12 : 8;
	gfx_init(w, h);
	maplayer = (u32 *)malloc((size_t)w * h * 4);
	land = (unsigned char *)malloc(LAND_W * LAND_H);
	for (int y = 0; y < LAND_H; y++) {
		unsigned char *row = land + y * LAND_W; int x = 0, v = 0;
		for (unsigned r = land_row[y]; r < land_row[y + 1]; r++) { int n = land_runs[r]; for (int i = 0; i < n && x < LAND_W; i++) row[x++] = v; v ^= 1; }
		while (x < LAND_W) row[x++] = 0;
	}
}

static int bar_h() { return 11 * U; }
int ui_list_rows() { int top = 16 * U, bottom = H - bar_h() - 2 * U; return (bottom - top) / (gfx_line_h(F_BODY) + gfx_line_h(F_SMALL) + 2 * U); }
static float pxdeg(const UiModel *m) { return (float)W / 360.f * m->zoom; }
static inline u32 lerpc(u32 a, u32 b, float t)
{
	int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
	int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
	int r = ar + (int)((br - ar) * t), g = ag + (int)((bg - ag) * t), c = ab + (int)((bb - ab) * t);
	return ((u32)r << 16) | ((u32)g << 8) | (u32)c;
}
static inline int landat(int col, int row) { col %= LAND_W; if (col < 0) col += LAND_W; if (row < 0 || row >= LAND_H) return 0; return land[row * LAND_W + col]; }

static void draw_map_layer(const UiModel *m)
{
	gfx_set_target(maplayer);
	float sc = pxdeg(m);
	static int colmap[4096]; static float colfrac[4096];
	for (int x = 0; x < W && x < 4096; x++) {
		float lon = m->lon + (x - W / 2) / sc; float c = (lon + 180.f) / 360.f * LAND_W - 0.5f;
		colmap[x] = (int)floorf(c); colfrac[x] = c - colmap[x];
	}
	for (int y = 0; y < H; y++) {
		float lat = m->lat + (H / 2 - y) / sc;
		u32 ocean = lerpc(OCEAN_T, OCEAN_B, (float)y / H);
		u32 *row = maplayer + y * W;
		if (lat > 90 || lat < -90) { for (int x = 0; x < W; x++) row[x] = OCEAN_T; continue; }
		float rf = (90.f - lat) / 180.f * LAND_H - 0.5f; int r = (int)floorf(rf); float fy = rf - r;
		/* below ~1 cell per pixel use nearest; above, bilinear contour with soft coast line */
		bool smooth = sc / 4.f > 1.f;
		for (int x = 0; x < W; x++) {
			int c = colmap[x];
			if (!smooth) {
				int l = landat(c, r);
				row[x] = l ? ((!landat(c - 1, r) || !landat(c + 1, r) || !landat(c, r - 1) || !landat(c, r + 1)) ? COAST : LAND) : ocean;
				continue;
			}
			float fx = colfrac[x];
			float v = (landat(c, r) * (1 - fx) + landat(c + 1, r) * fx) * (1 - fy) + (landat(c, r + 1) * (1 - fx) + landat(c + 1, r + 1) * fx) * fy;
			float e = fabsf(v - 0.5f);                    /* 0 at contour, 0.5 far */
			float aa = 4.f / sc * 1.5f;                     /* ~1.5 px expressed in mask units */
			if (aa > 0.5f) aa = 0.5f;
			u32 base = v >= 0.5f ? LAND : ocean;
			if (e < aa) base = lerpc(COAST, base, e / aa);
			row[x] = base;
		}
	}
	/* graticule */
	int step = m->zoom < 3 ? 30 : (m->zoom < 10 ? 10 : 5);
	for (int lon = -180; lon <= 180; lon += step) { int x = (int)(W / 2 + (lon - m->lon) * sc); gfx_rect_a(x, 0, 1, H, GRID, 70); }
	for (int lat = -90; lat <= 90; lat += step) { int y = (int)(H / 2 - (lat - m->lat) * sc); gfx_rect_a(0, y, W, 1, GRID, 70); }
	/* stations as dots, size by count */
	float zr = m->zoom < 2 ? 1.f : (m->zoom < 8 ? 1.4f : 2.f);
	for (int i = 0; i < m->nplaces; i++) {
		const Place &p = m->places[i];
		float x = W / 2 + (p.lon - m->lon) * sc, y = H / 2 - (p.lat - m->lat) * sc;
		if (x < -360 * sc + W && x < -4) x += 360 * sc;
		if (x < -4 || y < -4 || x > W + 4 || y > H + 4) continue;
		float t = p.size >= 40 ? 1.f : p.size / 40.f;
		float r = (1.1f + 1.6f * sqrtf(t)) * zr * (U / 8.f);
		gfx_circle(x, y, r, lerpc(DOT_LO, DOT_HI, t), 150 + (int)(105 * t));
	}
	gfx_set_target(NULL);
}

/* ---------- widgets ---------- */
static void hints(int x, int cy, const char *const *items, int n)
{
	int bs = 3 * U;
	for (int i = 0; i < n; i += 2) {
		x += gfx_button(items[i], x, cy, bs) + U;
		x += gfx_text(F_SMALL, x, cy - gfx_line_h(F_SMALL) / 2, items[i + 1], DIM) + 3 * U;
	}
}
static int hints_w(const char *const *items, int n)
{
	int w = 0, bs = 3 * U;
	for (int i = 0; i < n; i += 2) w += (items[i][1] ? gfx_text_w(F_SMALL, items[i]) + bs / 2 + 4 : bs) + U + gfx_text_w(F_SMALL, items[i + 1]) + 3 * U;
	return w;
}
static void spinner(float cx, float cy, float r, unsigned f)
{
	gfx_ring(cx, cy, r, U * 0.5f, 0x22324D, 255);
	float a0 = f * 0.15f; a0 = fmodf(a0, 6.2832f);
	gfx_arc(cx, cy, r, U * 0.5f, a0, a0 + 1.9f, ACCENT, 255);
}
static void eq(int x, int y, int h, unsigned f, bool on)
{
	for (int i = 0; i < 4; i++) {
		float v = on ? 0.35f + 0.65f * fabsf(sinf(f * (0.11f + i * 0.037f) + i * 1.3f)) : 0.25f;
		int bh = (int)(h * v);
		gfx_rrect(x + i * (U + U / 2), y + h - bh, U, bh, U / 2, ACCENT, 255);
	}
}

static void draw_bar(const UiModel *m)
{
	int bh = bar_h(), y = H - bh;
	gfx_rect_a(0, y, W, bh, PANEL, 238);
	gfx_rect_a(0, y, W, 1, 0x24344F, 255);
	int ic = 6 * U, ix = 3 * U, iy = y + U + U / 2;
	gfx_rrect(ix, iy, ic, ic, U, CARD, 255);
	int st = m->pstate;
	if (st == P_PLAYING) eq(ix + U + U / 2, iy + U + U / 2, ic - 3 * U, m->frame, true);
	else if (st == P_BUFFERING || st == P_CONNECTING) spinner(ix + ic / 2.f, iy + ic / 2.f, ic * 0.28f, m->frame);
	else if (st == P_ERROR) { gfx_circle(ix + ic / 2.f, iy + ic / 2.f, ic * 0.3f, ERR, 255); gfx_text(F_BOLD, ix + ic / 2 - gfx_text_w(F_BOLD, "!") / 2, iy + ic / 2 - gfx_line_h(F_BOLD) / 2, "!", PANEL); }
	else eq(ix + U + U / 2, iy + U + U / 2, ic - 3 * U, 0, false);

	int tx = ix + ic + 2 * U, right_w = 34 * U, maxw = W - tx - right_w - 3 * U;
	char l2[200];
	if (st == P_IDLE) {
		gfx_text(F_BOLD, tx, iy, "Nada sonando", TEXT);
		snprintf(l2, sizeof l2, "Mueve el mapa, elige una ciudad y pulsa X");
		gfx_text_fit(F_SMALL, tx, iy + gfx_line_h(F_BOLD) + 2, l2, maxw, DIM);
	} else {
		int nw = gfx_text_fit(F_BOLD, tx, iy, m->now_name ? m->now_name : "", maxw - 4 * U, TEXT);
		if (m->is_fav && m->now_id && m->is_fav(m->now_id)) gfx_star(tx + nw + 2 * U, iy + gfx_line_h(F_BOLD) / 2.f, U * 0.9f, FAV, 255);
		if (st == P_PLAYING) snprintf(l2, sizeof l2, "%s  \xC2\xB7  %s", m->now_place ? m->now_place : "", m->pfmt && m->pfmt[0] ? m->pfmt : "EN VIVO");
		else snprintf(l2, sizeof l2, "%s  \xC2\xB7  %s", m->now_place ? m->now_place : "", m->pmsg ? m->pmsg : "");
		gfx_text_fit(F_SMALL, tx, iy + gfx_line_h(F_BOLD) + 2, l2, maxw, st == P_ERROR ? ERR : DIM);
	}
	/* volume */
	int vx = W - right_w, vy = iy + U;
	gfx_tri(vx, vy + U, vx + U, vy + U, vx + U, vy + 2 * U, DIM, 255);
	gfx_rect(vx, vy + U, U, U, DIM);
	gfx_tri(vx + U, vy + U, vx + 2 * U, vy, vx + 2 * U, vy + 3 * U, DIM, 255);
	gfx_tri(vx + U, vy + U, vx + 2 * U, vy + 3 * U, vx + U, vy + 2 * U, DIM, 255);
	int bx = vx + 3 * U, bw = 16 * U;
	gfx_rrect(bx, vy + U + U / 4, bw, U / 2 + 1, U / 4, 0x26354F, 255);
	gfx_rrect(bx, vy + U + U / 4, (int)(bw * m->volume), U / 2 + 1, U / 4, ACCENT, 255);
	char vt[16]; snprintf(vt, sizeof vt, "%d%%", (int)(m->volume * 100 + 0.5f));
	gfx_text(F_SMALL, bx + bw + U, vy + U * 1.5f - gfx_line_h(F_SMALL) / 2, vt, DIM);
	if (!m->tls) { int cw = gfx_text_w(F_SMALL, "TLS OFF") + 2 * U; gfx_rrect(vx, vy + 3 * U + U / 2, cw, gfx_line_h(F_SMALL) + 2, U, 0x4A1E28, 255); gfx_text(F_SMALL, vx + U, vy + 3 * U + U / 2 + 1, "TLS OFF", ERR); }

	/* contextual hints */
	static const char *const hm[] = { "X", "Abrir", "L1", "R1", "Zoom", "Start", "Favoritos", "T", "Parar", "L2", "R2", "Volumen" };
	static const char *const hs[] = { "X", "Reproducir", "S", "Favorito", "O", "Volver", "L1", "R1", "Página", "T", "Parar" };
	static const char *const hf[] = { "X", "Reproducir", "S", "Quitar", "O", "Volver", "T", "Parar" };
	/* L1/R1 pairs are drawn as two pills followed by one label */
	const char *const *h; int n;
	if (m->view == UV_MAP) { h = hm; n = 12; } else if (m->view == UV_STATIONS) { h = hs; n = 11; } else { h = hf; n = 8; }
	int hy = H - 2 * U;
	int x = 3 * U, bs = 3 * U;
	for (int i = 0; i < n;) {
		bool pair = i + 2 < n && h[i][1] && h[i + 1][1] && (h[i][0] == 'L' || h[i][0] == 'R') && (h[i + 1][0] == 'L' || h[i + 1][0] == 'R');
		if (pair) { x += gfx_button(h[i], x, hy, bs) + U / 2; x += gfx_button(h[i + 1], x, hy, bs) + U; x += gfx_text(F_SMALL, x, hy - gfx_line_h(F_SMALL) / 2, h[i + 2], DIM) + 3 * U; i += 3; }
		else { x += gfx_button(h[i], x, hy, bs) + U; x += gfx_text(F_SMALL, x, hy - gfx_line_h(F_SMALL) / 2, h[i + 1], DIM) + 3 * U; i += 2; }
	}
	(void)hints; (void)hints_w;
}

static void draw_crosshair(const UiModel *m)
{
	float cx = W / 2.f, cy = H / 2.f;
	float pulse = 0.5f + 0.5f * sinf(m->frame * 0.08f);
	bool hot = m->hover >= 0;
	gfx_ring(cx, cy, 2.6f * U + (hot ? pulse * U : 0), 2.f, hot ? ACCENT : TEXT, hot ? 230 : 150);
	gfx_circle(cx, cy, 2.5f, TEXT, 220);
	if (hot) {
		const Place &p = m->places[m->hover];
		float sc = pxdeg(m);
		float px_ = W / 2 + (p.lon - m->lon) * sc, py_ = H / 2 - (p.lat - m->lat) * sc;
		gfx_circle(px_, py_, U * 0.7f, ACCENT, 255);
		char sub[96]; snprintf(sub, sizeof sub, "%s  \xC2\xB7  %d %s", p.country, p.size, p.size == 1 ? "emisora" : "emisoras");
		int w = gfx_text_w(F_BOLD, p.title); int w2 = gfx_text_w(F_SMALL, sub); if (w2 > w) w = w2;
		int cw = w + 4 * U + 12 * U, ch = gfx_line_h(F_BOLD) + gfx_line_h(F_SMALL) + 3 * U;
		int x = (int)cx + 4 * U, y = (int)cy - ch / 2;
		if (x + cw > W - 2 * U) x = (int)cx - 4 * U - cw;
		gfx_rrect(x + 3, y + 4, cw, ch, U, 0x000000, 90);
		gfx_rrect(x, y, cw, ch, U, CARD, 245);
		gfx_text(F_BOLD, x + 2 * U, y + U + U / 2, p.title, TEXT);
		gfx_text(F_SMALL, x + 2 * U, y + U + U / 2 + gfx_line_h(F_BOLD), sub, DIM);
		gfx_button("X", x + cw - 6 * U, y + ch / 2, 4 * U);
	}
}

static void top_hud(const UiModel *m)
{
	int y = 2 * U;
	gfx_rrect(2 * U, y, gfx_text_w(F_BOLD, "Radio Garden PS3") + 9 * U, gfx_line_h(F_BOLD) + 2 * U, 2 * U, PANEL, 220);
	gfx_circle(4 * U, y + gfx_line_h(F_BOLD) / 2.f + U, U * 0.8f, DOT_HI, 255);
	gfx_ring(4 * U, y + gfx_line_h(F_BOLD) / 2.f + U, U * 1.6f, 1.5f, DOT_HI, 150);
	gfx_text(F_BOLD, 6 * U + U / 2, y + U, "Radio Garden PS3", TEXT);
	char t[64]; snprintf(t, sizeof t, "%.2f\xC2\xB0 %c  %.2f\xC2\xB0 %c   x%.1f", fabsf(m->lat), m->lat >= 0 ? 'N' : 'S', fabsf(m->lon), m->lon >= 0 ? 'E' : 'O', m->zoom);
	int tw = gfx_text_w(F_SMALL, t) + 4 * U;
	gfx_rrect(W - tw - 2 * U, y, tw, gfx_line_h(F_SMALL) + 2 * U, 2 * U, PANEL, 200);
	gfx_text(F_SMALL, W - tw, y + U, t, DIM);
}

static void center_card(const UiModel *m, const char *title, const char *sub, bool spin, bool err)
{
	int w = 60 * U, h = 18 * U, x = (W - w) / 2, y = H / 2 - h - 6 * U;
	gfx_rrect(x, y, w, h, 2 * U, CARD, 245);
	if (spin) spinner(x + 6 * U, y + h / 2.f, 2.2f * U, m->frame);
	if (err) { gfx_circle(x + 6 * U, y + h / 2.f, 2.4f * U, ERR, 255); gfx_text(F_TITLE, x + 6 * U - gfx_text_w(F_TITLE, "!") / 2, y + h / 2 - gfx_line_h(F_TITLE) / 2, "!", CARD); }
	gfx_text_fit(F_BOLD, x + 11 * U, y + 4 * U, title, w - 13 * U, TEXT);
	gfx_text_fit(F_SMALL, x + 11 * U, y + 4 * U + gfx_line_h(F_BOLD) + U, sub, w - 13 * U, err ? ERR : DIM);
	if (err) { int hy = y + h - 3 * U; int bx = x + 11 * U; bx += gfx_button("X", bx, hy, 3 * U) + U; bx += gfx_text(F_SMALL, bx, hy - gfx_line_h(F_SMALL) / 2, "Reintentar", DIM) + 3 * U; bx += gfx_button("Select", bx, hy, 3 * U) + U; gfx_text(F_SMALL, bx, hy - gfx_line_h(F_SMALL) / 2, "TLS on/off", DIM); }
}

static void list_panel(const UiModel *m, const char *kicker, const char *title, const char *sub, int n, int sel, int top,
                       void (*row)(const UiModel *, int, int, int, int, bool), bool loading, const char *err)
{
	int pw = W * 42 / 100; if (pw < 70 * U) pw = 70 * U; if (pw > W - 20 * U) pw = W - 20 * U;
	int px = W - pw, ph = H - bar_h();
	gfx_rect_a(0, 0, px, ph, 0x02060C, 120);
	gfx_rect_a(px, 0, pw, ph, PANEL, 244);
	gfx_rect_a(px, 0, 1, ph, 0x24344F, 255);
	int x = px + 4 * U, y = 3 * U;
	gfx_text(F_SMALL, x, y, kicker, ACCENT);
	gfx_text_fit(F_TITLE, x, y + gfx_line_h(F_SMALL), title, pw - 8 * U, TEXT);
	gfx_text_fit(F_SMALL, x, y + gfx_line_h(F_SMALL) + gfx_line_h(F_TITLE), sub, pw - 8 * U, DIM);
	int ly = 16 * U, rh = gfx_line_h(F_BODY) + gfx_line_h(F_SMALL) + 2 * U, rows = ui_list_rows();
	if (loading) { spinner(px + pw / 2.f, ly + 8 * U, 2.4f * U, m->frame); const char *t = "Cargando emisoras..."; gfx_text(F_SMALL, px + (pw - gfx_text_w(F_SMALL, t)) / 2, ly + 12 * U, t, DIM); return; }
	if (err) { gfx_text_fit(F_BODY, x, ly + 2 * U, err, pw - 8 * U, ERR); return; }
	if (n == 0) { gfx_text_fit(F_BODY, x, ly + 2 * U, kicker[0] == 'T' ? "Aún no hay favoritos. Pulsa Cuadrado en una emisora." : "Sin emisoras", pw - 8 * U, DIM); return; }
	for (int i = 0; i < rows && top + i < n; i++) {
		int idx = top + i, ry = ly + i * rh;
		bool s = idx == sel;
		if (s) { gfx_rrect(px + 2 * U, ry, pw - 5 * U, rh - U / 2, U, SEL, 255); gfx_rrect(px + 2 * U, ry + U, U / 2, rh - 2 * U - U / 2, U / 4, ACCENT, 255); }
		row(m, idx, px + 4 * U, ry + U, pw - 11 * U, s);
	}
	if (n > rows) {
		int th = ph - ly - 3 * U, sh = th * rows / n; if (sh < 3 * U) sh = 3 * U;
		int sy = ly + (th - sh) * top / (n - rows);
		gfx_rrect(W - 2 * U, ly, U / 2, th, U / 4, 0x1E2C44, 255);
		gfx_rrect(W - 2 * U, sy, U / 2, sh, U / 4, DIM, 255);
	}
}

static void station_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Station &s = m->stations[i];
	bool playing = m->now_id && !strcmp(m->now_id, s.id) && m->pstate != P_IDLE;
	int ix = x;
	if (playing) { eq(x, y + 2, gfx_line_h(F_BODY) - 6, m->pstate == P_PLAYING ? m->frame : 0, m->pstate == P_PLAYING); ix += 7 * U; }
	bool fav = m->is_fav && m->is_fav(s.id);
	int nw = gfx_text_fit(sel ? F_BOLD : F_BODY, ix, y, s.name, w - (ix - x) - (fav ? 3 * U : 0), playing ? ACCENT : TEXT);
	if (fav) gfx_star(ix + nw + 2 * U, y + gfx_line_h(F_BODY) / 2.f, U * 0.8f, FAV, 255);
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

void ui_render(UiModel *m)
{
	if (m->map_dirty) { draw_map_layer(m); m->map_dirty = false; }
	memcpy(gfx_buffer(), maplayer, (size_t)W * H * 4);
	if (m->view == UV_MAP) draw_crosshair(m);
	top_hud(m);
	if (m->view == UV_MAP && m->loading == 1) center_card(m, "Cargando el mapa de emisoras", "Descargando miles de ciudades, puede tardar un poco...", true, false);
	if (m->view == UV_MAP && m->loading == 0 && m->error) center_card(m, "No se pudo cargar el mapa", m->error, false, true);
	if (m->view == UV_STATIONS && m->panel_place) {
		char sub[96]; snprintf(sub, sizeof sub, "%s  \xC2\xB7  %d emisoras", m->panel_place->country, m->loading == 2 ? m->panel_place->size : m->nst);
		list_panel(m, "EMISORAS EN", m->panel_place->title, sub, m->nst, m->st_sel, m->st_top, station_row, m->loading == 2, m->loading ? NULL : m->error);
	}
	if (m->view == UV_FAVS) {
		char sub[64]; snprintf(sub, sizeof sub, "%d guardadas", m->nfav);
		list_panel(m, "TUS EMISORAS", "Favoritos", sub, m->nfav, m->fav_sel, m->fav_top, fav_row, false, NULL);
	}
	draw_bar(m);
	if (m->toast && m->toast_t > 0) {
		int a = m->toast_t > 20 ? 245 : m->toast_t * 12;
		int tw = gfx_text_w(F_BODY, m->toast) + 6 * U, th = gfx_line_h(F_BODY) + 2 * U;
		int x = (W - tw) / 2, y = H - bar_h() - th - 3 * U;
		gfx_rrect(x, y, tw, th, th / 2, 0x26385A, a);
		gfx_text(F_BODY, x + 3 * U, y + U, m->toast, TEXT, a);
	}
}

bool ui_animating(const UiModel *m)
{
	return m->pstate == P_PLAYING || m->pstate == P_BUFFERING || m->pstate == P_CONNECTING || m->loading || m->toast_t > 0 || (m->view == UV_MAP && m->hover >= 0);
}
