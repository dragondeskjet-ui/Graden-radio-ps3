#ifndef RG_LOG_H
#define RG_LOG_H
/* Appends a line to /dev_hdd0/game/RGDN00001/USRDIR/debug.log (opened and closed each time, so it survives a crash). */
void rg_log(const char *fmt, ...);
void rg_log_reset();
/* stage marker for the watchdog: set by the main thread, reported if the frame counter stops advancing */
extern volatile int rg_stage;
extern volatile unsigned rg_wd_frame;
void rg_watchdog_start();
#define RG_STAGE(n) (rg_stage = (n))
#endif
