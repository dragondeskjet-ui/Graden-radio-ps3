#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ui.h"
#include "gfx.h"
#include "player.h"
#include "landmask.h"

/* palette */
#define SPACE     0x03060D
#define ATMOS     0x4FA3FF
#define DOT_LO    0x2FD49A
#define DOT_HI    0x9BFFD6
#define ACCENT    0xFFC24B
#define TEXT      0xEEF3FA
#define DIM       0x91A0B8
#define PANEL     0x0C1624
#define PANEL2    0x101D31
#define CARD      0x13203A
#define LINE      0x1E2D45
#define SEL       0x1D3A66
#define ERR       0xFF6B6B
#define FAV       0xFF8FB1

#define OCEAN_DEEP 0x08172C
#define TEX_W 1440
#define TEX_H 720

static int W, H, U;
static unsigned char *land = NULL;
static u32 *maplayer = NULL;
static u32 *earth = NULL;          /* XRGB texture */
static int PX, PY, PW, PH;         /* left panel */
static int CX, CY, CH;             /* player card y/h */
static float VCX, VCY;             /* globe view centre */

static inline u32 lerpc(u32 a, u32 b, float t)
{
	int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
	return ((u32)(ar + (int)((br - ar) * t)) << 16) | ((u32)(ag + (int)((bg - ag) * t)) << 8) | (u32)(ab + (int)((bb - ab) * t));
}
static inline u32 scalec(u32 c, float k)
{
	int r = (int)(((c >> 16) & 255) * k), g = (int)(((c >> 8) & 255) * k), b = (int)((c & 255) * k);
	if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
	return (r << 16) | (g << 8) | b;
}

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
	PX = 3 * U; PY = 3 * U; PW = W * 30 / 100; if (PW < 54 * U) PW = 54 * U;
	CH = 17 * U; CY = H - CH - 3 * U; PH = CY - 2 * U - PY;
	VCX = PX + PW + (W - PX - PW) / 2.f; VCY = H / 2.f;
}

bool ui_load_earth(const char *path)
{
	FILE *f = fopen(path, "rb"); if (!f) return false;
	unsigned char *raw = (unsigned char *)malloc(TEX_W * TEX_H * 2);
	bool ok = raw && fread(raw, 2, TEX_W * TEX_H, f) == (size_t)(TEX_W * TEX_H);
	fclose(f);
	if (ok) {
		earth = (u32 *)malloc(TEX_W * TEX_H * 4);
		for (int i = 0; i < TEX_W * TEX_H; i++) {
			unsigned v = (raw[i * 2] << 8) | raw[i * 2 + 1];
			unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
			earth[i] = ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
		}
	}
	free(raw);
	return ok;
}

void ui_view_center(float *x, float *y) { *x = VCX; *y = VCY; }
float ui_globe_radius(const UiModel *m) { float base = fminf(W - PX - PW, H) * 0.43f; return base * m->zoom; }

bool ui_project(const UiModel *m, float lon, float lat, float *x, float *y)
{
	const float d = 0.0174533f;
	float la = lat * d, lo = (lon - m->lon) * d, la0 = m->lat * d;
	float cl = cosf(la);
	float z = sinf(la0) * sinf(la) + cosf(la0) * cl * cosf(lo);
	if (z <= 0.02f) return false;
	float R = ui_globe_radius(m);
	*x = VCX + R * cl * sinf(lo);
	*y = VCY - R * (cosf(la0) * sinf(la) - sinf(la0) * cl * cosf(lo));
	return true;
}

int ui_list_rows() { int top = PY + 19 * U, bottom = PY + PH - 2 * U; return (bottom - top) / (gfx_line_h(F_BODY) + gfx_line_h(F_SMALL) + 2 * U); }

/* ---------------- globe ---------------- */
static inline u32 tex_sample(float lon, float lat)
{
	float u = (lon + 180.f) / 360.f * TEX_W - 0.5f, v = (90.f - lat) / 180.f * TEX_H - 0.5f;
	int x0 = (int)floorf(u), y0 = (int)floorf(v); float fx = u - x0, fy = v - y0;
	int x1 = x0 + 1, y1 = y0 + 1;
	x0 = ((x0 % TEX_W) + TEX_W) % TEX_W; x1 = ((x1 % TEX_W) + TEX_W) % TEX_W;
	if (y0 < 0) y0 = 0; if (y1 >= TEX_H) y1 = TEX_H - 1; if (y0 >= TEX_H) y0 = TEX_H - 1; if (y1 < 0) y1 = 0;
	if (!earth) {
		int c = (int)((lon + 180.f) / 360.f * LAND_W) % LAND_W, r = (int)((90.f - lat) / 180.f * LAND_H);
		if (r < 0) r = 0; if (r >= LAND_H) r = LAND_H - 1;
		return land[r * LAND_W + c] ? 0x5F7D4A : 0x0A2440;
	}
	u32 a = lerpc(earth[y0 * TEX_W + x0], earth[y0 * TEX_W + x1], fx);
	u32 b = lerpc(earth[y1 * TEX_W + x0], earth[y1 * TEX_W + x1], fx);
	return lerpc(a, b, fy);
}
/* Near coasts the texture mixes land and sea; pick the brightest-green neighbour as land colour. */
static inline u32 tex_sample_land(float lon, float lat, u32 c)
{
	if (!earth) return 0x5F7D4A;
	u32 best = c; int bs = -999;
	for (int k = 0; k < 5; k++) {
		static const float o[5][2] = { {0,0}, {0.3f,0}, {-0.3f,0}, {0,0.3f}, {0,-0.3f} };
		u32 s = tex_sample(lon + o[k][0], lat + o[k][1]);
		int sc = (int)((s >> 8) & 255) + (int)((s >> 16) & 255) - 2 * (int)(s & 255);
		if (sc > bs) { bs = sc; best = s; }
	}
	return best;
}
static inline unsigned hash(unsigned x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }

static void draw_globe(const UiModel *m)
{
	const float d = 0.0174533f;
	float R = ui_globe_radius(m), la0 = m->lat * d, s0 = sinf(la0), c0 = cosf(la0);
	float Lx = -0.45f, Ly = 0.55f, Lz = 0.70f;          /* light from upper-left front */
	int step = m->fast ? 2 : 1;
	float cellpx = R * 3.14159f / LAND_H;          /* screen px per mask cell near centre */
	bool crisp = cellpx > 1.6f;
	float edge = 1.5f / cellpx;
	for (int y = 0; y < H; y += step) {
		float ny = (VCY - y - step * 0.5f + 0.5f) / R;
		u32 *row = maplayer + y * W;
		for (int x = 0; x < W; x += step) {
			float nx = (x + step * 0.5f - 0.5f - VCX) / R;
			float r2 = nx * nx + ny * ny;
			u32 c;
			if (r2 >= 1.f) {
				float r = sqrtf(r2);
				c = SPACE;
				unsigned hs = hash((unsigned)(x / step) * 7919u + (unsigned)(y / step) * 104729u);
				if ((hs & 1023) < 3) c = lerpc(SPACE, 0xBFD0F0, ((hs >> 10) & 255) / 255.f);
				if (r < 1.08f) { float t = 1.f - (r - 1.f) / 0.08f; c = lerpc(c, ATMOS, t * t * 0.55f); }
			} else {
				float z = sqrtf(1.f - r2);
				float lat = asinf(ny * c0 + z * s0) / d;
				float lon = m->lon + atan2f(nx, z * c0 - ny * s0) / d;
				if (lon > 180) lon -= 360; if (lon < -180) lon += 360;
				c = tex_sample(lon, lat);
				if (crisp) {
					/* sharpen coastlines: bilinear land mask threshold, ~1.5 px antialiased edge */
					float cu = (lon + 180.f) / 360.f * LAND_W - 0.5f, cv = (90.f - lat) / 180.f * LAND_H - 0.5f;
					int c0x = (int)floorf(cu), c0y = (int)floorf(cv); float fx = cu - c0x, fy = cv - c0y;
					int c1x = (c0x + 1) % LAND_W; c0x = (c0x % LAND_W + LAND_W) % LAND_W; c1x = (c1x + LAND_W) % LAND_W;
					int r0 = c0y < 0 ? 0 : (c0y >= LAND_H ? LAND_H - 1 : c0y), r1 = c0y + 1 >= LAND_H ? LAND_H - 1 : (c0y + 1 < 0 ? 0 : c0y + 1);
					float v = (land[r0 * LAND_W + c0x] * (1 - fx) + land[r0 * LAND_W + c1x] * fx) * (1 - fy) + (land[r1 * LAND_W + c0x] * (1 - fx) + land[r1 * LAND_W + c1x] * fx) * fy;
					float t = (v - 0.5f) / edge + 0.5f; t = t < 0 ? 0 : (t > 1 ? 1 : t);
					if (t < 1.f) {
						u32 oc = OCEAN_DEEP;
						u32 lc = t > 0.f ? tex_sample_land(lon, lat, c) : c;
						c = lerpc(oc, lc, t);
					}
				}
				float lam = nx * Lx + ny * Ly + z * Lz; if (lam < 0) lam = 0;
				float k = 0.42f + 0.78f * lam;
				c = scalec(c, k);
				float rim = 1.f - z; rim = rim * rim * rim;   /* atmospheric scattering at the limb */
				c = lerpc(c, ATMOS, rim * 0.55f);
			}
			row[x] = c;
			if (step == 2) { if (x + 1 < W) row[x + 1] = c; if (y + 1 < H) { row[W + x] = c; if (x + 1 < W) row[W + x + 1] = c; } }
		}
	}
	/* stations: soft glow + bright core, scaled with zoom */
	gfx_set_target(maplayer);
	float zr = m->zoom < 2 ? 0.9f : (m->zoom < 8 ? 1.25f : 1.8f);
	for (int i = 0; i < m->nplaces; i++) {
		const Place &p = m->places[i];
		float x, y;
		if (!ui_project(m, p.lon, p.lat, &x, &y)) continue;
		if (x < -6 || y < -6 || x > W + 6 || y > H + 6) continue;
		float t = p.size >= 40 ? 1.f : p.size / 40.f;
		float r = (0.9f + 1.4f * sqrtf(t)) * zr * (U / 8.f);
		if (t > 0.3f) gfx_circle(x, y, r * 2.2f, DOT_LO, 40);
		gfx_circle(x, y, r, lerpc(DOT_LO, DOT_HI, t), 170 + (int)(85 * t));
	}
	gfx_set_target(NULL);
}

/* ---------------- widgets ---------------- */
static void spinner(float cx, float cy, float r, unsigned f)
{
	gfx_ring(cx, cy, r, U * 0.5f, 0x22324D, 255);
	float a0 = fmodf(f * 0.15f, 6.2832f);
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
	float r = 3.4f * U;
	gfx_circle(cx, cy, r, TEXT, 255);
	const FontDef *f = strlen(t) > 3 ? F_SMALL : F_BOLD;
	gfx_text(f, cx - gfx_text_w(f, t) / 2, cy - gfx_line_h(f) / 2, t, PANEL);
}

static void panel_header(const char *title, const char *sub, int count, bool has_badge)
{
	int hh = 14 * U;
	gfx_rrect(PX, PY, PW, hh + 2 * U, 2 * U, PANEL2, 250);
	gfx_rect(PX, PY + hh, PW, 2 * U, PANEL);   /* square bottom edge joins list */
	int tx = PX + 3 * U;
	if (has_badge) { badge(PX + 6 * U, PY + hh / 2, count); tx = PX + 11 * U; }
	gfx_text_fit(F_TITLE, tx, PY + hh / 2 - gfx_line_h(F_TITLE) + U / 2, title, PW - (tx - PX) - 3 * U, TEXT);
	gfx_text_fit(F_BODY, tx, PY + hh / 2 + U / 2, sub, PW - (tx - PX) - 3 * U, DIM);
	gfx_rect(PX, PY + hh, PW, 1, LINE);
}

typedef void (*RowFn)(const UiModel *, int, int, int, int, bool);

static void panel_list(const UiModel *m, const char *section, int n, int sel, int top, RowFn row, bool loading, const char *err, const char *empty)
{
	int x = PX + 3 * U, w = PW - 6 * U, y = PY + 14 * U + 2 * U;
	gfx_text(F_BOLD, x, y, section, TEXT);
	y += gfx_line_h(F_BOLD) + U;
	gfx_rect(x, y, w, 1, LINE);
	int ly = PY + 19 * U, rh = gfx_line_h(F_BODY) + gfx_line_h(F_SMALL) + 2 * U, rows = ui_list_rows();
	if (loading) { spinner(PX + PW / 2.f, ly + 8 * U, 2.4f * U, m->frame); const char *t = "Cargando emisoras..."; gfx_text(F_SMALL, PX + (PW - gfx_text_w(F_SMALL, t)) / 2, ly + 12 * U, t, DIM); return; }
	if (err) { gfx_text_fit(F_BODY, x, ly + 2 * U, err, w, ERR); return; }
	if (n == 0) { gfx_text_fit(F_BODY, x, ly + 2 * U, empty, w, DIM); return; }
	for (int i = 0; i < rows && top + i < n; i++) {
		int idx = top + i, ry = ly + i * rh;
		bool s = idx == sel;
		if (s) { gfx_rrect(PX + U, ry, PW - 3 * U, rh - U / 2, U, SEL, 255); gfx_rrect(PX + U, ry + U, U / 2, rh - 2 * U - U / 2, U / 4, ACCENT, 255); }
		else if (i) gfx_rect(x, ry - U / 4, w, 1, LINE);
		row(m, idx, x, ry + U, w - 3 * U, s);
	}
	if (n > rows) {
		int th = rows * rh, sh = th * rows / n; if (sh < 3 * U) sh = 3 * U;
		int sy = ly + (th - sh) * top / (n - rows);
		gfx_rrect(PX + PW - U - U / 2, ly, U / 2, th, U / 4, LINE, 255);
		gfx_rrect(PX + PW - U - U / 2, sy, U / 2, sh, U / 4, DIM, 255);
	}
}

static void station_row(const UiModel *m, int i, int x, int y, int w, bool sel)
{
	const Station &s = m->stations[i];
	bool playing = m->now_id && !strcmp(m->now_id, s.id) && m->pstate != P_IDLE;
	int ix = x;
	if (playing) { eq(x, y + 2, gfx_line_h(F_BODY) - 6, m->pstate == P_PLAYING ? m->frame : 0, m->pstate == P_PLAYING); ix += 7 * U; }
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

static void panel(const UiModel *m)
{
	gfx_rrect(PX + 3, PY + 5, PW, PH, 2 * U, 0x000000, 90);
	gfx_rrect(PX, PY, PW, PH, 2 * U, PANEL, 246);
	if (m->view == UV_STATIONS && m->panel_place) {
		const Place *p = m->panel_place;
		panel_header(p->title, p->country, m->loading == 2 ? p->size : m->nst, true);
		char sec[80]; snprintf(sec, sizeof sec, "Emisoras en %s", p->title);
		panel_list(m, sec, m->nst, m->st_sel, m->st_top, station_row, m->loading == 2, m->loading ? NULL : m->error, "Sin emisoras");
	} else if (m->view == UV_FAVS) {
		char sub[48]; snprintf(sub, sizeof sub, "%d %s", m->nfav, m->nfav == 1 ? "emisora guardada" : "emisoras guardadas");
		panel_header("Favoritos", sub, 0, false);
		panel_list(m, "Tus emisoras", m->nfav, m->fav_sel, m->fav_top, fav_row, false, NULL, "Aún no hay favoritos. Pulsa Cuadrado en una emisora.");
	} else {
		int x = PX + 3 * U, w = PW - 6 * U;
		if (m->hover >= 0) {
			const Place &p = m->places[m->hover];
			panel_header(p.title, p.country, p.size, true);
			int y = PY + 18 * U;
			char t[64]; snprintf(t, sizeof t, "%d %s en esta ciudad", p.size, p.size == 1 ? "emisora" : "emisoras");
			gfx_text_fit(F_BODY, x, y, t, w, TEXT);
			y += gfx_line_h(F_BODY) + 2 * U;
			int hx = x; hx = hint(hx, y + U + U / 2, "X", NULL, "Ver emisoras");
		} else {
			panel_header("Explorar", "Gira el globo y apunta a una ciudad", 0, false);
			int y = PY + 18 * U;
			const char *tips[][3] = { { "L1", "R1", "Acercar y alejar" }, { "L3", NULL, "Ir a un lugar al azar" }, { "Start", NULL, "Favoritos" }, { "Select", NULL, "Verificación TLS" } };
			for (int i = 0; i < 4; i++) hint(x, y + i * 5 * U + U + U / 2, tips[i][0], tips[i][1], tips[i][2]);
		}
		/* nearby cities */
		if (m->nplaces > 0) {
			const int K = 6; int idx[K]; float dist[K]; int nk = 0;
			float cl0 = cosf(m->lat * 0.0174533f);
			for (int i = 0; i < m->nplaces; i++) {
				if (i == m->hover) continue;
				float dl = m->places[i].lon - m->lon; if (dl > 180) dl -= 360; if (dl < -180) dl += 360;
				float dx = dl * cl0, dy = m->places[i].lat - m->lat, dd = dx * dx + dy * dy;
				if (nk < K) { idx[nk] = i; dist[nk++] = dd; }
				else { int w = 0; for (int k = 1; k < K; k++) if (dist[k] > dist[w]) w = k; if (dd < dist[w]) { idx[w] = i; dist[w] = dd; } else continue; }
			}
			for (int a = 0; a < nk; a++) for (int b = a + 1; b < nk; b++) if (dist[b] < dist[a]) { float t = dist[a]; dist[a] = dist[b]; dist[b] = t; int q = idx[a]; idx[a] = idx[b]; idx[b] = q; }
			int y = PY + (m->hover >= 0 ? 30 : 40) * U;
			int rh = gfx_line_h(F_BODY) + U;
			int maxn = (PY + PH - 3 * U - y - gfx_line_h(F_BOLD) - 2 * U) / rh; if (maxn > nk) maxn = nk;
			if (maxn > 0) {
				gfx_text(F_BOLD, x, y, "Cerca de aquí", TEXT);
				y += gfx_line_h(F_BOLD) + U; gfx_rect(x, y, w, 1, LINE); y += U;
				for (int k = 0; k < maxn; k++) {
					const Place &q = m->places[idx[k]];
					char n[8]; snprintf(n, sizeof n, "%d", q.size);
					int nw = gfx_text_w(F_SMALL, n);
					gfx_circle(x + U, y + gfx_line_h(F_BODY) / 2.f, U * 0.55f, DOT_HI, 255);
					gfx_text_fit(F_BODY, x + 3 * U, y, q.title, w - 3 * U - nw - 3 * U, TEXT);
					gfx_text(F_SMALL, x + w - nw, y + (gfx_line_h(F_BODY) - gfx_line_h(F_SMALL)) / 2, n, DIM);
					y += rh;
				}
			}
		}
		if (m->loading == 1) { int y = PY + PH - 12 * U; spinner(x + 2.5f * U, y + 2.5f * U, 2.f * U, m->frame); gfx_text_fit(F_BODY, x + 7 * U, y + U, "Cargando ciudades...", w - 7 * U, TEXT); }
		if (!m->loading && m->error) {
			int y = PY + PH - 14 * U;
			gfx_text_fit(F_BOLD, x, y, "No se pudo cargar el mapa", w, ERR);
			gfx_text_fit(F_SMALL, x, y + gfx_line_h(F_BOLD), m->error, w, DIM);
			int hx = hint(x, y + gfx_line_h(F_BOLD) + gfx_line_h(F_SMALL) + 3 * U, "X", NULL, "Reintentar");
			(void)hx;
		}
	}
}

static void player_card(const UiModel *m)
{
	gfx_rrect(PX + 3, CY + 5, PW, CH, 2 * U, 0x000000, 90);
	gfx_rrect(PX, CY, PW, CH, 2 * U, PANEL, 246);
	int x = PX + 3 * U, w = PW - 6 * U, y = CY + 2 * U, st = m->pstate;
	int ic = 5 * U;
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
	/* controls row: stop + volume slider */
	int cy = CY + CH - 4 * U;
	int hx = hint(x, cy, "T", NULL, "Parar");
	int bx = hx, bw = PW - (bx - PX) - 9 * U;
	gfx_button("L2", bx, cy, 3 * U); bx += gfx_text_w(F_SMALL, "L2") + 3 * U / 2 + 4 + U;
	int ex = PX + PW - 3 * U - (gfx_text_w(F_SMALL, "R2") + 3 * U / 2 + 4);
	bw = ex - U - bx;
	gfx_rrect(bx, cy - U / 4, bw, U / 2 + 1, U / 4, 0x26354F, 255);
	gfx_rrect(bx, cy - U / 4, (int)(bw * m->volume), U / 2 + 1, U / 4, ACCENT, 255);
	gfx_circle(bx + bw * m->volume, cy, U * 0.9f, TEXT, 255);
	gfx_button("R2", ex, cy, 3 * U);
}

static void crosshair(const UiModel *m)
{
	bool hot = m->hover >= 0;
	float pulse = 0.5f + 0.5f * sinf(m->frame * 0.08f);
	float r = 5.5f * U + (hot ? pulse * U * 0.6f : 0);
	gfx_ring(VCX, VCY, r, 2.2f, hot ? ACCENT : TEXT, hot ? 235 : 160);
	gfx_circle(VCX, VCY, 2.5f, TEXT, 230);
	if (hot) { float x, y; if (ui_project(m, m->places[m->hover].lon, m->places[m->hover].lat, &x, &y)) { gfx_circle(x, y, U * 0.75f, ACCENT, 255); gfx_ring(x, y, U * 1.4f, 1.5f, ACCENT, 200); } }
}

static void hud(const UiModel *m)
{
	char t[64]; snprintf(t, sizeof t, "%.2f\xC2\xB0 %c   %.2f\xC2\xB0 %c   x%.1f", fabsf(m->lat), m->lat >= 0 ? 'N' : 'S', fabsf(m->lon), m->lon >= 0 ? 'E' : 'O', m->zoom);
	int tw = gfx_text_w(F_SMALL, t) + 4 * U;
	gfx_rrect(W - tw - 3 * U, 3 * U, tw, gfx_line_h(F_SMALL) + 2 * U, 2 * U, PANEL, 200);
	gfx_text(F_SMALL, W - tw - U, 4 * U, t, DIM);
	if (!m->tls) { int cw = gfx_text_w(F_SMALL, "TLS OFF") + 4 * U; gfx_rrect(W - cw - 3 * U, 7 * U + gfx_line_h(F_SMALL), cw, gfx_line_h(F_SMALL) + 2 * U, 2 * U, 0x4A1E28, 230); gfx_text(F_SMALL, W - cw - U, 8 * U + gfx_line_h(F_SMALL), "TLS OFF", ERR); }
	/* contextual hints along the bottom of the globe area */
	int y = H - 3 * U - U, x = PX + PW + 4 * U;
	gfx_rrect(x - 2 * U, y - 2 * U - U / 2, W - x - U, 5 * U, 2 * U + U / 2, PANEL, 170);
	if (m->view == UV_MAP) { x = hint(x, y, "X", NULL, "Abrir"); x = hint(x, y, "L1", "R1", "Zoom"); x = hint(x, y, "L3", NULL, "Azar"); hint(x, y, "Start", NULL, "Favoritos"); }
	else if (m->view == UV_STATIONS) { x = hint(x, y, "X", NULL, "Reproducir"); x = hint(x, y, "S", NULL, "Favorito"); x = hint(x, y, "L1", "R1", "Página"); hint(x, y, "O", NULL, "Volver"); }
	else { x = hint(x, y, "X", NULL, "Reproducir"); x = hint(x, y, "S", NULL, "Quitar"); hint(x, y, "O", NULL, "Volver"); }
}

void ui_render(UiModel *m)
{
	if (m->map_dirty) { draw_globe(m); m->map_dirty = false; }
	memcpy(gfx_buffer(), maplayer, (size_t)W * H * 4);
	if (m->view == UV_MAP) crosshair(m);
	panel(m);
	player_card(m);
	hud(m);
	if (m->toast && m->toast_t > 0) {
		int a = m->toast_t > 20 ? 245 : m->toast_t * 12;
		int tw = gfx_text_w(F_BODY, m->toast) + 6 * U, th = gfx_line_h(F_BODY) + 2 * U;
		int x = (int)VCX - tw / 2, y = 3 * U;
		gfx_rrect(x, y, tw, th, th / 2, 0x26385A, a);
		gfx_text(F_BODY, x + 3 * U, y + U, m->toast, TEXT, a);
	}
}

bool ui_animating(const UiModel *m)
{
	return m->pstate == P_PLAYING || m->pstate == P_BUFFERING || m->pstate == P_CONNECTING || m->loading || m->toast_t > 0 || (m->view == UV_MAP && m->hover >= 0);
}
