#ifndef RG_LOG_H
#define RG_LOG_H
/* Appends a line to /dev_hdd0/game/RGDN00001/USRDIR/debug.log (opened and closed each time, so it survives a crash). */
void rg_log(const char *fmt, ...);
void rg_log_reset();
#endif
