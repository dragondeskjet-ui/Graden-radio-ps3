#ifndef UI_H
#define UI_H
#include "api.h"

enum UiView { UV_MAP, UV_STATIONS, UV_FAVS, UV_COUNTRIES, UV_CITIES };

struct Country { char name[28]; int nplaces; int nstations; int first; };  /* first: offset into country_places[] */

struct Fav { char id[16]; char name[56]; char place[64]; };

struct UiModel {
	UiView view;
	float lon, lat, zoom;
	const Place *places; int nplaces; int hover;
	const Place *panel_place;
	const Station *stations; int nst; int st_sel; int st_top;
	const Fav *favs; int nfav; int fav_sel; int fav_top;
	int loading;              /* 0 none, 1 places, 2 stations */
	const char *error;        /* load error, NULL if none */
	int pstate; const char *pfmt; const char *pmsg; float volume;
	const char *now_name; const char *now_place; const char *now_id;
	bool (*is_fav)(const char *id);
	const char *toast; int toast_t;
	bool tls; unsigned frame;
	/* country search */
	const Country *const *clist; int nclist; int c_sel, c_top;    /* filtered countries */
	const char *query; bool kb_open; int kb_sel;
	const Country *cur_country; const int *city_idx; int ncity; int city_sel, city_top;

	bool map_dirty;           /* globe needs re-render (camera moved) */
	bool ui_dirty;            /* panels need re-render */
	float fps;
};

void ui_init(unsigned w, unsigned h);
void ui_render(UiModel *m);
int  ui_list_rows();
int  ui_list_rows_ex(bool search, bool two_line);
bool ui_animating(const UiModel *m);
bool ui_load_earth(const char *path);                       /* RGB565 BE 4096x2048 */
void ui_set_places(const Place *p, int n);                  /* precompute unit vectors */
int  ui_pick(const UiModel *m);                             /* place under crosshair or -1 */
/* parallel helper: run job(0,arg) and job(1,arg), possibly on two hardware threads */
typedef void (*UiJob)(int part, void *arg);
void ui_set_parallel(void (*par)(UiJob job, void *arg));
extern const char *const UI_KB_KEYS[]; extern const int UI_KB_N, UI_KB_COLS;
bool ui_project(const UiModel *m, float lon, float lat, float *x, float *y);  /* false if on far side */
void ui_view_center(float *x, float *y);
float ui_globe_radius(const UiModel *m);
#endif
