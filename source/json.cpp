#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "json.h"

struct Arena { char *cur; char *end; Arena *prev; };
struct P { const char *s; const char *e; Arena *a; bool err; int depth; };

static void *aalloc(P *p, size_t n)
{
	n = (n + 7) & ~(size_t)7;
	if (!p->a || (size_t)(p->a->end - p->a->cur) < n) {
		size_t sz = n > (1 << 20) ? n : (1 << 20);
		Arena *a = (Arena *)malloc(sizeof(Arena) + sz);
		if (!a) { p->err = true; return NULL; }
		a->cur = (char *)(a + 1); a->end = a->cur + sz; a->prev = p->a; p->a = a;
	}
	void *r = p->a->cur; p->a->cur += n; return r;
}

static void ws(P *p) { while (p->s < p->e && (*p->s == ' ' || *p->s == '\n' || *p->s == '\r' || *p->s == '\t')) p->s++; }

static int hex4(const char *s)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i]; v <<= 4;
		if (c >= '0' && c <= '9') v |= c - '0';
		else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
		else return -1;
	}
	return v;
}

static char *pstr(P *p)
{
	if (p->s >= p->e || *p->s != '"') { p->err = true; return NULL; }
	p->s++;
	const char *st = p->s;
	size_t cap = 0;
	while (p->s < p->e && *p->s != '"') { if (*p->s == '\\') p->s++; p->s++; cap++; }
	if (p->s >= p->e) { p->err = true; return NULL; }
	char *out = (char *)aalloc(p, cap * 3 + 1);
	if (!out) return NULL;
	char *o = out;
	for (const char *q = st; q < p->s; q++) {
		if (*q != '\\') { *o++ = *q; continue; }
		q++;
		switch (*q) {
		case 'n': *o++ = '\n'; break; case 't': *o++ = '\t'; break; case 'r': *o++ = '\r'; break;
		case 'b': *o++ = '\b'; break; case 'f': *o++ = '\f'; break;
		case 'u': {
			if (q + 4 >= p->s) { p->err = true; return NULL; }
			int cp = hex4(q + 1); q += 4;
			if (cp < 0) { p->err = true; return NULL; }
			if (cp >= 0xD800 && cp < 0xDC00 && q + 6 < p->s && q[1] == '\\' && q[2] == 'u') {
				int lo = hex4(q + 3);
				if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); q += 6; }
			}
			if (cp < 0x80) *o++ = cp;
			else if (cp < 0x800) { *o++ = 0xC0 | (cp >> 6); *o++ = 0x80 | (cp & 63); }
			else if (cp < 0x10000) { *o++ = 0xE0 | (cp >> 12); *o++ = 0x80 | ((cp >> 6) & 63); *o++ = 0x80 | (cp & 63); }
			else { *o++ = 0xF0 | (cp >> 18); *o++ = 0x80 | ((cp >> 12) & 63); *o++ = 0x80 | ((cp >> 6) & 63); *o++ = 0x80 | (cp & 63); }
			break; }
		default: *o++ = *q; break;
		}
	}
	*o = 0;
	p->s++; /* closing quote */
	return out;
}

/* Own number parser: newlib's atof/strtod share unlocked state with printf("%f") on other threads. */
static double parse_num(const char *s)
{
	double sign = 1, v = 0; int e = 0;
	if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); e--; } }
	if (*s == 'e' || *s == 'E') {
		s++; int es = 1, ev = 0;
		if (*s == '-') { es = -1; s++; } else if (*s == '+') s++;
		while (*s >= '0' && *s <= '9' && ev < 1000) ev = ev * 10 + (*s++ - '0');
		e += es * ev;
	}
	if (e > 300) e = 300; if (e < -300) e = -300;
	double p10 = 1; for (int i = e < 0 ? -e : e; i > 0; i--) p10 *= 10;
	return sign * (e < 0 ? v / p10 : v * p10);
}

static JNode *pval(P *p);

static JNode *newnode(P *p, JType t)
{
	static unsigned cnt = 0;
	if ((++cnt & 255) == 0) usleep(300);      /* let the render thread run while a big document parses */
	JNode *n = (JNode *)aalloc(p, sizeof(JNode));
	if (n) { memset(n, 0, sizeof(JNode)); n->type = t; }
	return n;
}

static JNode *pval(P *p)
{
	ws(p);
	if (p->err || p->s >= p->e || p->depth > 64) { p->err = true; return NULL; }
	char c = *p->s;
	JNode *n;
	if (c == '"') { n = newnode(p, J_STR); if (!n) return NULL; n->str = pstr(p); return n; }
	if (c == '{' || c == '[') {
		bool obj = c == '{';
		char close = obj ? '}' : ']';
		n = newnode(p, obj ? J_OBJ : J_ARR); if (!n) return NULL;
		p->s++; p->depth++;
		JNode **tail = &n->child;
		ws(p);
		if (p->s < p->e && *p->s == close) { p->s++; p->depth--; return n; }
		for (;;) {
			ws(p);
			char *key = NULL;
			if (obj) {
				key = pstr(p); if (!key) return NULL;
				ws(p);
				if (p->s >= p->e || *p->s != ':') { p->err = true; return NULL; }
				p->s++;
			}
			JNode *v = pval(p); if (!v) return NULL;
			v->key = key; *tail = v; tail = &v->next;
			ws(p);
			if (p->s < p->e && *p->s == ',') { p->s++; continue; }
			if (p->s < p->e && *p->s == close) { p->s++; break; }
			p->err = true; return NULL;
		}
		p->depth--;
		return n;
	}
	if (!strncmp(p->s, "true", 4))  { p->s += 4; n = newnode(p, J_BOOL); if (n) n->num = 1; return n; }
	if (!strncmp(p->s, "false", 5)) { p->s += 5; n = newnode(p, J_BOOL); return n; }
	if (!strncmp(p->s, "null", 4))  { p->s += 4; return newnode(p, J_NULL); }
	if (c == '-' || (c >= '0' && c <= '9')) {
		char buf[40]; int i = 0;
		while (p->s < p->e && i < 39 && (strchr("+-.eE", *p->s) || (*p->s >= '0' && *p->s <= '9'))) buf[i++] = *p->s++;
		buf[i] = 0;
		n = newnode(p, J_NUM); if (n) n->num = parse_num(buf);
		return n;
	}
	p->err = true; return NULL;
}

JDoc *json_parse(const char *text, size_t len)
{
	P p = { text, text + len, NULL, false, 0 };
	JNode *r = pval(&p);
	JDoc *d = NULL;
	if (r && !p.err) {
		d = (JDoc *)malloc(sizeof(JDoc));
		if (d) { d->root = r; d->arena = p.a; return d; }
	}
	while (p.a) { Arena *pr = p.a->prev; free(p.a); p.a = pr; }
	return NULL;
}

void json_free(JDoc *d)
{
	if (!d) return;
	Arena *a = (Arena *)d->arena;
	while (a) { Arena *pr = a->prev; free(a); a = pr; }
	free(d);
}

JNode *json_get(JNode *obj, const char *key)
{
	if (!obj || obj->type != J_OBJ) return NULL;
	for (JNode *c = obj->child; c; c = c->next) if (c->key && !strcmp(c->key, key)) return c;
	return NULL;
}
