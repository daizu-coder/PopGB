/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * GDI video backend + Video Config dialog for PopGB.
 *
 * This file used to talk to the display through GAPI; session 20
 * replaced it with plain GDI (GetDC/PatBlt/CreateDIBSection/BitBlt) -
 * see ce_video.c's header comment for why (GAPI needs an extra DLL that
 * is not part of the OS; GDI is always present as part of coredll).
 *
 * gnuboy draws every frame at native 160x144 (fb.delegate_scaling, with
 * lcd.c's `scale` rcvar forced to 1). ce_video.c's BlitToScreen() scales
 * that by hand into an off-screen RGB565 DIB section and BitBlt()s it to
 * the screen 1:1.
 *
 * Scale (x1 / x2 / Wide / Full) is this file's own setting (CeScaleMode
 * in ce_video.c), not a gnuboy rcvar. Color Filter is lcd.c's own
 * `colorfilter` rcvar, flipped through rc_getmem(), with pal_dirty()
 * called after each flip so the colors on screen change at once. Frame
 * Skip is this file's own setting too (gnuboy is not a libretro core):
 * vid_end() skips only the blit to the screen, never gnuboy's own
 * drawing, while the audio ring's fill (ce_audio.h's
 * CeAudioGetBufferOccupancyPercent) is at or below a threshold, up to
 * Frame Skip frames in a row. The dialog also holds the UI language and
 * debug log toggles.
 */
#ifndef CE_VIDEO_H
#define CE_VIDEO_H

#include <windows.h>

/* Loads saved Scale/Color Filter/Frame Skip/Debug Log from the config
 * file into gnuboy's own `colorfilter` rcvar, this file's own statics and
 * ce_log.c, and forces lcd.c's `scale` rcvar to 1. Call once from
 * WinMain, after rc_exportvars(lcd_exports) has run (so the `scale`/
 * `colorfilter` rcvars exist to write into) and before the first ROM
 * loads. */
void CeVideoInit(void);

/* Historically wrapped any modal dialog shown while the GAPI display
 * was open (the Main Menu, Sound/Video/Input Config, the ROM picker,
 * ...) - GXOpenDisplay() claimed exclusive full-screen access, and
 * showing another top-level window/dialog on top of it without
 * suspending first was against GAPI's documented contract. Now that
 * this file uses plain GDI (no exclusive full-screen surface to
 * protect), both are no-ops - kept only so ce_main.c doesn't need to
 * change its call sites. */
void CeVideoSuspend(void);
void CeVideoResume(void);

void CeShowVideoConfigDialog(HWND owner);

/* Redraws the screen from whatever is already sitting in the native
 * buffer, bypassing Frame Skip - see ce_video.c's own comment above its
 * definition for why ce_main.c's WM_PAINT handler needs this now (this
 * backend draws straight into the window's own client area, unlike the
 * old GAPI backend's separate exclusive surface). Safe to call any time
 * after vid_init(); harmless before the first ROM loads too, it just
 * repaints whatever (zeroed) pixels are already in the buffer. */
void CeVideoForceRepaint(void);

/* Screenshot support. CeVideoGetLastImage() hands back the image currently
 * on screen at the active Scale (the top-left width x height of the
 * off-screen RGB565 DIB, rows `pitch` bytes apart, top-down). Returns 0
 * until vid_end() has blitted at least one frame since the last
 * CeVideoInvalidateLastImage() (called on every ROM load). */
void CeVideoInvalidateLastImage(void);
int  CeVideoGetLastImage(const void **pixels, unsigned *width, unsigned *height, unsigned *pitch);

#ifdef CE_PERF_DIAG
/* Diagnostic-only (see sys.h/Makefile.ce): total ms spent inside
 * BlitToScreen() (screen-wide PatBlt clear on a Scale change + the
 * hand-scaling into the DIB section + BitBlt) since the last call, then
 * resets to 0. Safe to
 * call every frame regardless of whether any blits happened (0 if not -
 * e.g. every frame Frame Skip decided to skip). */
unsigned CeVideoGetAndResetBlitMs(void);
#endif

/* Always-on: last completed 60-blit avg/max (ms) of BlitToScreen().
 * ce_main.c folds these into the unified cross-core "perf: retro_run
 * ... blit ..." summary line. 0/0 until the first 60 blits finish.
 * Independent of the CE_PERF_DIAG accessor above. */
void CeVideoGetBlitPerf(unsigned *avgMs, unsigned *maxMs);

#endif
