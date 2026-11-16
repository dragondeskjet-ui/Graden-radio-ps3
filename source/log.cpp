#include <stdio.h>
#include <stdarg.h>
#include <ppu-lv2.h>
#include <sys/systime.h>
#include <sys/mutex.h>
#include <string.h>
#include "log.h"
#define LOG_FILE "/dev_hdd0/game/RGDN00001/USRDIR/debug.log"

/* lv2 syscall 352: sys_memory_get_user_memory_size */
struct MemInfo { u32 total; u32 avail; } __attribute__((aligned(8)));
static s32 mem_info(MemInfo *m) { lv2syscall1(352, (u64)m); return_to_user_prog(s32); }

static sys_mutex_t mtx; static int mtx_ok = 0;
void rg_log_reset()
{
	if (!mtx_ok) {
		sys_mutex_attr_t a; memset(&a, 0, sizeof a);
		a.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO; a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
		a.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED; a.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
		mtx_ok = sysMutexCreate(&mtx, &a) == 0;
	}
	FILE *f = fopen(LOG_FILE, "w"); if (f) fclose(f);
}
void rg_log(const char *fmt, ...)
{
	if (mtx_ok) sysMutexLock(mtx, 0);
	FILE *f = fopen(LOG_FILE, "a"); if (!f) { if (mtx_ok) sysMutexUnlock(mtx); return; }
	static u64 t0 = 0; u64 s, ns; sysGetCurrentTime(&s, &ns); u64 ms = s * 1000 + ns / 1000000; if (!t0) t0 = ms;
	MemInfo mi = { 0, 0 }; mem_info(&mi);
	fprintf(f, "[%6.2fs free %5.1fMB] ", (ms - t0) / 1000.0, mi.avail / 1048576.0);
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fputc('\n', f); fclose(f);
	if (mtx_ok) sysMutexUnlock(mtx);
}
