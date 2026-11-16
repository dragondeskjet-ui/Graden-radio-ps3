#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "api.h"
#include "http.h"
#include "json.h"
#include "log.h"

#define BASE "https://radio.garden/api/ara/content"

Place *g_places = NULL; int g_nplaces = 0;
Station g_stations[MAX_STATIONS]; int g_nstations = 0;

static void cpy(char *d, size_t n, JNode *s) { d[0] = 0; if (s && s->type == J_STR && s->str) snprintf(d, n, "%s", s->str); }

/* ---- single-pass scanner: no DOM, writes straight into the Place array ---- */
static const char *ws(const char *p, const char *e) { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++; return p; }
static const char *rd_str(const char *p, const char *e, char *o, size_t cap)
{
	size_t k = 0; p++;                                    /* opening quote */
	while (p < e && *p != '"') {
		unsigned c = (unsigned char)*p++; bool uni = false;
		if (c == '\\' && p < e) {
			c = (unsigned char)*p++;
			if (c == 'n' || c == 't' || c == 'r') c = ' ';
			else if (c == 'u' && p + 4 <= e) { c = 0; for (int i = 0; i < 4; i++) { char h = *p++; c = c * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); } uni = true; }
		}
		if (!o) continue;
		if (!uni || c < 0x80) { if (k + 1 < cap) o[k++] = (char)c; }
		else if (c < 0x800) { if (k + 2 < cap) { o[k++] = (char)(0xC0 | (c >> 6)); o[k++] = (char)(0x80 | (c & 63)); } }
		else if (k + 3 < cap) { o[k++] = (char)(0xE0 | (c >> 12)); o[k++] = (char)(0x80 | ((c >> 6) & 63)); o[k++] = (char)(0x80 | (c & 63)); }
	}
	if (o) o[k] = 0;
	return p < e ? p + 1 : p;
}
static const char *rd_num(const char *p, const char *e, double *v)
{
	bool neg = false; double r = 0; if (p < e && *p == '-') { neg = true; p++; }
	while (p < e && *p >= '0' && *p <= '9') r = r * 10 + (*p++ - '0');
	if (p < e && *p == '.') { p++; double f = 0.1; while (p < e && *p >= '0' && *p <= '9') { r += (*p++ - '0') * f; f *= 0.1; } }
	if (p < e && (*p == 'e' || *p == 'E')) {
		p++; bool en = false; if (p < e && (*p == '-' || *p == '+')) { en = *p == '-'; p++; }
		int x = 0; while (p < e && *p >= '0' && *p <= '9' && x < 400) x = x * 10 + (*p++ - '0');
		while (x-- > 0) r = en ? r / 10 : r * 10;
	}
	*v = neg ? -r : r; return p;
}
static const char *skip_val(const char *p, const char *e)
{
	if (p >= e) return p;
	if (*p == '"') return rd_str(p, e, NULL, 0);
	if (*p == '{' || *p == '[') {
		int d = 0;
		while (p < e) { char c = *p; if (c == '"') { p = rd_str(p, e, NULL, 0); continue; } if (c == '{' || c == '[') d++; else if (c == '}' || c == ']') { if (--d == 0) return p + 1; } p++; }
		return p;
	}
	while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
	return p;
}
static int scan_places(const char *s, size_t n, Place **out)
{
	const char *e = s + n, *p = strstr(s, "\"list\"");
	if (!p) return 0;
	while (p < e && *p != '[') p++;
	if (p >= e) return 0; p++;
	int cap = 16384, k = 0;
	Place *arr = (Place *)calloc(cap, sizeof(Place)); if (!arr) return 0;
	for (;;) {
		p = ws(p, e);
		if (p >= e || *p != '{') break;
		p++;
		if (k >= cap) { cap *= 2; Place *t = (Place *)realloc(arr, cap * sizeof(Place)); if (!t) break; arr = t; memset(arr + k, 0, (cap - k) * sizeof(Place)); }
		Place &q = arr[k]; q.size = 1; bool geo = false;
		for (;;) {
			p = ws(p, e);
			if (p >= e) break;
			if (*p == '}') { p++; break; }
			if (*p != '"') { p++; continue; }
			char key[16]; p = rd_str(p, e, key, sizeof key);
			p = ws(p, e); if (p < e && *p == ':') p++; p = ws(p, e);
			if (!strcmp(key, "id") && p < e && *p == '"') p = rd_str(p, e, q.id, sizeof q.id);
			else if (!strcmp(key, "title") && p < e && *p == '"') p = rd_str(p, e, q.title, sizeof q.title);
			else if (!strcmp(key, "country") && p < e && *p == '"') p = rd_str(p, e, q.country, sizeof q.country);
			else if (!strcmp(key, "size") && p < e && *p != '"') { double v; p = rd_num(p, e, &v); q.size = v < 1 ? 1 : (v > 65535 ? 65535 : (u16)v); }
			else if (!strcmp(key, "geo") && p < e && *p == '[') {
				double lo, la; p = ws(p + 1, e); p = rd_num(p, e, &lo); p = ws(p, e); p = rd_num(p, e, &la);
				q.lon = (float)lo; q.lat = (float)la; geo = true;
				while (p < e && *p != ']') p++; if (p < e) p++;
			} else p = skip_val(p, e);
		}
		if (geo && q.id[0]) k++; else memset(&q, 0, sizeof q);
		if ((k & 511) == 511) usleep(300);
	}
	*out = arr; return k;
}

/* ---- on-disk cache: first run downloads, later runs are instant ---- */
#define CACHE_FILE "/dev_hdd0/game/RGDN00001/USRDIR/places.bin"
struct CacheHdr { char magic[4]; u32 count; u32 psize; u32 pad; };
static bool cache_load()
{
	FILE *f = fopen(CACHE_FILE, "rb"); if (!f) return false;
	CacheHdr h; bool ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "RGP3", 4) && h.psize == sizeof(Place) && h.count > 100 && h.count < 100000;
	Place *arr = ok ? (Place *)malloc(h.count * sizeof(Place)) : NULL;
	if (arr && fread(arr, sizeof(Place), h.count, f) == h.count) { free(g_places); g_places = arr; g_nplaces = (int)h.count; fclose(f); return true; }
	free(arr); fclose(f); return false;
}
static void cache_save()
{
	FILE *f = fopen(CACHE_FILE, "wb"); if (!f) return;
	CacheHdr h; memcpy(h.magic, "RGP3", 4); h.count = g_nplaces; h.psize = sizeof(Place); h.pad = 0;
	fwrite(&h, sizeof h, 1, f); fwrite(g_places, sizeof(Place), g_nplaces, f); fclose(f);
}

bool api_load_places(char *err, size_t errlen)
{
	if (cache_load()) { rg_log("places: from cache (%d)", g_nplaces); return true; }
	Buf b;
	rg_log("places: download start");
	if (!http_get(BASE "/places", &b, 32u << 20, err, errlen)) { rg_log("places: download failed: %s", err); return false; }
	rg_log("places: downloaded %u bytes", (unsigned)b.n);
	b.d[b.n] = 0;
	Place *arr = NULL; int k = scan_places(b.d, b.n, &arr);
	buf_free(&b);
	rg_log("places: scanned %d", k);
	if (k == 0) { free(arr); snprintf(err, errlen, "Respuesta inesperada (places)"); return false; }
	free(g_places); g_places = arr; g_nplaces = k;
	cache_save(); rg_log("places: cache saved");
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
