#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <io/pad.h>
#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "rsxutil.h"
#include "gfx.h"
#include "gpu.h"
#include "ui.h"
#include "http.h"
#include "api.h"
#include "player.h"
#include "log.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define DATA_DIR  "/dev_hdd0/game/RGDN00001/USRDIR/"
#define FAV_FILE  DATA_DIR "favoritos.txt"
#define CONF_FILE DATA_DIR "config.txt"
#define MAX_FAVS  200

static volatile int running = 1;
extern "C" {
static void program_exit_callback() { sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0); gcmSetWaitFlip(context); rsxFinish(context, 1); }
static void sysutil_exit_callback(u64 status, u64, void *) { if (status == SYSUTIL_EXIT_GAME) running = 0; }
}

static u64 now_us() { u64 s, ns; sysGetCurrentTime(&s, &ns); return s * 1000000ull + ns / 1000; }

/* ---------- background job ---------- */
enum { J_NONE, J_PLACES, J_STATIONS };
static volatile int job_kind = J_NONE, job_state = 0;
static char job_err[128], job_arg[16];
static sys_ppu_thread_t job_th; static int job_started = 0;
static void job_entry(void *)
{
	bool ok = job_kind == J_PLACES ? api_load_places(job_err, sizeof job_err) : api_load_stations(job_arg, job_err, sizeof job_err);
	job_state = ok ? 2 : 3;
	sysThreadExit(0);
}
static void job_start(int kind, const char *arg)
{
	if (job_started) { u64 r; sysThreadJoin(job_th, &r); job_started = 0; }
	job_kind = kind; job_state = 1; job_err[0] = 0;
	snprintf(job_arg, sizeof job_arg, "%s", arg ? arg : "");
	if (sysThreadCreate(&job_th, job_entry, NULL, 1001, 0x80000, THREAD_JOINABLE, (char *)"rg_job") == 0) job_started = 1;
	else { job_state = 3; snprintf(job_err, sizeof job_err, "No se pudo crear el hilo"); }
}

/* ---------- favourites ---------- */
static Fav favs[MAX_FAVS]; static int nfav = 0;
static bool is_fav(const char *id) { for (int i = 0; i < nfav; i++) if (!strcmp(favs[i].id, id)) return true; return false; }
static void favs_save()
{
	FILE *f = fopen(FAV_FILE, "w"); if (!f) return;
	for (int i = 0; i < nfav; i++) fprintf(f, "%s\t%s\t%s\n", favs[i].id, favs[i].name, favs[i].place);
	fclose(f);
}
static void favs_load()
{
	FILE *f = fopen(FAV_FILE, "r"); if (!f) return;
	char line[256];
	while (nfav < MAX_FAVS && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "\t");
		if (!a || !b) continue;
		Fav &v = favs[nfav++];
		snprintf(v.id, sizeof v.id, "%s", a); snprintf(v.name, sizeof v.name, "%s", b); snprintf(v.place, sizeof v.place, "%s", c ? c : "");
	}
	fclose(f);
}
static bool fav_toggle(const char *id, const char *name, const char *place)
{
	for (int i = 0; i < nfav; i++) if (!strcmp(favs[i].id, id)) { memmove(&favs[i], &favs[i + 1], (nfav - i - 1) * sizeof(Fav)); nfav--; favs_save(); return false; }
	if (nfav >= MAX_FAVS) return false;
	memmove(&favs[1], &favs[0], nfav * sizeof(Fav)); nfav++;
	snprintf(favs[0].id, sizeof favs[0].id, "%s", id); snprintf(favs[0].name, sizeof favs[0].name, "%s", name); snprintf(favs[0].place, sizeof favs[0].place, "%s", place);
	favs_save(); return true;
}

/* ---------- countries ---------- */
static Country *countries = NULL; static int ncountries = 0;
static int *country_places = NULL;
static const Country **cfilter = NULL; static int ncfilter = 0;
static char query[32] = "";

static int cmp_country(const void *a, const void *b) { return strcasecmp(((const Country *)a)->name, ((const Country *)b)->name); }
static const Place *sort_base;
static int cmp_place_size(const void *a, const void *b) { return (int)sort_base[*(const int *)b].size - (int)sort_base[*(const int *)a].size; }

static void build_countries()
{
	free(countries); free(country_places); free(cfilter);
	countries = (Country *)calloc(g_nplaces + 1, sizeof(Country)); ncountries = 0;
	int *cidx = (int *)malloc(sizeof(int) * (g_nplaces + 1));
	for (int i = 0; i < g_nplaces; i++) {
		int c = -1;
		for (int k = ncountries - 1; k >= 0; k--) if (!strcmp(countries[k].name, g_places[i].country)) { c = k; break; }
		if (c < 0) { c = ncountries++; snprintf(countries[c].name, sizeof countries[c].name, "%s", g_places[i].country[0] ? g_places[i].country : "?"); }
		countries[c].nplaces++; countries[c].nstations += g_places[i].size; cidx[i] = c;
	}
	/* stable sort by name, remembering mapping */
	int *order = (int *)malloc(sizeof(int) * (ncountries + 1));
	for (int k = 0; k < ncountries; k++) order[k] = k;
	Country *sorted = (Country *)malloc(sizeof(Country) * (ncountries + 1));
	memcpy(sorted, countries, sizeof(Country) * ncountries);
	qsort(sorted, ncountries, sizeof(Country), cmp_country);
	int *remap = (int *)malloc(sizeof(int) * (ncountries + 1));
	for (int k = 0; k < ncountries; k++) for (int j = 0; j < ncountries; j++) if (!strcmp(sorted[j].name, countries[k].name)) { remap[k] = j; break; }
	free(countries); countries = sorted;
	int off = 0; for (int k = 0; k < ncountries; k++) { countries[k].first = off; off += countries[k].nplaces; countries[k].nplaces = 0; }
	country_places = (int *)malloc(sizeof(int) * (g_nplaces + 1));
	for (int i = 0; i < g_nplaces; i++) { Country &c = countries[remap[cidx[i]]]; country_places[c.first + c.nplaces++] = i; }
	sort_base = g_places;
	for (int k = 0; k < ncountries; k++) qsort(country_places + countries[k].first, countries[k].nplaces, sizeof(int), cmp_place_size);
	free(order); free(remap); free(cidx);
	cfilter = (const Country **)malloc(sizeof(Country *) * (ncountries + 1));
}

/* lower-case ASCII with common Latin-1 accents folded, for matching */
static void fold(const char *s, char *o, size_t n)
{
	size_t k = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p && k + 1 < n; p++) {
		int c = *p;
		if (c == 0xC3 && p[1]) {
			int d = p[1] | 0x20; p++;
			c = (d >= 0xE0 && d <= 0xE5) ? 'a' : (d == 0xE7) ? 'c' : (d >= 0xE8 && d <= 0xEB) ? 'e' : (d >= 0xEC && d <= 0xEF) ? 'i' :
			    (d == 0xF1) ? 'n' : (d >= 0xF2 && d <= 0xF6) ? 'o' : (d >= 0xF9 && d <= 0xFC) ? 'u' : '?';
		}
		o[k++] = (char)tolower(c);
	}
	o[k] = 0;
}
static void apply_filter()
{
	char q[40]; fold(query, q, sizeof q);
	ncfilter = 0;
	for (int k = 0; k < ncountries; k++) {
		char nm[64]; fold(countries[k].name, nm, sizeof nm);
		if (!q[0] || strstr(nm, q)) cfilter[ncfilter++] = &countries[k];
	}
	/* prefix matches first */
	if (q[0]) { int w = 0; for (int i = 0; i < ncfilter; i++) { char nm[64]; fold(cfilter[i]->name, nm, sizeof nm); if (!strncmp(nm, q, strlen(q))) { const Country *t = cfilter[i]; memmove(&cfilter[w + 1], &cfilter[w], (i - w) * sizeof(*cfilter)); cfilter[w++] = t; } } }
}

/* ---------- config ---------- */
static void conf_save(const UiModel &m)
{
	FILE *f = fopen(CONF_FILE, "w"); if (!f) return;
	fprintf(f, "%f %f %f %f %d %d %d %d %d\n", m.lon, m.lat, m.zoom, player_get_volume(), (int)g_tls_verify, m.theme, m.ar, m.ag, m.ab);
	fclose(f);
}
static void conf_load(UiModel &m)
{
	FILE *f = fopen(CONF_FILE, "r"); if (!f) return;
	float lo, la, z, v; int t, th = 0, r = 255, g = 194, b = 75;
	int n = fscanf(f, "%f %f %f %f %d %d %d %d %d", &lo, &la, &z, &v, &t, &th, &r, &g, &b);
	if (n >= 5) { m.lon = lo; m.lat = la; m.zoom = z < 1 ? 1 : (z > 40 ? 40 : z); player_set_volume(v); g_tls_verify = t; }
	if (n >= 9) { m.theme = th % UI_NTHEMES; m.ar = r & 255; m.ag = g & 255; m.ab = b & 255; }
	fclose(f);
}

/* ---------- state ---------- */
static UiModel M;
static char now_name[96], now_place[96], now_id[16];
static float tgt_lon, tgt_lat, tgt_zoom; static bool anim_cam = false;
static UiView back_from_stations = UV_MAP;

static void toast(const char *t) { static char buf[96]; snprintf(buf, sizeof buf, "%s", t); M.toast = buf; M.toast_t = 120; }

static void play(const char *id, const char *name, const char *place)
{
	char url[200];
	api_stream_url(id, url, sizeof url);
	snprintf(now_id, sizeof now_id, "%s", id); snprintf(now_name, sizeof now_name, "%s", name); snprintf(now_place, sizeof now_place, "%s", place);
	player_play(url);
}
static void fly_to(const Place &p, float min_zoom)
{
	tgt_lat = p.lat; tgt_lon = p.lon; tgt_zoom = M.zoom < min_zoom ? min_zoom : M.zoom; anim_cam = true;
}
static void open_stations(int pi, UiView back)
{
	M.panel_place = &g_places[pi];
	M.st_sel = M.st_top = 0; M.nst = 0; g_nstations = 0; M.error = NULL;
	job_start(J_STATIONS, g_places[pi].id);
	M.loading = 2; M.view = UV_STATIONS; back_from_stations = back;
	fly_to(g_places[pi], 4);
}
static void scroll_fix(int sel, int *top, int n, int rows)
{
	if (sel < *top) *top = sel;
	if (sel >= *top + rows) *top = sel - rows + 1;
	if (*top > n - rows) *top = n - rows > 0 ? n - rows : 0;
	if (*top < 0) *top = 0;
}
/* list navigation shared by all lists; returns true if selection moved */
static bool list_nav(int *sel, int *top, int n, int rows, bool up, bool dn, bool pgup, bool pgdn)
{
	if (n <= 0) return false;
	int p = *sel;
	if (up) (*sel)--; if (dn) (*sel)++;
	if (pgup) *sel -= rows; if (pgdn) *sel += rows;
	if (*sel < 0) *sel = 0; if (*sel >= n) *sel = n - 1;
	scroll_fix(*sel, top, n, rows);
	return p != *sel;
}

int main(int, const char **)
{
	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	init_screen(host_addr, HOST_SIZE);
	ioPadInit(7);
	atexit(program_exit_callback);
	sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);
	setRenderTarget(curr_fb);
	rg_log_reset();
	rg_log("start %ux%u", display_width, display_height);
	gpu_init(display_width, display_height);
	ui_init(display_width, display_height);

	memset(&M, 0, sizeof M);
	M.view = UV_MAP; M.lon = -30; M.lat = 15; M.zoom = 1.f; M.hover = -1;
	M.stations = g_stations; M.favs = favs; M.is_fav = is_fav; M.ui_dirty = true;
	M.now_name = now_name; M.now_place = now_place; M.now_id = now_id; M.query = query;
	M.ar = 0xFF; M.ag = 0xC2; M.ab = 0x4B;
	conf_load(M);
	/* first frame while the 16 MB texture loads into video memory */
	ui_frame(&M); flip();
	if (!gpu_load_earth(DATA_DIR "earth.565")) toast("No se encontró earth.565 (globo sin textura)");

	bool net_ok = http_init();
	rg_log("http_init %s", net_ok ? "ok" : "FAILED");
	bool audio_ok = player_init();
	rg_log("player_init %s", audio_ok ? "ok" : "FAILED");
	favs_load();
	if (net_ok) { job_start(J_PLACES, NULL); M.loading = 1; rg_log("job places started"); }
	else M.error = "Red no disponible. Revisa la conexión de la PS3.";
	if (!audio_ok) toast("No se pudo abrir la salida de audio");

	padInfo pinfo; padData pd;
	u32 prev = 0; int rep = 0, save_timer = 0, still = 0, last_hover = -2, last_state = -1, last_nst = -1;
	float last_vol = -1; int fps_frames = 0; u64 fps_t0 = now_us();

	while (running) {
		sysUtilCheckCallback();
		ioPadGetInfo(&pinfo);
		static int lx = 0, ly = 0, ry = 0;
		u32 b = 0; bool padfound = false;
		for (int i = 0; i < MAX_PADS; i++) {
			if (!pinfo.status[i]) continue;
			ioPadGetData(i, &pd);
			padfound = true;
			if (pd.len == 0) { b = prev; break; }   /* no new data this frame: keep buttons and sticks */
			b = (pd.BTN_CROSS << 0) | (pd.BTN_CIRCLE << 1) | (pd.BTN_TRIANGLE << 2) | (pd.BTN_SQUARE << 3) |
			    (pd.BTN_START << 4) | (pd.BTN_SELECT << 5) | (pd.BTN_L1 << 6) | (pd.BTN_R1 << 7) | (pd.BTN_L2 << 8) | (pd.BTN_R2 << 9) |
			    (pd.BTN_UP << 10) | (pd.BTN_DOWN << 11) | (pd.BTN_LEFT << 12) | (pd.BTN_RIGHT << 13) | (pd.BTN_L3 << 14) | (pd.BTN_R3 << 15);
			int hx = (int)pd.ANA_L_H - 128, hy = (int)pd.ANA_L_V - 128, rv = (int)pd.ANA_R_V - 128;
			lx = abs(hx) > 28 ? hx : 0; ly = abs(hy) > 28 ? hy : 0; ry = abs(rv) > 28 ? rv : 0;
			break;
		}
		if (!padfound) { lx = ly = ry = 0; }
		u32 pr = b & ~prev; prev = b;
		#define P(bit) (pr & (1u << (bit)))
		#define D(bit) (b & (1u << (bit)))
		bool stepup = false, stepdn = false, stepl = false, stepr = false;
		if (D(10) || D(11) || D(12) || D(13) || ly) {
			if (rep == 0 || (rep > 24 && rep % 5 == 0)) { stepup = D(10) || ly < -60; stepdn = D(11) || ly > 60; stepl = D(12); stepr = D(13); }
			rep++;
		} else rep = 0;
		bool uidirty = pr != 0 || stepup || stepdn || stepl || stepr;

		/* background jobs */
		if (M.loading && job_state != 1) {
			if (job_kind == J_PLACES) {
				if (job_state == 2) {
					rg_log("places loaded: %d", g_nplaces);
					M.places = g_places; M.nplaces = g_nplaces; M.error = NULL;
					ui_set_places(g_places, g_nplaces); rg_log("ui_set_places ok");
					build_countries(); rg_log("countries: %d", ncountries);
					apply_filter(); rg_log("filter ok");
					char t[64]; snprintf(t, sizeof t, "%d ciudades con radio", g_nplaces); toast(t);
				} else M.error = job_err;
			} else if (job_kind == J_STATIONS) { if (job_state == 2) { M.nst = g_nstations; M.error = NULL; } else M.error = job_err; }
			M.loading = 0; uidirty = true;
		}

		/* global buttons */
		if (P(2)) { player_stop(); toast("Detenido"); }
		if (P(15)) { g_tls_verify = !g_tls_verify; toast(g_tls_verify ? "Verificación TLS activada" : "Verificación TLS desactivada"); save_timer = 60; }
		if (D(8) || D(9)) { player_set_volume(player_get_volume() + (D(9) ? 0.01f : -0.01f)); save_timer = 120; }
		if (P(4) && !M.kb_open) { M.view = M.view == UV_MENU ? UV_MAP : UV_MENU; }
		if (P(5) && !M.kb_open) { if (M.view == UV_COUNTRIES || M.view == UV_CITIES) M.view = UV_MAP; else { M.view = UV_COUNTRIES; apply_filter(); M.c_sel = M.c_top = 0; } }

		bool moved = false;
		if (M.view == UV_MAP) {
			if (M.error && !M.loading && P(0) && net_ok) { job_start(J_PLACES, NULL); M.loading = 1; M.error = NULL; }
			float spd = 1.5f / M.zoom;
			float mx = (D(13) ? 1.f : 0.f) - (D(12) ? 1.f : 0.f) + lx / 100.f;
			float my = (D(10) ? 1.f : 0.f) - (D(11) ? 1.f : 0.f) - ly / 100.f;
			if (mx || my) {
				M.lon += mx * spd / fmaxf(0.25f, cosf(M.lat * 0.0174533f)); M.lat += my * spd; anim_cam = false;
				if (M.lon > 180) M.lon -= 360; if (M.lon < -180) M.lon += 360;
				if (M.lat > 85) M.lat = 85; if (M.lat < -85) M.lat = -85;
				moved = true;
			}
			if (P(14) && M.nplaces) { fly_to(g_places[rand() % M.nplaces], 5); toast("Viajando a un lugar al azar…"); }
			if (P(0) && M.hover >= 0 && !M.loading) open_stations(M.hover, UV_MAP);
		} else if (M.view == UV_STATIONS) {
			if (P(1)) { M.view = back_from_stations; anim_cam = false; }
			int rows = ui_rows(&M);
			list_nav(&M.st_sel, &M.st_top, M.nst, rows, stepup, stepdn, P(6), P(7));
			if (M.nst > 0 && !M.loading) {
				const Station &s = g_stations[M.st_sel];
				char place[96]; snprintf(place, sizeof place, "%s, %s", M.panel_place->title, M.panel_place->country);
				if (P(0) && audio_ok) play(s.id, s.name, place);
				if (P(3)) toast(fav_toggle(s.id, s.name, place) ? "Añadido a favoritos" : "Quitado de favoritos");
			}
		} else if (M.view == UV_FAVS) {
			if (P(1)) M.view = UV_MAP;
			int rows = ui_rows(&M);
			list_nav(&M.fav_sel, &M.fav_top, nfav, rows, stepup, stepdn, P(6), P(7));
			if (nfav > 0) {
				Fav f = favs[M.fav_sel];
				if (P(0) && audio_ok) play(f.id, f.name, f.place);
				if (P(3)) { fav_toggle(f.id, f.name, f.place); if (M.fav_sel >= nfav && nfav) M.fav_sel = nfav - 1; toast("Quitado de favoritos"); }
			}
		} else if (M.view == UV_COUNTRIES) {
			if (M.kb_open) {
				int cols = UI_KB_COLS, k = M.kb_sel;
				int nrows_letters = (26 + cols - 1) / cols;
				if (stepr) k = k < 26 ? (k % cols == cols - 1 || k == 25 ? k - (k % cols) : k + 1) : (k == 28 ? 26 : k + 1);
				if (stepl) k = k < 26 ? (k % cols == 0 ? (k + cols - 1 > 25 ? 25 : k + cols - 1) : k - 1) : (k == 26 ? 28 : k - 1);
				if (stepdn) { if (k < 26) { k += cols; if (k > 25) k = 26 + ((k - cols) % cols) * 3 / cols; } else k = (k - 26) * 3; }
				if (stepup) { if (k >= 26) { k = (nrows_letters - 1) * cols + (k - 26) * 2; if (k > 25) k = 25; } else if (k >= cols) k -= cols; }
				M.kb_sel = k;
				if (P(0)) {
					size_t ql = strlen(query);
					if (k < 26) { if (ql < sizeof query - 1) { query[ql] = 'A' + k; query[ql + 1] = 0; } }
					else if (k == 26) { if (ql && ql < sizeof query - 1) { query[ql] = ' '; query[ql + 1] = 0; } }
					else if (k == 27) { if (ql) query[ql - 1] = 0; }
					else M.kb_open = false;
					apply_filter(); M.c_sel = M.c_top = 0;
				}
				if (P(3)) { size_t ql = strlen(query); if (ql) query[ql - 1] = 0; apply_filter(); M.c_sel = M.c_top = 0; }
				if (P(1) || P(4)) M.kb_open = false;
			} else {
				if (P(1)) M.view = UV_MAP;
				if (P(3)) { M.kb_open = true; M.kb_sel = 0; }
				int rows = ui_rows(&M);
				list_nav(&M.c_sel, &M.c_top, ncfilter, rows, stepup, stepdn, P(6), P(7));
				if (P(0) && ncfilter > 0) {
					M.cur_country = cfilter[M.c_sel];
					M.city_idx = country_places + M.cur_country->first; M.ncity = M.cur_country->nplaces; M.city_sel = M.city_top = 0;
					M.view = UV_CITIES;
					if (M.ncity) fly_to(g_places[M.city_idx[0]], 2.5f);
				}
			}
			M.clist = cfilter; M.nclist = ncfilter;
		} else if (M.view == UV_CITIES) {
			if (P(1)) M.view = UV_COUNTRIES;
			int rows = ui_rows(&M);
			if (list_nav(&M.city_sel, &M.city_top, M.ncity, rows, stepup, stepdn, P(6), P(7))) fly_to(g_places[M.city_idx[M.city_sel]], M.zoom);
			if (P(0) && M.ncity > 0) open_stations(M.city_idx[M.city_sel], UV_CITIES);
		} else if (M.view == UV_MENU) {
			if (P(1)) M.view = UV_MAP;
			int top = 0;
			list_nav(&M.menu_sel, &top, UI_MENU_N, UI_MENU_N, stepup, stepdn, false, false);
			int dir = stepr ? 1 : (stepl ? -1 : 0);
			/* hold left/right on colour sliders for fast changes */
			if (!dir && (D(12) || D(13)) && (M.menu_sel == MENU_R || M.menu_sel == MENU_G || M.menu_sel == MENU_B)) dir = D(13) ? 3 : -3;
			if (dir) {
				int *c = M.menu_sel == MENU_R ? &M.ar : (M.menu_sel == MENU_G ? &M.ag : (M.menu_sel == MENU_B ? &M.ab : NULL));
				if (c) { *c += dir * (abs(dir) == 1 ? 5 : 1); if (*c < 0) *c = 0; if (*c > 255) *c = 255; save_timer = 90; uidirty = true; }
				if (M.menu_sel == MENU_THEME && (stepl || stepr)) { M.theme = (M.theme + UI_NTHEMES + (stepr ? 1 : -1)) % UI_NTHEMES; save_timer = 30; }
				if (M.menu_sel == MENU_TLS && (stepl || stepr)) { g_tls_verify = !g_tls_verify; save_timer = 30; }
			}
			if (P(0)) {
				switch (M.menu_sel) {
				case MENU_FAVS: M.view = UV_FAVS; M.fav_sel = M.fav_top = 0; break;
				case MENU_COUNTRIES: M.view = UV_COUNTRIES; apply_filter(); M.c_sel = M.c_top = 0; break;
				case MENU_THEME: M.theme = (M.theme + 1) % UI_NTHEMES; save_timer = 30; break;
				case MENU_TLS: g_tls_verify = !g_tls_verify; save_timer = 30; break;
				case MENU_RANDOM: if (M.nplaces) { M.view = UV_MAP; fly_to(g_places[rand() % M.nplaces], 5); } break;
				default: break;
				}
			}
		}
		M.nfav = nfav;

		/* zoom works in every view */
		float zf = 1.f;
		if (D(7) && (M.view == UV_MAP)) zf *= 1.03f;
		if (D(6) && (M.view == UV_MAP)) zf /= 1.03f;
		if (ry) zf *= 1.f - ry / 128.f * 0.035f;
		if (zf != 1.f) { M.zoom *= zf; if (M.zoom < 1) M.zoom = 1; if (M.zoom > 40) M.zoom = 40; anim_cam = false; moved = true; }

		/* camera animation */
		if (anim_cam) {
			float dl = tgt_lon - M.lon; if (dl > 180) dl -= 360; if (dl < -180) dl += 360;
			M.lon += dl * 0.12f; M.lat += (tgt_lat - M.lat) * 0.12f; M.zoom *= powf(tgt_zoom / M.zoom, 0.12f);
			if (M.lon > 180) M.lon -= 360; if (M.lon < -180) M.lon += 360;
			if (fabsf(dl) < 0.005f && fabsf(tgt_lat - M.lat) < 0.005f && fabsf(M.zoom / tgt_zoom - 1) < 0.003f) anim_cam = false;
			moved = true;
		}
		if (moved) { save_timer = 120; still = 0; }
		else still++;

		/* hover only matters on the map; panel follows it */
		int hv = M.view == UV_MAP ? ui_pick(&M) : -1;
		if (hv != last_hover) { last_hover = hv; M.hover = hv; uidirty = true; }
		if (still == 10 && M.view == UV_MAP) uidirty = true;     /* refresh "Cerca de aquí" once the globe stops */
		if (M.nst != last_nst) { last_nst = M.nst; uidirty = true; }

		/* player status */
		M.pstate = player_state(); M.pfmt = player_format(); M.pmsg = player_message(); M.volume = player_get_volume(); M.tls = g_tls_verify;
		if (M.pstate != last_state) { last_state = M.pstate; uidirty = true; }
		if (M.volume != last_vol) { last_vol = M.volume; uidirty = true; }
		if (M.toast_t > 0) M.toast_t--;
		if (save_timer > 0 && --save_timer == 0) conf_save(M);
		if (uidirty) M.ui_dirty = true;

		/* the GPU renders every vblank (60 Hz); flip() waits for vsync */
		M.frame++;
		if (M.loading == 1 && (M.frame % 60) == 0) rg_log("main alive frame %u (loading)", M.frame);
		static int logframes = 0;
		bool lg = M.nplaces > 0 && logframes < 3;
		if (lg) rg_log("frame %d with places: building", logframes);
		ui_frame(&M);
		if (lg) rg_log("frame %d: submitted, flipping", logframes);
		flip();
		if (lg) { rg_log("frame %d: flip ok", logframes); logframes++; }

		fps_frames++;
		u64 t = now_us();
		if (t - fps_t0 >= 500000) { M.fps = fps_frames * 1e6f / (float)(t - fps_t0); fps_frames = 0; fps_t0 = t; }
	}

	conf_save(M);
	player_shutdown();
	http_shutdown();
	return 0;
}
