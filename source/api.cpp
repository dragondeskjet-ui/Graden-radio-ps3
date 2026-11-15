#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"
#include "http.h"
#include "json.h"

#define BASE "https://radio.garden/api/ara/content"

Place *g_places = NULL; int g_nplaces = 0;
Station g_stations[MAX_STATIONS]; int g_nstations = 0;

static void cpy(char *d, size_t n, JNode *s) { d[0] = 0; if (s && s->type == J_STR && s->str) snprintf(d, n, "%s", s->str); }

bool api_load_places(char *err, size_t errlen)
{
	Buf b;
	if (!http_get(BASE "/places", &b, 32u << 20, err, errlen)) return false;
	JDoc *d = json_parse(b.d, b.n);
	buf_free(&b);
	if (!d) { snprintf(err, errlen, "JSON invalido (places)"); return false; }
	JNode *list = json_get(json_get(d->root, "data"), "list");
	if (!list || list->type != J_ARR) { json_free(d); snprintf(err, errlen, "Respuesta inesperada (places)"); return false; }
	int cnt = 0;
	for (JNode *n = list->child; n; n = n->next) cnt++;
	Place *arr = (Place *)calloc(cnt ? cnt : 1, sizeof(Place));
	if (!arr) { json_free(d); snprintf(err, errlen, "Sin memoria"); return false; }
	int k = 0;
	for (JNode *n = list->child; n; n = n->next) {
		if (n->type != J_OBJ) continue;
		JNode *geo = json_get(n, "geo");
		if (!geo || geo->type != J_ARR || !geo->child || !geo->child->next) continue;
		Place &p = arr[k];
		p.lon = (float)geo->child->num; p.lat = (float)geo->child->next->num;
		cpy(p.id, sizeof p.id, json_get(n, "id"));
		cpy(p.title, sizeof p.title, json_get(n, "title"));
		cpy(p.country, sizeof p.country, json_get(n, "country"));
		JNode *sz = json_get(n, "size");
		p.size = (sz && sz->type == J_NUM) ? (u16)(sz->num > 65535 ? 65535 : sz->num) : 1;
		if (p.id[0]) k++;
	}
	json_free(d);
	free(g_places); g_places = arr; g_nplaces = k;
	if (k == 0) { snprintf(err, errlen, "Lista de lugares vacia"); return false; }
	return true;
}

bool api_load_stations(const char *place_id, char *err, size_t errlen)
{
	char url[160];
	snprintf(url, sizeof url, BASE "/page/%s/channels", place_id);
	Buf b;
	if (!http_get(url, &b, 4u << 20, err, errlen)) return false;
	JDoc *d = json_parse(b.d, b.n);
	buf_free(&b);
	if (!d) { snprintf(err, errlen, "JSON invalido (emisoras)"); return false; }
	JNode *content = json_get(json_get(d->root, "data"), "content");
	g_nstations = 0;
	if (content && content->type == J_ARR) {
		for (JNode *sec = content->child; sec; sec = sec->next) {
			JNode *items = json_get(sec, "items");
			if (!items || items->type != J_ARR) continue;
			for (JNode *it = items->child; it && g_nstations < MAX_STATIONS; it = it->next) {
				JNode *pg = json_get(it, "page");
				if (!pg) pg = it;
				JNode *u = json_get(pg, "url");
				if (!u || u->type != J_STR || !u->str) continue;
				const char *slash = strrchr(u->str, '/');
				if (!slash || !strstr(u->str, "/listen/")) continue;
				Station &s = g_stations[g_nstations];
				snprintf(s.id, sizeof s.id, "%s", slash + 1);
				cpy(s.name, sizeof s.name, json_get(pg, "title"));
				cpy(s.sub, sizeof s.sub, json_get(pg, "subtitle"));
				if (!s.name[0]) snprintf(s.name, sizeof s.name, "%s", s.id);
				g_nstations++;
			}
		}
	}
	json_free(d);
	if (g_nstations == 0) { snprintf(err, errlen, "Sin emisoras en este lugar"); return false; }
	return true;
}

void api_stream_url(const char *channel_id, char *out, size_t outlen)
{
	snprintf(out, outlen, BASE "/listen/%s/channel.mp3", channel_id);
}
