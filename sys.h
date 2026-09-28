#ifndef VID_H
#define VID_H

/* stuff implemented by the different sys/ backends */

void vid_begin();
void vid_end();
void vid_init();
void vid_preinit();
void vid_close();
void vid_setpal(int i, int r, int g, int b);
void vid_settitle(char *title);

void pcm_init();
int pcm_submit();
void pcm_close();
void pcm_pause(int dopause);

void ev_poll(int wait);

void sys_checkdir(char *path, int wr);
void sys_sleep(int us);
void sys_sanitize(char *s);

void joy_init();
void joy_poll();
void joy_close();

void kb_init();
void kb_poll();
void kb_close();

#ifdef CE_PERF_DIAG
/* Diagnostic-only (2026-08-24 investigation into intermittent audio
 * dropouts at Frame Skip=30): per-frame timing breakdown so a sys/
 * backend can tell CPU/LCD execution cost apart from APU mix cost,
 * independent of whatever that backend does for screen output/audio
 * submission. Only declared when CE_PERF_DIAG is defined (Makefile.ce's
 * SYS_DEFS) - every other sys/ backend's build is untouched since this
 * whole block compiles out. emu.c calls ce_perf_begin()/ce_perf_end() in
 * matched pairs around the sections named by the slot constants below;
 * ce_perf_end() accumulates elapsed time into that slot rather than
 * overwriting, so multiple begin/end pairs per frame for the same slot
 * (emu.c's main scanline loop and its separate VBlank-wait loop both use
 * CE_PERF_SLOT_CPU_LCD) sum correctly. */
void ce_perf_begin(int slot);
void ce_perf_end(int slot);
#define CE_PERF_SLOT_CPU_LCD 0  /* cpu_emulate()/emu_step() - LR35902 execution, with lcd_refreshline() piggybacking on it via lcdc.c's interrupt-driven scanline callbacks */
#define CE_PERF_SLOT_APU_MIX 1  /* sound_mix() - APU sample generation, called once per frame outside the CPU loop */
#endif

/* FIXME these have different prototype for obsolete ( == M$ ) platforms */
#include <sys/time.h>
int sys_elapsed(struct timeval *prev);
void sys_initpath();

#endif
