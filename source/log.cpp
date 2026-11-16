#include <stdio.h>
#include <stdarg.h>
#include "log.h"
#define LOG_FILE "/dev_hdd0/game/RGDN00001/USRDIR/debug.log"
void rg_log_reset() { FILE *f = fopen(LOG_FILE, "w"); if (f) fclose(f); }
void rg_log(const char *fmt, ...)
{
	FILE *f = fopen(LOG_FILE, "a"); if (!f) return;
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fputc('\n', f); fclose(f);
}
