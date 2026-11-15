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
};

void ui_init(unsigned w, unsigned h);
void ui_render(UiModel *m);
int  ui_list_rows();
bool ui_animating(const UiModel *m);
#endif
