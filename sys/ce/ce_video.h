/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * GDI video backend + Video Config dialog for PopGB.
 *
 * This file used to talk to the display through GAPI; session 20
 * replaced it with plain GDI (StretchDIBits/PatBlt/GetDC) - see
 * ce_video.c's header comment for why (GAPI needs an extra DLL that is
 * not part of the OS; GDI is always present as part of coredll).
 *
 * Scale and Color Filter are gnuboy's own rcvars (lcd.c's `scale`/
 * `colorfilter`, exported via lcd_exports[] - see the port's dev notes): lcd.c
 * itself center-and-magnifies into fb.ptr according to `scale`, so
 * this needs no display-side blit mode at all - the dialog just steps the rcvar's
 * backing int directly (see ce_video.c's s_pScale). Frame Skip is not a
 * core option (gnuboy is not a libretro core) - it's this file's own
 * setting, consumed by vid_begin()'s own occupancy-driven skip logic
 * (see ce_audio.h's CeAudioGetBufferOccupancyPercent).
 */
#ifndef CE_VIDEO_H
#define CE_VIDEO_H

#include <windows.h>

/* Loads saved Scale/Color Filter/Frame Skip/Open Last Folder from the
 * config file into gnuboy's own rcvars and this file's own statics.
 * Call once from WinMain, after rc_exportvars(lcd_exports) has run (so
 * the `scale`/`colorfilter` rcvars exist to write into) and before the
 * first ROM loads. */
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
 * BlitToScreen() (screen-wide PatBlt clear on a Scale change +
 * StretchDIBits) since the last call, then resets to 0. Safe to
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
