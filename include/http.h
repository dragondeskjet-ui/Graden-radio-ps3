#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>
#include <curl/curl.h>

struct Buf { char *d; size_t n; size_t cap; };

extern volatile int g_tls_verify;           /* 1 = verify certificates */

bool http_init();
void http_shutdown();
/* Configure options shared by every request. */
void http_setup(CURL *c);
/* GET url into a growing buffer. Returns false on error and fills err. */
bool http_get(const char *url, Buf *out, size_t max_bytes, char *err, size_t errlen);
void buf_free(Buf *b);
#endif
