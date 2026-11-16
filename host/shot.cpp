// Host-side screenshot renderer: runs the real UI code with sample data (no network, no PS3).
volatile int rg_stage; volatile unsigned rg_wd_frame;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ui.h"
#include "gfx.h"
#include "gpu.h"
#include "player.h"
#include "landmask.h"
#include <time.h>
#include <algorithm>

Place *g_places; int g_nplaces; Station g_stations[MAX_STATIONS]; int g_nstations;

static unsigned rng = 12345; static float frand() { rng = rng * 1103515245 + 12345; return ((rng >> 8) & 0xFFFFFF) / 16777216.f; }
static int island(float lat, float lon) {
	int r = (int)((90 - lat) / 180 * LAND_H), c = (int)((lon + 180) / 360 * LAND_W);
	if (r < 0 || r >= LAND_H || c < 0 || c >= LAND_W) return 0;
	int x = 0, v = 0; for (unsigned i = land_row[r]; i < land_row[r + 1]; i++) { x += land_runs[i]; if (c < x) return v; v ^= 1; } return 0;
}
struct Named { const char *t, *c; float lat, lon; int n; };
static Named named[] = {
	{"Santarém, PA","Brasil",-2.44f,-54.71f,14},{"Belém","Brasil",-1.45f,-48.49f,38},{"Manaus","Brasil",-3.12f,-60.02f,31},
	{"Itaituba","Brasil",-4.27f,-55.98f,5},{"Óbidos","Brasil",-1.91f,-55.52f,3},{"Alenquer","Brasil",-1.94f,-54.74f,4},
	{"Monte Alegre","Brasil",-2.0f,-54.07f,3},{"Oriximiná","Brasil",-1.76f,-55.86f,4},{"Macapá","Brasil",0.03f,-51.07f,17},
	{"Madrid","España",40.42f,-3.70f,120},{"Ciudad de México","México",19.43f,-99.13f,140},{"Buenos Aires","Argentina",-34.6f,-58.38f,160},
	{"Bogotá","Colombia",4.71f,-74.07f,90},{"Lima","Perú",-12.05f,-77.04f,85},{"Lisboa","Portugal",38.72f,-9.14f,70},
};
u32 *gpu_host_frame();
int main(int argc, char **argv)
{
	const int W = 1280, H = 720;
	gpu_init(W, H);
	if (!gpu_load_earth("../release/USRDIR/earth.565")) gpu_load_earth("release/USRDIR/earth.565");
	ui_init(W, H);
	int N = 7000; g_places = (Place *)calloc(N + 32, sizeof(Place));
	for (unsigned i = 0; i < sizeof named / sizeof *named; i++) { Place &p = g_places[g_nplaces++]; p.lat = named[i].lat; p.lon = named[i].lon; snprintf(p.title, sizeof p.title, "%s", named[i].t); snprintf(p.country, sizeof p.country, "%s", named[i].c); p.size = named[i].n; snprintf(p.id, sizeof p.id, "N%u", i); }
	while (g_nplaces < N) {
		float lat = -55 + frand() * 125, lon = -180 + frand() * 360;
		float w = cosf(lat * 3.14159f / 180); if (lat > 60) w *= 0.3f; if (frand() > w) continue;
		if (!island(lat, lon)) continue;
		Place &p = g_places[g_nplaces++]; p.lat = lat; p.lon = lon; float s = frand(); p.size = (u16)(1 + s * s * s * 50); snprintf(p.title, sizeof p.title, "Ciudad %d", g_nplaces); snprintf(p.country, sizeof p.country, "Ejemplo");
	}
	const char *names[][2] = { {"Rádio Exemplo FM","Notícias e música regional"},{"Web Rádio Amostra","Pop brasileiro"},{"Rádio Comunitária Demo","Comunidade"},
		{"Ondas do Rio (demo)","Música popular"},{"Rádio Teste AM","Esportes e jornalismo"},{"Estação Demo 104","Sertanejo"},{"Rádio Amostra Gospel","Religiosa"},
		{"Demo Hits FM","Top 40"},{"Rádio Floresta (demo)","MPB"},{"Rádio Escola Demo","Educativa"},{"Tapajós Demo Web","Variedades"},{"Clube Demo FM","Forró e brega"},
		{"Rádio Prova 2","Notícias"},{"Rádio Exemplo Rural","Agro"} };
	g_nstations = 14;
	for (int i = 0; i < g_nstations; i++) { snprintf(g_stations[i].id, 16, "S%d", i); snprintf(g_stations[i].name, 56, "%s", names[i][0]); snprintf(g_stations[i].sub, 40, "%s", names[i][1]); }
	static Fav favs[] = { {"S0","Rádio Exemplo FM","Santarém, PA, Brasil"},{"F1","Radio Demo Madrid","Madrid, España"},{"F2","Estación Prueba 99","Bogotá, Colombia"},{"F3","Radio Muestra Sur","Buenos Aires, Argentina"} };
	static auto isfav = [](const char *id) { return !strcmp(id, "S0") || !strcmp(id, "S8"); };
	ui_set_places(g_places, g_nplaces);

	UiModel m; memset(&m, 0, sizeof m);
	m.places = g_places; m.nplaces = g_nplaces; m.hover = -1; m.stations = g_stations; m.nst = g_nstations;
	m.favs = favs; m.nfav = 4; m.is_fav = isfav; m.tls = true; m.volume = 0.8f; m.frame = 37; m.fps = 60;
	m.ar = 0xFF; m.ag = 0xC2; m.ab = 0x4B;
	auto save = [&](const char *fn) {
		u32 *b = gpu_host_frame(); FILE *f = fopen(fn, "wb"); fprintf(f, "P6 %d %d 255\n", W, H);
		for (int i = 0; i < W * H; i++) { unsigned char c[3] = { (unsigned char)(b[i] >> 16), (unsigned char)(b[i] >> 8), (unsigned char)b[i] }; fwrite(c, 1, 3, f); }
		fclose(f);
	};
	auto shot = [&](const char *fn) { m.ui_dirty = true; ui_frame(&m); save(fn); };
	// 1: dark glass, world, playing
	m.theme = 0; m.view = UV_MAP; m.lon = -62; m.lat = 8; m.zoom = 1.0f;
	m.pstate = P_PLAYING; m.pfmt = "AAC"; m.now_name = "Radio Demo Madrid"; m.now_place = "Madrid, España"; m.now_id = "F1";
	shot("shot1.ppm");
	// 2: light glass, hover Santarém
	m.theme = 1; m.lon = -54.71f; m.lat = -2.44f; m.zoom = 9; m.hover = 0; m.pstate = P_IDLE; m.ar = 0x0A; m.ag = 0x84; m.ab = 0xFF;
	shot("shot2.ppm");
	// 3: aero, stations
	m.theme = 2; m.view = UV_STATIONS; m.panel_place = &g_places[0]; m.st_sel = 2; m.st_top = 0; m.lon = -54.71f; m.lat = -2.44f; m.zoom = 4; m.ar = 0x1F; m.ag = 0xB2; m.ab = 0x5A;
	m.pstate = P_PLAYING; m.pfmt = "MP3"; m.now_name = "Rádio Exemplo FM"; m.now_place = "Santarém, PA, Brasil"; m.now_id = "S0";
	m.toast = "Añadido a favoritos"; m.toast_t = 40;
	shot("shot3.ppm");
	// 4: custom RGB, menu
	m.theme = 3; m.view = UV_MENU; m.menu_sel = 3; m.toast_t = 0; m.ar = 0x8E; m.ag = 0x3B; m.ab = 0xD8; m.lon = -10; m.lat = 25; m.zoom = 1.6f;
	m.pstate = P_BUFFERING; m.pmsg = "Cargando buffer..."; m.now_name = "Radio Demo Madrid"; m.now_place = "Madrid, España"; m.now_id = "F1";
	shot("shot4.ppm");
	// 5: dark glass, country search with keyboard
	static Country cs[] = { {"Argentina",120,900,0},{"Armenia",4,20,0},{"Aruba",1,6,0},{"Australia",210,1400,0},{"Austria",60,300,0},{"Azerbaijan",5,30,0} };
	static const Country *cl[6] = { &cs[0],&cs[1],&cs[2],&cs[3],&cs[4],&cs[5] };
	m.theme = 0; m.ar = 0xFF; m.ag = 0xC2; m.ab = 0x4B;
	m.view = UV_COUNTRIES; m.clist = cl; m.nclist = 6; m.c_sel = 0; m.query = "AR"; m.kb_open = true; m.kb_sel = 17;
	m.pstate = P_PLAYING; m.pfmt = "AAC"; m.lon = -60; m.lat = -25; m.zoom = 1.3f;
	shot("shot5.ppm");
	return 0;
}
