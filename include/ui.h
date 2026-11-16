#ifndef UI_H
#define UI_H
#include "api.h"

enum UiView { UV_MAP, UV_STATIONS, UV_FAVS, UV_COUNTRIES, UV_CITIES, UV_MENU };

struct Country { char name[28]; int nplaces; int nstations; int first; };
struct Fav { char id[16]; char name[56]; char place[64]; };

#define UI_NTHEMES 4
enum { MENU_FAVS, MENU_COUNTRIES, MENU_THEME, MENU_R, MENU_G, MENU_B, MENU_TLS, MENU_RANDOM, UI_MENU_N };

struct UiModel {
	UiView view;
	float lon, lat, zoom;
	const Place *places; int nplaces; int hover;
	const Place *panel_place;
	const Station *stations; int nst; int st_sel; int st_top;
	const Fav *favs; int nfav; int fav_sel; int fav_top;
	int loading;              /* 0 none, 1 places, 2 stations */
	const char *error;
	int pstate; const char *pfmt; const char *pmsg; float volume;
	const char *now_name; const char *now_place; const char *now_id;
	bool (*is_fav)(const char *id);
	const char *toast; int toast_t;
	bool tls; unsigned frame;
	/* country search */
	const Country *const *clist; int nclist; int c_sel, c_top;
	const char *query; bool kb_open; int kb_sel;
	const Country *cur_country; const int *city_idx; int ncity; int city_sel, city_top;
	/* menu / appearance */
	int menu_sel; int theme; int ar, ag, ab;
	bool ui_dirty;            /* panels must be redrawn */
	float fps;
};

void ui_init(unsigned w, unsigned h);
void ui_set_places(const Place *p, int n);
int  ui_pick(const UiModel *m);
int  ui_rows(const UiModel *m);           /* visible rows of the list in the current view */
void ui_frame(UiModel *m);                /* builds what changed and submits the frame to the GPU */
bool ui_project(const UiModel *m, float lon, float lat, float *x, float *y);
const char *ui_theme_name(int t);
extern const char *const UI_KB_KEYS[]; extern const int UI_KB_N, UI_KB_COLS;
#endif
