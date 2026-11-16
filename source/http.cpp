#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/net.h>
#include <sysmodule/sysmodule.h>
#include "http.h"
#include "log.h"

#define CA_FILE "/dev_hdd0/game/RGDN00001/USRDIR/cacert.pem"
#define UA "Mozilla/5.0 (PLAYSTATION 3) RadioGardenPS3/1.0"

volatile int g_tls_verify = 1;

bool http_init()
{
	if (sysModuleLoad(SYSMODULE_NET) < 0) return false;
	if (netInitialize() < 0) return false;
	return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void http_shutdown() { curl_global_cleanup(); netDeinitialize(); }

void http_setup(CURL *c)
{
	curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_FILE);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, g_tls_verify ? 1L : 0L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, g_tls_verify ? 2L : 0L);
}

struct Ctx { Buf *b; size_t max; };

static size_t wcb(char *p, size_t sz, size_t n, void *u)
{
	Ctx *c = (Ctx *)u;
	size_t len = sz * n;
	if (c->b->n + len + 1 > c->max) return 0;
	if (c->b->n + len + 1 > c->b->cap) {
		size_t nc = c->b->cap ? c->b->cap : 65536;
		while (nc < c->b->n + len + 1) nc *= 2;
		char *nd = (char *)realloc(c->b->d, nc);
		if (!nd) return 0;
		c->b->d = nd; c->b->cap = nc;
	}
	memcpy(c->b->d + c->b->n, p, len);
	c->b->n += len;
	c->b->d[c->b->n] = 0;
	return len;
}

bool http_get(const char *url, Buf *out, size_t max_bytes, char *err, size_t errlen)
{
	CURL *c = curl_easy_init();
	if (!c) { snprintf(err, errlen, "curl init"); return false; }
	out->d = NULL; out->n = out->cap = 0;
	Ctx ctx = { out, max_bytes };
	http_setup(c);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
	char cerr[CURL_ERROR_SIZE] = {0};
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
	rg_log("http_get: perform %s", url);
	CURLcode rc = curl_easy_perform(c);
	rg_log("http_get: done rc=%d bytes=%u", (int)rc, (unsigned)out->n);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) {
		snprintf(err, errlen, "%s (%d)", cerr[0] ? cerr : curl_easy_strerror(rc), (int)rc);
		buf_free(out); return false;
	}
	if (code != 200) { snprintf(err, errlen, "HTTP %ld", code); buf_free(out); return false; }
	return true;
}

void buf_free(Buf *b) { free(b->d); b->d = NULL; b->n = b->cap = 0; }
