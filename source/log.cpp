#include <stdio.h>
#include <stdarg.h>
#include <ppu-lv2.h>
#include <sys/systime.h>
#include <sys/mutex.h>
#include <string.h>
#include <sys/thread.h>
#include <unistd.h>
#include "log.h"
volatile int rg_stage = 0; volatile unsigned rg_wd_frame = 0;
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
	fprintf(f, "[%5u.%02us free %3uMB] ", (unsigned)((ms - t0) / 1000), (unsigned)((ms - t0) % 1000 / 10), (unsigned)(mi.avail >> 20));
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fputc('\n', f); fclose(f);
	if (mtx_ok) sysMutexUnlock(mtx);
}

static void wd_entry(void *)
{
	unsigned last = 0xFFFFFFFFu; int same = 0;
	for (;;) {
		sleep(1);
		unsigned f = rg_wd_frame;
		if (f == last) { if (++same <= 6 || same % 10 == 0) rg_log("WATCHDOG: main stuck %ds at stage %d (frame %u)", same, rg_stage, f); }
		else { if (same) rg_log("WATCHDOG: main resumed at stage %d", rg_stage); same = 0; }
		last = f;
	}
}
void rg_watchdog_start()
{
	sys_ppu_thread_t t; sysThreadCreate(&t, wd_entry, NULL, 900, 0x4000, 0, (char *)"rg_wd");
}
