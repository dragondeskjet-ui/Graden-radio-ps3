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
#include "http.h"
#include "api.h"
#include "player.h"

SYS_PROCESS_PARAM(1001, 0x100000);

static volatile int running = 1;

extern "C" {
static void program_exit_callback()
{
	sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	gcmSetWaitFlip(context);
	rsxFinish(context, 1);
}
static void sysutil_exit_callback(u64 status, u64 param, void *usrdata)
{
	if (status == SYSUTIL_EXIT_GAME) running = 0;
}
}

/* ---------- colours ---------- */
#define C_BG     0x0B1220
#define C_GRID   0x16233A
#define C_DOT    0x2E8B6A
#define C_DOT_B  0x58E0A8
#define C_TEXT   0xE6EDF7
#define C_DIM    0x8A97AD
#define C_ACC    0xF2C14E
#define C_ERR    0xFF6B6B
#define C_PANEL  0x111C30
#define C_SEL    0x24407A

/* ---------- background job (one at a time) ---------- */
enum { J_NONE, J_PLACES, J_STATIONS };
static volatile int job_kind = J_NONE;
static volatile int job_state = 0;       /* 0 idle, 1 running, 2 ok, 3 failed */
static char job_err[128];
static char job_arg[16];
static sys_ppu_thread_t job_th;
static int job_started = 0;

static void job_entry(void *)
{
	bool ok = false;
	job_err[0] = 0;
	if (job_kind == J_PLACES) ok = api_load_places(job_err, sizeof job_err);
	else if (job_kind == J_STATIONS) ok = api_load_stations(job_arg, job_err, sizeof job_err);
	job_state = ok ? 2 : 3;
	sysThreadExit(0);
}

static void job_start(int kind, const char *arg)
{
	if (job_started) { u64 r; sysThreadJoin(job_th, &r); job_started = 0; }
	job_kind = kind; job_state = 1;
	snprintf(job_arg, sizeof job_arg, "%s", arg ? arg : "");
	if (sysThreadCreate(&job_th, job_entry, NULL, 1001, 0x80000, THREAD_JOINABLE, (char *)"rg_job") == 0) job_started = 1;
	else { job_state = 3; snprintf(job_err, sizeof job_err, "no se pudo crear hilo"); }
}

/* ---------- app state ---------- */
enum View { V_MAP, V_STATIONS };
static View view = V_MAP;
static float cLon = 10.f, cLat = 30.f, zoom = 1.f;
static int hover = -1;                    /* index of place under cursor */
static int place_sel = -1;                /* place whose stations are shown */
static int st_sel = 0, st_top = 0;
static int playing_idx = -1;
static char playing_name[96] = "";
static char playing_place[96] = "";
static int dirty = 2;
static int S = 1;                         /* text scale */

static float pxdeg() { return (float)display_width / 360.f * zoom; }

static void find_hover()
{
	hover = -1;
	if (!g_places) return;
	float sc = pxdeg();
	float best = (16.f * S) * (16.f * S);
	for (int i = 0; i < g_nplaces; i++) {
		float dx = (g_places[i].lon - cLon) * sc, dy = (g_places[i].lat - cLat) * sc;
		if (dx > 20 * S || dx < -20 * S || dy > 20 * S || dy < -20 * S) continue;
		float d = dx * dx + dy * dy;
		if (d < best) { best = d; hover = i; }
	}
}

static void draw_text_c(int cx, int y, const char *t, u32 col, int sc) { gfx_text(cx - gfx_text_width(t, sc) / 2, y, t, col, sc); }

static void draw_bar()
{
	int h = 44 * S;
	int y = display_height - h;
	gfx_rect(0, y, display_width, h, C_PANEL);
	PState st = player_state();
	char line[200];
	if (st == P_IDLE) snprintf(line, sizeof line, "Sin reproducir");
	else if (st == P_ERROR) snprintf(line, sizeof line, "Error: %s", player_message());
	else snprintf(line, sizeof line, "%s%s  -  %s", st == P_PLAYING ? "> " : "... ", playing_name, playing_place);
	gfx_text(12 * S, y + 4 * S, line, st == P_ERROR ? C_ERR : C_ACC, S);
	char vol[40]; snprintf(vol, sizeof vol, "Vol %d%%  TLS %s", (int)(player_get_volume() * 100 + 0.5f), g_tls_verify ? "ok" : "OFF");
	gfx_text(display_width - gfx_text_width(vol, S) - 12 * S, y + 4 * S, vol, C_DIM, S);
	const char *help = view == V_MAP
		? "Stick/D-pad: mover  L1/R1: zoom  X: emisoras  Triangulo: parar  L2/R2: volumen  Select: TLS"
		: "Arriba/Abajo: elegir  X: reproducir  O: volver al mapa  Triangulo: parar  L2/R2: volumen";
	gfx_text(12 * S, y + 22 * S, help, C_DIM, S);
}

static void draw_map()
{
	gfx_clear(C_BG);
	float sc = pxdeg();
	int W = display_width, H = display_height;
	/* graticule every 30 degrees */
	for (int lon = -180; lon <= 180; lon += 30) { int x = (int)(W / 2 + (lon - cLon) * sc); gfx_rect(x, 0, 1, H, C_GRID); }
	for (int lat = -90; lat <= 90; lat += 30) { int y = (int)(H / 2 - (lat - cLat) * sc); gfx_rect(0, y, W, 1, C_GRID); }
	if (g_places) {
		for (int i = 0; i < g_nplaces; i++) {
			int x = (int)(W / 2 + (g_places[i].lon - cLon) * sc);
			int y = (int)(H / 2 - (g_places[i].lat - cLat) * sc);
			if (x < 0 || y < 0 || x >= W || y >= H) continue;
			int r = g_places[i].size > 20 ? 2 : 1;
			if (zoom > 6) r++;
			gfx_rect(x - r / 2, y - r / 2, r * S, r * S, g_places[i].size > 20 ? C_DOT_B : C_DOT);
		}
	}
	/* crosshair */
	int cx = W / 2, cy = H / 2;
	gfx_rect(cx - 14 * S, cy, 10 * S, S, C_TEXT); gfx_rect(cx + 5 * S, cy, 10 * S, S, C_TEXT);
	gfx_rect(cx, cy - 14 * S, S, 10 * S, C_TEXT); gfx_rect(cx, cy + 5 * S, S, 10 * S, C_TEXT);
	if (hover >= 0) {
		Place &p = g_places[hover];
		char t[120]; snprintf(t, sizeof t, "%s, %s  (%d emisoras)", p.title, p.country, p.size);
		int w = gfx_text_width(t, S) + 16 * S;
		gfx_rect(cx - w / 2, cy + 22 * S, w, 22 * S, C_PANEL);
		draw_text_c(cx, cy + 25 * S, t, C_TEXT, S);
	}
	gfx_text(12 * S, 8 * S, "RADIO GARDEN PS3", C_ACC, S * 2 > 2 ? 2 : S);
	if (job_kind == J_PLACES && job_state == 1) draw_text_c(W / 2, H / 3, "Cargando mapa de emisoras... (puede tardar)", C_TEXT, S);
	if (job_kind == J_PLACES && job_state == 3) {
		draw_text_c(W / 2, H / 3, "No se pudo cargar el mapa", C_ERR, S);
		draw_text_c(W / 2, H / 3 + 24 * S, job_err, C_DIM, S);
		draw_text_c(W / 2, H / 3 + 48 * S, "X: reintentar   Select: alternar verificacion TLS", C_TEXT, S);
	}
	if (job_kind == J_STATIONS && job_state == 1) draw_text_c(W / 2, 60 * S, "Cargando emisoras...", C_TEXT, S);
	if (job_kind == J_STATIONS && job_state == 3) draw_text_c(W / 2, 60 * S, job_err, C_ERR, S);
}

static void draw_stations()
{
	gfx_clear(C_BG);
	int W = display_width, H = display_height;
	Place &p = g_places[place_sel];
	char t[120]; snprintf(t, sizeof t, "%s, %s", p.title, p.country);
	gfx_text(16 * S, 10 * S, t, C_ACC, 2 * S > 2 ? 2 : S);
	int row = 24 * S, top = 50 * S, avail = H - top - 50 * S;
	int rows = avail / row;
	if (st_sel < st_top) st_top = st_sel;
	if (st_sel >= st_top + rows) st_top = st_sel - rows + 1;
	for (int i = 0; i < rows && st_top + i < g_nstations; i++) {
		int idx = st_top + i, y = top + i * row;
		if (idx == st_sel) gfx_rect(8 * S, y - 2 * S, W - 16 * S, row, C_SEL);
		u32 col = idx == playing_idx && place_sel >= 0 ? C_ACC : C_TEXT;
		gfx_text(16 * S, y + 2 * S, g_stations[idx].name, col, S);
		if (g_stations[idx].sub[0]) gfx_text(W / 2, y + 2 * S, g_stations[idx].sub, C_DIM, S);
	}
	if (job_kind == J_STATIONS && job_state == 1) draw_text_c(W / 2, H / 2, "Cargando emisoras...", C_TEXT, S);
}

static void render()
{
	if (view == V_MAP) draw_map(); else draw_stations();
	draw_bar();
	gfx_present(color_buffer[curr_fb]);
	flip();
}

static void open_stations(int pi)
{
	place_sel = pi; st_sel = st_top = 0; g_nstations = 0;
	job_start(J_STATIONS, g_places[pi].id);
	view = V_STATIONS;
	dirty = 2;
}

int main(int argc, const char *argv[])
{
	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	init_screen(host_addr, HOST_SIZE);
	ioPadInit(7);
	atexit(program_exit_callback);
	sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);
	setRenderTarget(curr_fb);
	gfx_init(display_width, display_height);
	S = display_height >= 1000 ? 2 : 1;

	bool net_ok = http_init();
	bool audio_ok = player_init();
	if (net_ok) job_start(J_PLACES, NULL);
	else { job_kind = J_PLACES; job_state = 3; snprintf(job_err, sizeof job_err, "Red no disponible"); }
	if (!audio_ok) { job_kind = J_NONE; }

	padInfo pinfo; padData pdata;
	u32 prev_btn = 0;
	int held_frames = 0;
	int last_job_state = -1;
	PState last_ps = P_IDLE;

	while (running) {
		sysUtilCheckCallback();
		ioPadGetInfo(&pinfo);

		/* input (first connected pad) */
		bool x = false, o = false, tri = false, l1 = false, r1 = false, l2 = false, r2 = false, sel = false;
		bool up = false, down = false, left = false, right = false;
		int ax = 0, ay = 0;
		for (int i = 0; i < MAX_PADS; i++) {
			if (!pinfo.status[i]) continue;
			ioPadGetData(i, &pdata);
			x |= pdata.BTN_CROSS; o |= pdata.BTN_CIRCLE; tri |= pdata.BTN_TRIANGLE;
			l1 |= pdata.BTN_L1; r1 |= pdata.BTN_R1; l2 |= pdata.BTN_L2; r2 |= pdata.BTN_R2; sel |= pdata.BTN_SELECT;
			up |= pdata.BTN_UP; down |= pdata.BTN_DOWN; left |= pdata.BTN_LEFT; right |= pdata.BTN_RIGHT;
			int hx = (int)pdata.ANA_L_H - 128, hy = (int)pdata.ANA_L_V - 128;
			if (abs(hx) > 24) ax += hx; if (abs(hy) > 24) ay += hy;
			break;
		}
		u32 btn = (x ? 1 : 0) | (o ? 2 : 0) | (tri ? 4 : 0) | (sel ? 8 : 0);
		u32 pressed = btn & ~prev_btn; prev_btn = btn;

		/* job completion */
		if (job_state != last_job_state) { last_job_state = job_state; dirty = 2; if (job_kind == J_PLACES && job_state == 2) find_hover(); }
		PState ps = player_state(); if (ps != last_ps) { last_ps = ps; dirty = 2; }

		if (pressed & 4) { player_stop(); playing_idx = -1; dirty = 2; }
		if (pressed & 8) { g_tls_verify = !g_tls_verify; dirty = 2; }
		if (l2 || r2) { player_set_volume(player_get_volume() + (r2 ? 0.01f : -0.01f)); dirty = 2; }

		if (view == V_MAP) {
			if (job_kind == J_PLACES && job_state == 3 && (pressed & 1)) { job_start(J_PLACES, NULL); dirty = 2; }
			float sc = pxdeg();
			float speed = 6.f / sc; /* degrees per frame at full deflection */
			float mx = (right ? 1.f : 0.f) - (left ? 1.f : 0.f) + ax / 128.f;
			float my = (up ? 1.f : 0.f) - (down ? 1.f : 0.f) - ay / 128.f;
			if (mx != 0.f || my != 0.f) {
				cLon += mx * speed * 3.f; cLat += my * speed * 3.f;
				if (cLon > 180) cLon = 180; if (cLon < -180) cLon = -180;
				if (cLat > 85) cLat = 85; if (cLat < -85) cLat = -85;
				find_hover(); dirty = 2;
			}
			if (l1 || r1) {
				zoom *= r1 ? 1.03f : 1.f / 1.03f;
				if (zoom < 1.f) zoom = 1.f; if (zoom > 80.f) zoom = 80.f;
				find_hover(); dirty = 2;
			}
			if ((pressed & 1) && hover >= 0 && job_kind != J_PLACES) open_stations(hover);
			else if ((pressed & 1) && hover >= 0 && job_state == 2) open_stations(hover);
		} else {
			bool step = false; int dir = 0;
			if (up || down) {
				held_frames++;
				if (held_frames == 1 || (held_frames > 20 && held_frames % 4 == 0)) { step = true; dir = up ? -1 : 1; }
			} else held_frames = 0;
			if (step && g_nstations > 0) { st_sel += dir; if (st_sel < 0) st_sel = 0; if (st_sel >= g_nstations) st_sel = g_nstations - 1; dirty = 2; }
			if (pressed & 2) { view = V_MAP; dirty = 2; }
			if ((pressed & 1) && job_state == 2 && g_nstations > 0) {
				char url[200];
				api_stream_url(g_stations[st_sel].id, url, sizeof url);
				snprintf(playing_name, sizeof playing_name, "%s", g_stations[st_sel].name);
				snprintf(playing_place, sizeof playing_place, "%s, %s", g_places[place_sel].title, g_places[place_sel].country);
				playing_idx = st_sel;
				if (audio_ok) player_play(url);
				dirty = 2;
			}
		}

		if (dirty > 0) { render(); dirty--; }
		else usleep(8000);
		usleep(2000);
	}

	player_shutdown();
	http_shutdown();
	return 0;
}
