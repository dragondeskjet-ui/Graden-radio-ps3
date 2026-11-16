#ifndef UI_H
#define UI_H
#include "api.h"

enum UiView { UV_MAP, UV_STATIONS, UV_FAVS };

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
	bool map_dirty;
	bool fast;                /* render globe at half resolution while moving */
};

void ui_init(unsigned w, unsigned h);
void ui_render(UiModel *m);
int  ui_list_rows();
bool ui_animating(const UiModel *m);
bool ui_load_earth(const char *path);                       /* RGB565 BE 1440x720; optional */
bool ui_project(const UiModel *m, float lon, float lat, float *x, float *y);  /* false if on far side */
void ui_view_center(float *x, float *y);
float ui_globe_radius(const UiModel *m);
#endif
