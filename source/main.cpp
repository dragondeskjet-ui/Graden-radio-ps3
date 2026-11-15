#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <io/pad.h>
#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "rsxutil.h"
#include "gfx.h"
#include "ui.h"
#include "http.h"
#include "api.h"
#include "player.h"

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

/* ---------- background job ---------- */
enum { J_NONE, J_PLACES, J_STATIONS };
static volatile int job_kind = J_NONE, job_state = 0;  /* 1 running, 2 ok, 3 failed */
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

/* ---------- config (last view, volume) ---------- */
static void conf_save(const UiModel &m)
{
	FILE *f = fopen(CONF_FILE, "w"); if (!f) return;
	fprintf(f, "%f %f %f %f %d\n", m.lon, m.lat, m.zoom, player_get_volume(), (int)g_tls_verify);
	fclose(f);
}
static void conf_load(UiModel &m)
{
	FILE *f = fopen(CONF_FILE, "r"); if (!f) return;
	float lo, la, z, v; int t;
	if (fscanf(f, "%f %f %f %f %d", &lo, &la, &z, &v, &t) == 5) { m.lon = lo; m.lat = la; m.zoom = z < 1 ? 1 : z; player_set_volume(v); g_tls_verify = t; }
	fclose(f);
}

/* ---------- state ---------- */
static UiModel M;
static char now_name[96], now_place[96], now_id[16];
static float tgt_lon, tgt_lat, tgt_zoom; static bool anim_cam = false;

static void toast(const char *t) { static char buf[96]; snprintf(buf, sizeof buf, "%s", t); M.toast = buf; M.toast_t = 90; }
static float pxdeg() { return (float)display_width / 360.f * M.zoom; }

static void find_hover()
{
	M.hover = -1;
	if (!g_places || M.loading == 1) return;
	float sc = pxdeg(), best = 18.f * 18.f * (display_height >= 900 ? 2.25f : 1.f), lim = 30.f;
	for (int i = 0; i < g_nplaces; i++) {
		float dx = (g_places[i].lon - M.lon) * sc, dy = (g_places[i].lat - M.lat) * sc;
		if (dx > lim || dx < -lim || dy > lim || dy < -lim) continue;
		float d = dx * dx + dy * dy;
		if (d < best || (d < best * 1.5f && M.hover >= 0 && g_places[i].size > g_places[M.hover].size * 3)) { best = d; M.hover = i; }
	}
}

static void play(const char *id, const char *name, const char *place)
{
	char url[200];
	api_stream_url(id, url, sizeof url);
	snprintf(now_id, sizeof now_id, "%s", id); snprintf(now_name, sizeof now_name, "%s", name); snprintf(now_place, sizeof now_place, "%s", place);
	player_play(url);
}

static void open_stations(int pi)
{
	M.panel_place = &g_places[pi];
	M.st_sel = M.st_top = 0; M.nst = 0; g_nstations = 0; M.error = NULL;
	job_start(J_STATIONS, g_places[pi].id);
	M.loading = 2; M.view = UV_STATIONS;
	/* shift the map so the city sits in the visible area left of the panel */
	tgt_zoom = M.zoom < 12 ? 12 : M.zoom;
	float sc = display_width / 360.f * tgt_zoom;
	tgt_lat = g_places[pi].lat; tgt_lon = g_places[pi].lon + (display_width * 0.21f) / sc;
	anim_cam = true;
}

static void scroll_fix(int sel, int *top, int n)
{
	int rows = ui_list_rows();
	if (sel < *top) *top = sel;
	if (sel >= *top + rows) *top = sel - rows + 1;
	if (*top > n - rows) *top = n - rows > 0 ? n - rows : 0;
	if (*top < 0) *top = 0;
}

int main(int, const char **)
{
	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	init_screen(host_addr, HOST_SIZE);
	ioPadInit(7);
	atexit(program_exit_callback);
	sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);
	setRenderTarget(curr_fb);
	ui_init(display_width, display_height);

	memset(&M, 0, sizeof M);
	M.view = UV_MAP; M.lon = -30; M.lat = 15; M.zoom = 1.f; M.hover = -1;
	M.stations = g_stations; M.favs = favs; M.is_fav = is_fav; M.map_dirty = true;
	M.now_name = now_name; M.now_place = now_place; M.now_id = now_id;

	bool net_ok = http_init();
	bool audio_ok = player_init();
	favs_load(); conf_load(M);
	if (net_ok) { job_start(J_PLACES, NULL); M.loading = 1; }
	else M.error = "Red no disponible. Revisa la conexión de la PS3.";
	if (!audio_ok) toast("No se pudo abrir la salida de audio");

	padInfo pinfo; padData pd;
	u32 prev = 0; int rep = 0; int save_timer = 0;

	while (running) {
		sysUtilCheckCallback();
		ioPadGetInfo(&pinfo);
		u32 b = 0; int lx = 0, ly = 0, ry = 0;
		for (int i = 0; i < MAX_PADS; i++) {
			if (!pinfo.status[i]) continue;
			ioPadGetData(i, &pd);
			b = (pd.BTN_CROSS << 0) | (pd.BTN_CIRCLE << 1) | (pd.BTN_TRIANGLE << 2) | (pd.BTN_SQUARE << 3) |
			    (pd.BTN_START << 4) | (pd.BTN_SELECT << 5) | (pd.BTN_L1 << 6) | (pd.BTN_R1 << 7) | (pd.BTN_L2 << 8) | (pd.BTN_R2 << 9) |
			    (pd.BTN_UP << 10) | (pd.BTN_DOWN << 11) | (pd.BTN_LEFT << 12) | (pd.BTN_RIGHT << 13) | (pd.BTN_L3 << 14);
			int hx = (int)pd.ANA_L_H - 128, hy = (int)pd.ANA_L_V - 128, rv = (int)pd.ANA_R_V - 128;
			lx = abs(hx) > 28 ? hx : 0; ly = abs(hy) > 28 ? hy : 0; ry = abs(rv) > 28 ? rv : 0;
			break;
		}
		u32 pr = b & ~prev; prev = b;
		#define P(bit) (pr & (1u << (bit)))
		#define D(bit) (b & (1u << (bit)))
		bool stepup = false, stepdn = false;
		if (D(10) || D(11) || ly) { if (rep == 0 || (rep > 18 && rep % 3 == 0)) { stepup = D(10) || ly < -60; stepdn = D(11) || ly > 60; } rep++; } else rep = 0;
		bool redraw = false;

		/* background jobs */
		if (M.loading && job_state != 1) {
			if (job_kind == J_PLACES) {
				if (job_state == 2) { M.places = g_places; M.nplaces = g_nplaces; M.error = NULL; M.map_dirty = true; find_hover(); char t[64]; snprintf(t, sizeof t, "%d ciudades con radio", g_nplaces); toast(t); }
				else M.error = job_err;
			} else if (job_kind == J_STATIONS) { if (job_state == 2) { M.nst = g_nstations; M.error = NULL; } else M.error = job_err; }
			M.loading = 0; redraw = true;
		}

		/* global */
		if (P(2)) { player_stop(); toast("Detenido"); }
		if (P(5)) { g_tls_verify = !g_tls_verify; toast(g_tls_verify ? "Verificación TLS activada" : "Verificación TLS desactivada"); }
		if (D(8) || D(9)) { player_set_volume(player_get_volume() + (D(9) ? 0.01f : -0.01f)); redraw = true; save_timer = 120; }
		if (P(4)) { M.view = M.view == UV_FAVS ? UV_MAP : UV_FAVS; M.fav_sel = 0; M.fav_top = 0; redraw = true; }

		if (M.view == UV_MAP) {
			if (M.error && !M.loading && P(0) && net_ok) { job_start(J_PLACES, NULL); M.loading = 1; M.error = NULL; redraw = true; }
			float sc = pxdeg(), spd = 9.f / sc;
			float mx = (D(13) ? 1.f : 0.f) - (D(12) ? 1.f : 0.f) + lx / 100.f;
			float my = (D(10) ? 1.f : 0.f) - (D(11) ? 1.f : 0.f) - ly / 100.f;
			if (mx || my) {
				M.lon += mx * spd; M.lat += my * spd; anim_cam = false;
				if (M.lon > 180) M.lon -= 360; if (M.lon < -180) M.lon += 360;
				if (M.lat > 85) M.lat = 85; if (M.lat < -85) M.lat = -85;
				M.map_dirty = true; save_timer = 120;
			}
			float zf = 1.f;
			if (D(7)) zf *= 1.035f; if (D(6)) zf /= 1.035f;
			if (ry) zf *= 1.f - ry / 128.f * 0.04f;
			if (zf != 1.f) { M.zoom *= zf; if (M.zoom < 1) M.zoom = 1; if (M.zoom > 120) M.zoom = 120; M.map_dirty = true; anim_cam = false; save_timer = 120; }
			if (P(14) && M.nplaces) {             /* L3: random city */
				int i = rand() % M.nplaces;
				tgt_lon = g_places[i].lon; tgt_lat = g_places[i].lat; tgt_zoom = M.zoom < 14 ? 14 : M.zoom; anim_cam = true;
				toast("Viajando a un lugar al azar…");
			}
			if (M.map_dirty) find_hover();
			if (P(0) && M.hover >= 0 && !M.loading) open_stations(M.hover);
		} else if (M.view == UV_STATIONS) {
			if (P(1)) { M.view = UV_MAP; anim_cam = false; find_hover(); redraw = true; }
			if (M.nst > 0) {
				int ps = M.st_sel;
				if (stepup) M.st_sel--; if (stepdn) M.st_sel++;
				if (P(6)) M.st_sel -= ui_list_rows(); if (P(7)) M.st_sel += ui_list_rows();
				if (M.st_sel < 0) M.st_sel = 0; if (M.st_sel >= M.nst) M.st_sel = M.nst - 1;
				if (ps != M.st_sel) redraw = true;
				scroll_fix(M.st_sel, &M.st_top, M.nst);
				const Station &s = g_stations[M.st_sel];
				char place[96]; snprintf(place, sizeof place, "%s, %s", M.panel_place->title, M.panel_place->country);
				if (P(0)) { if (audio_ok) play(s.id, s.name, place); redraw = true; }
				if (P(3)) { toast(fav_toggle(s.id, s.name, place) ? "Añadido a favoritos" : "Quitado de favoritos"); }
			}
		} else {
			if (P(1)) { M.view = UV_MAP; redraw = true; }
			if (nfav > 0) {
				int ps = M.fav_sel;
				if (stepup) M.fav_sel--; if (stepdn) M.fav_sel++;
				if (P(6)) M.fav_sel -= ui_list_rows(); if (P(7)) M.fav_sel += ui_list_rows();
				if (M.fav_sel < 0) M.fav_sel = 0; if (M.fav_sel >= nfav) M.fav_sel = nfav - 1;
				if (ps != M.fav_sel) redraw = true;
				scroll_fix(M.fav_sel, &M.fav_top, nfav);
				Fav f = favs[M.fav_sel];
				if (P(0)) { if (audio_ok) play(f.id, f.name, f.place); redraw = true; }
				if (P(3)) { fav_toggle(f.id, f.name, f.place); if (M.fav_sel >= nfav && nfav) M.fav_sel = nfav - 1; toast("Quitado de favoritos"); }
			}
			M.nfav = nfav;
		}
		M.nfav = nfav;

		/* camera animation */
		if (anim_cam) {
			float dl = tgt_lon - M.lon; if (dl > 180) dl -= 360; if (dl < -180) dl += 360;
			M.lon += dl * 0.12f; M.lat += (tgt_lat - M.lat) * 0.12f; M.zoom *= powf(tgt_zoom / M.zoom, 0.12f);
			if (fabsf(dl) < 0.01f && fabsf(tgt_lat - M.lat) < 0.01f && fabsf(M.zoom / tgt_zoom - 1) < 0.005f) { anim_cam = false; if (M.view == UV_MAP) find_hover(); }
			M.map_dirty = true;
		}

		/* player status */
		M.pstate = player_state(); M.pfmt = player_format(); M.pmsg = player_message(); M.volume = player_get_volume(); M.tls = g_tls_verify;
		if (M.toast_t > 0) M.toast_t--;
		if (save_timer > 0 && --save_timer == 0) conf_save(M);

		static int last_state = -1; static unsigned slow = 0;
		if (M.pstate != last_state) { last_state = M.pstate; redraw = true; }
		if (pr) redraw = true;
		/* animate at ~30 fps when something moves, otherwise only on change */
		if (M.map_dirty || redraw || (ui_animating(&M) && (++slow & 1))) {
			M.frame++;
			ui_render(&M);
			gfx_present(color_buffer[curr_fb]);
			flip();
		} else usleep(16000);
	}

	conf_save(M);
	player_shutdown();
	http_shutdown();
	return 0;
}
