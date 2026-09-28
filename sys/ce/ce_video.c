/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * ce_video.c - GDI video backend for PopGB.
 *
 * Session 20 rewrite: this file used to talk to the display through
 * GAPI. Not a technical fix: GAPI needs an extra DLL that is not part
 * of the OS itself, while GDI is always present as part of coredll, so
 * the display path was moved to plain GDI to keep the binary free of
 * any dependency beyond the OS. Two GAPI-free alternatives were
 * investigated and ruled out on real hardware first:
 *   - ExtEscape(GETRAWFRAMEBUFFER) - the display driver reports it as
 *     unsupported (result=0);
 *   - DirectDraw (ddraw.dll) - DirectDrawCreate resolves, but calling
 *     into the object it returns crashed the device outright (a
 *     hardware BER crash report, PC landing in DLL space) before the
 *     ROM picker dialog could even appear. cegcc also ships no ddraw.h
 *     at all, so continuing down that path meant guessing at an
 *     un-headered COM ABI with no way to tell a wrong vtable slot from
 *     a genuinely broken driver.
 * GDI (BitBlt/PatBlt/GetDC/CreateDIBSection) is what's left: it's part
 * of coredll.dll like every other Win32 call this port already makes,
 * so it carries no extra DLL dependency at all. A small GDI-only homebrew sample for this same device family
 * (GetDC/CreateCompatibleDC/BitBlt/FillRect/SetPixel) confirmed GDI
 * itself runs at real-time speed here - so the first cut of this
 * rewrite tried
 * handing the scale-and-letterbox step itself to StretchDIBits() in one
 * call, expecting the driver to do it at least as cheaply as GAPI's own
 * blit had. Real hardware showed the opposite: StretchDIBits() turned
 * out to be much slower than the flat (unscaled) BitBlt that reference
 * sample uses - this device's driver evidently has no fast path for
 * arbitrary-ratio stretching. So BlitToScreen() below does the scale
 * itself instead (the same per-pixel nearest-neighbor loop with a
 * precomputed per-column lookup table the old GAPI backend used,
 * unchanged), into an off-screen RGB565 DIB section (s_dibBits) sized
 * to the full physical screen, then transfers that with a plain 1:1
 * BitBlt() - i.e. exactly the reference sample's own proven-fast shape,
 * just with the pixel data prepared by hand first instead of drawn
 * directly with GDI primitives.
 *
 * gnuboy still renders each frame at native 160x144 into a small fixed
 * RAM buffer (`s_nativeBuf`, see fb.h's `delegate_scaling` flag - the
 * same mechanism sys/sdl2/sdl2.c uses to let SDL's own renderer do the
 * final scale) instead of writing scaled pixels directly to the screen.
 * One of 4 Scale modes (x1 / x2 / Wide / Full - see CeScaleMode below)
 * picks the destination rect within the DIB section that gets filled
 * each frame.
 *
 * gnuboy's own scanline drawing is otherwise untouched - lcd.c's
 * `scale` rcvar is simply forced to 1 once (CeVideoInit()) so it always
 * draws unscaled 160x144 into fb.ptr, same as sys/sdl2/sdl2.c does.
 *
 * Session 23: BlitToScreen()'s scaled (non-identityX) branch packs 2
 * pixels per 32-bit store instead of 2 separate 16-bit stores - ported
 * from sister PopNES's ce_display.c CeDisplayBlitRGB565(), which
 * confirmed on real hardware (same device family) an ~18-20% blit-time
 * drop from the same change. See that loop's own comment for the
 * alignment/endianness reasoning.
 *
 * Session 26: the old aspect-correct "x1.5" mode (dstW 355, an odd,
 * non-integer scale that still needed the per-column xLut gather) is
 * replaced by an exact integer "x2" mode (320x288, letterboxed). x2's
 * every dest pixel-pair is the *same* source pixel, so it skips the
 * xLut gather entirely: broadcast one source uint16 to a 32-bit pair
 * (p | (p<<16)) with a single aligned store per source pixel, and
 * duplicate each finished row for the second scanline - the same
 * s_scale2x idea PopNES uses. "Wide" is also pinned to a fixed
 * dstW of 400 (was the 355..480 midpoint, 417) - marginally narrower
 * so marginally cheaper, no structural change (still an xLut gather).
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "ce_video.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_fileopen.h"
#include "ce_resource.h"
#include "ce_audio.h"

#include "defs.h"
#include "rc.h"
#include "fb.h"
#include "lcd.h"

extern HWND CeGetWindow(void); /* ce_main.c */

struct fb fb;

/* gnuboy's own native resolution - fb.w/fb.h/fb.pitch below describe
 * this RAM buffer, not the physical screen (see this file's header
 * comment). fb.ptr is set once, in vid_init(), and never touched again -
 * lcd.c always draws into the same fixed buffer every frame. */
#define CE_NATIVE_W 160
#define CE_NATIVE_H 144
#define CE_NATIVE_PITCH (CE_NATIVE_W * 2) /* RGB565, 2 bytes/pixel */

static uint16_t s_nativeBuf[CE_NATIVE_W * CE_NATIVE_H];

/* This device's physical screen size (hardware-confirmed 480x320 by a
 * real GXDisplayProperties log back when this file still used GAPI -
 * GetSystemMetrics() below is expected to report the same numbers, just
 * through a standard call instead of a GAPI one). Read once in
 * vid_init(); the display doesn't resize at runtime. */
static int s_physW, s_physH;

/* Off-screen RGB565 DIB section sized to the full physical screen -
 * BlitToScreen() writes its scaled output directly into s_dibBits (see
 * this file's header comment for why: a plain 1:1 BitBlt() from this
 * section is much cheaper on this device than letting StretchDIBits()
 * do the scaling itself), then transfers it to the real screen with
 * BitBlt() through s_memDC, the section's own selected-in memory DC.
 * Built once in vid_init(), torn down in vid_close(); s_dibPitch is the
 * section's byte stride per row (DIB rows are DWORD-aligned, so this
 * isn't always simply s_physW*2). Top-down (negative biHeight) to match
 * gnuboy's own top-to-bottom row order - no per-frame row-reversal
 * needed. BI_BITFIELDS with explicit 5-6-5 masks because a plain
 * BI_RGB 16bpp DIB defaults to 5-5-5, not our 5-6-5 (see fb.cc[]
 * below - same layout). */
static HDC      s_memDC     = NULL;
static HBITMAP  s_dibBitmap = NULL;
static uint16_t *s_dibBits  = NULL;
static int       s_dibPitch = 0;

rcvar_t vid_exports[] =
{
	RCV_END
};

void vid_preinit()
{
}

void vid_init()
{
	HWND hwnd = CeGetWindow();
	HDC hdc;
	struct { BITMAPINFOHEADER h; DWORD masks[3]; } dibInfo;

	s_physW = GetSystemMetrics(SM_CXSCREEN);
	s_physH = GetSystemMetrics(SM_CYSCREEN);
	CeLog("vid_init: GetSystemMetrics cx=%d cy=%d", s_physW, s_physH);

	hdc = GetDC(hwnd);
	if (!hdc)
		die("GetDC(hwnd) failed\n");

	/* Paint the physical screen black immediately - otherwise whatever
	 * was in video memory before this process ever wrote to it (a
	 * previous app, the shell, ...) stays visible behind every dialog
	 * until the first ROM actually loads and gnuboy starts producing
	 * frames (see this file's header comment history - this is what a
	 * hardware screenshot from the old GAPI version of this file once
	 * showed). */
	PatBlt(hdc, 0, 0, s_physW, s_physH, BLACKNESS);

	memset(&dibInfo, 0, sizeof(dibInfo));
	dibInfo.h.biSize = sizeof(BITMAPINFOHEADER);
	dibInfo.h.biWidth = s_physW;
	dibInfo.h.biHeight = -s_physH; /* negative = top-down */
	dibInfo.h.biPlanes = 1;
	dibInfo.h.biBitCount = 16;
	dibInfo.h.biCompression = BI_BITFIELDS;
	dibInfo.masks[0] = 0xF800; /* red:   5 bits @ bit 11 - matches fb.cc[0] below */
	dibInfo.masks[1] = 0x07E0; /* green: 6 bits @ bit 5  - matches fb.cc[1] below */
	dibInfo.masks[2] = 0x001F; /* blue:  5 bits @ bit 0  - matches fb.cc[2] below */

	s_dibBitmap = CreateDIBSection(hdc, (BITMAPINFO *)&dibInfo, DIB_RGB_COLORS, (void **)&s_dibBits, NULL, 0);
	if (!s_dibBitmap || !s_dibBits)
		die("CreateDIBSection failed\n");
	s_dibPitch = ((s_physW * 16 + 31) / 32) * 4; /* DIB rows are DWORD-aligned */

	s_memDC = CreateCompatibleDC(hdc);
	if (!s_memDC)
		die("CreateCompatibleDC failed\n");
	SelectObject(s_memDC, s_dibBitmap);

	ReleaseDC(hwnd, hdc);

	fb.w = CE_NATIVE_W;
	fb.h = CE_NATIVE_H;
	fb.pelsize = 2;
	fb.pitch = CE_NATIVE_PITCH;
	fb.indexed = 0;
	fb.yuv = 0;
	fb.cc[0].r = 3; fb.cc[0].l = 11; /* red:   5 bits @ bit 11 */
	fb.cc[1].r = 2; fb.cc[1].l = 5;  /* green: 6 bits @ bit 5  */
	fb.cc[2].r = 3; fb.cc[2].l = 0;  /* blue:  5 bits @ bit 0  */
	fb.ptr = (byte *)s_nativeBuf;
	fb.enabled = 1;
	fb.dirty = 1;
	fb.delegate_scaling = 1; /* draw at native 1x - see sys/sdl2/sdl2.c for the same technique */
}

/* GAPI's exclusive full-screen surface needed suspending before any
 * other top-level window/dialog could safely show on top of it - GDI
 * has no such concept (any number of windows/dialogs already coexist
 * over a normal GDI-drawn surface), so these are both no-ops. Left in
 * place (rather than removed) because ce_main.c wraps every modal
 * dialog with calls to these - see ce_video.h. */
void CeVideoSuspend(void)
{
}

void CeVideoResume(void)
{
}

void vid_close()
{
	if (s_memDC)
	{
		DeleteDC(s_memDC);
		s_memDC = NULL;
	}
	if (s_dibBitmap)
	{
		DeleteObject(s_dibBitmap);
		s_dibBitmap = NULL;
		s_dibBits = NULL;
	}
	fb.enabled = 0;
}

/* ------------------------------------------------------------------ */
/* Scale mode + scaled blit                                            */
/* ------------------------------------------------------------------ */

typedef enum
{
	CE_SCALE_1TO1        = 0, /* "x1": no scaling, centered */
	CE_SCALE_2X          = 1, /* "x2": exact integer 2x (320x288), centered/letterboxed */
	CE_SCALE_HALFSTRETCH = 2, /* "Wide": fixed 400px wide, full screen height, letterboxed left/right */
	CE_SCALE_EXPAND      = 3, /* "Full": stretch to fill the screen exactly, aspect ratio not preserved */
} CeScaleMode;

static const CeScaleMode kScaleOrder[4] = {
	CE_SCALE_1TO1, CE_SCALE_2X, CE_SCALE_HALFSTRETCH, CE_SCALE_EXPAND
};
static const wchar_t *kScaleLabels[4] = { L"x1", L"x2", L"Wide", L"Full" };
#define CE_SCALE_CHOICE_COUNT 4

static CeScaleMode s_scaleMode = CE_SCALE_EXPAND;

static int ScaleModeToIndex(CeScaleMode m)
{
	int i;
	for (i = 0; i < CE_SCALE_CHOICE_COUNT; i++)
		if (kScaleOrder[i] == m)
			return i;
	return 0;
}

static void ComputeScaleRect(int physW, int physH, int *outX, int *outY, int *outW, int *outH)
{
	int w, h;

	switch (s_scaleMode)
	{
	case CE_SCALE_1TO1:
		w = CE_NATIVE_W;
		h = CE_NATIVE_H;
		break;

	case CE_SCALE_2X:
		w = CE_NATIVE_W * 2; /* 320 */
		h = CE_NATIVE_H * 2; /* 288 */
		break;

	case CE_SCALE_HALFSTRETCH:
		w = 400; /* fixed - clamped to physW below if the screen is ever narrower */
		h = physH;
		break;

	case CE_SCALE_EXPAND:
	default:
		w = physW;
		h = physH;
		break;
	}

	if (w > physW) w = physW;
	if (h > physH) h = physH;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	*outW = w;
	*outH = h;
	*outX = (physW - w) / 2;
	*outY = (physH - h) / 2;
}

/* Scale-and-letterbox blit from the fixed 160x144 native buffer to the
 * screen: a nearest-neighbor scale (16.16 fixed-point stepping, one
 * division per axis, not per pixel, plus a precomputed per-column
 * source-x lookup table reused for every row) into s_dibBits, the same
 * technique the old GAPI backend used directly on its raw framebuffer
 * pointer, followed by a single plain 1:1 BitBlt() of the scaled result
 * to the screen - see this file's header comment for why: this device's
 * driver turned out to make StretchDIBits()'s own built-in scaling much
 * more expensive than doing the same math by hand and blitting unscaled.
 * Clears the whole physical surface only when s_scaleGeomDirty is set
 * (first call, or right after a Scale mode change - see its
 * declaration) - every other frame reuses the same destination rect and
 * never touches the border again. */
#ifdef CE_PERF_DIAG
static unsigned s_blitAccumMs = 0;

unsigned CeVideoGetAndResetBlitMs(void)
{
	unsigned ms = s_blitAccumMs;
	s_blitAccumMs = 0;
	return ms;
}
#endif

/* Always-on (unlike the CE_PERF_DIAG breakdown above) lightweight blit
 * timing for the unified cross-core "perf:" summary line ce_main.c
 * writes - just a GetTickCount() pair around BlitToScreen()'s body.
 * Every CE_FRAMESKIP-affected build still counts one entry per blit
 * that actually ran, so with Frame Skip on the window covers fewer than
 * s_blitPerfCount emu-frames; at Frame Skip 0 (the default) it's 1:1.
 * The last completed 60-blit avg/max is snapshotted for ce_main.c to
 * fold into its line via CeVideoGetBlitPerf(). */
#define CE_BLIT_PERF_WINDOW 60
static unsigned s_blitPerfAccumMs = 0;
static unsigned s_blitPerfMaxMs   = 0;
static unsigned s_blitPerfCount   = 0;
static unsigned s_blitPerfLastAvg = 0;
static unsigned s_blitPerfLastMax = 0;

void CeVideoGetBlitPerf(unsigned *avgMs, unsigned *maxMs)
{
	if (avgMs) *avgMs = s_blitPerfLastAvg;
	if (maxMs) *maxMs = s_blitPerfLastMax;
}

/* Set whenever s_scaleMode changes (StepScaleMode()) or video first
 * initializes (CeVideoInit()) - tells the next BlitToScreen() call to
 * recompute dstX/dstY/dstW/dstH and, since whichever letterbox border
 * the *previous* mode left behind (if any) may no longer match, to
 * clear the whole physical surface one more time. Every subsequent
 * frame with the same mode reuses the cached rect and only overwrites
 * that same dstW x dstH area, so it never needs to touch the border
 * again - this device's screen is a direct, persistent framebuffer
 * behind GDI (see vid_init()'s own one-time clear and its comment), not
 * double-buffered, so untouched pixels simply stay put. */
static int s_scaleGeomDirty = 1;

/* Screenshot support (CeVideoGetLastImage() below, ported from the
 * sister PopSG's CeDisplayGetLastImage()): BlitToScreen() always writes
 * its scaled output to the DIB's top-left dstW x dstH before the BitBlt,
 * so that area *is* the image currently on screen. s_lastImgW/H record
 * the size of the most recent blit; s_lastImgValid is set only by
 * vid_end() (a real, freshly emulated frame) and cleared by
 * CeVideoInvalidateLastImage() on every ROM load, so a screenshot taken
 * right after switching ROMs can't save the previous game's picture. */
static int s_lastImgW = 0, s_lastImgH = 0;
static int s_lastImgValid = 0;

static void BlitToScreen(void)
{
	HDC hdc;
	HWND hwnd;
	static int dstX, dstY, dstW, dstH;
	static uint32_t xStep, yStep;
	static int xLut[512];
	static int identityX; /* 1 when dstW == CE_NATIVE_W: no horizontal
	                        * scaling, so each row is a straight copy */
	static int int2x;     /* 1 for the exact-2x mode: broadcast one source
	                        * pixel to a 32-bit pair, no xLut gather */
	uint32_t yAccum;
	int x, y;
	uint8_t *dstBase;
	DWORD perfStart = GetTickCount();
	unsigned perfElapsed;

	hwnd = CeGetWindow();
	hdc = GetDC(hwnd);
	if (!hdc)
		return;

	if (s_scaleGeomDirty)
	{
		PatBlt(hdc, 0, 0, s_physW, s_physH, BLACKNESS);
		ComputeScaleRect(s_physW, s_physH, &dstX, &dstY, &dstW, &dstH);
		if (dstW > 512) dstW = 512; /* defensive clamp against the fixed xLut size, not expected to trigger on this device */

		xStep = ((uint32_t)CE_NATIVE_W << 16) / (uint32_t)dstW;
		yStep = ((uint32_t)CE_NATIVE_H << 16) / (uint32_t)dstH;

		identityX = (dstW == CE_NATIVE_W);
		int2x = (s_scaleMode == CE_SCALE_2X && dstW == CE_NATIVE_W * 2);
		if (!identityX && !int2x)
			for (x = 0; x < dstW; x++)
				xLut[x] = (int)(((uint32_t)x * xStep) >> 16);

		s_scaleGeomDirty = 0;
	}

	dstBase = (uint8_t *)s_dibBits;
	yAccum = 0;
	for (y = 0; y < dstH; y++)
	{
		int srcY = (int)(yAccum >> 16);
		const uint16_t *srcRow = s_nativeBuf + srcY * CE_NATIVE_W;
		uint16_t *dstRow = (uint16_t *)(dstBase + (size_t)y * s_dibPitch);

		if (identityX)
			memcpy(dstRow, srcRow, (size_t)CE_NATIVE_W * 2);
		else if (int2x)
		{
			/* Exact 2x: dest pixel 2x and 2x+1 are both source pixel x,
			 * so there's no gather - broadcast the source uint16 into
			 * both halves of an aligned 32-bit store (little-endian: low
			 * half = lower address = dest pixel 2x). The second scanline
			 * for this source row is a plain memcpy of the row we just
			 * built. Same s_scale2x idea as sister PopNES. */
			if (y > 0 && srcY == (int)((yAccum - yStep) >> 16))
				memcpy(dstRow, (const uint8_t *)dstRow - s_dibPitch,
				       (size_t)CE_NATIVE_W * 4);
			else
			{
				uint32_t *dstRow32 = (uint32_t *)dstRow;
				for (x = 0; x < CE_NATIVE_W; x++)
				{
					uint32_t p = srcRow[x];
					dstRow32[x] = p | (p << 16);
				}
			}
		}
		else
		{
			/* 2 pixels/32-bit store: dstRow is always 4-byte aligned
			 * here (s_dibPitch is DWORD-aligned, dstBase is a DIB
			 * section's own bits, y*s_dibPitch is a multiple of that
			 * alignment) - ported from sister PopNES's
			 * ce_display.c CeDisplayBlitRGB565() (~18-20% measured
			 * blit-time drop on this same device family). xLut is a
			 * nearest-neighbor gather so the
			 * two source reads per pair can't be combined into one load,
			 * but the two writes can be packed into one aligned 32-bit
			 * store instead of two 16-bit ones. Assumes little-endian
			 * (true for every WinCE/ARM device this port runs on) - the
			 * low 16 bits of the packed word land at the lower address,
			 * i.e. pixel x, matching plain uint16_t store order. Odd
			 * dstW falls back to a single 16-bit store for the last
			 * pixel. */
			int pairLimit = dstW & ~1;
			uint32_t *dstRow32 = (uint32_t *)dstRow;
			for (x = 0; x < pairLimit; x += 2)
			{
				uint32_t px0 = srcRow[xLut[x]];
				uint32_t px1 = srcRow[xLut[x + 1]];
				dstRow32[x >> 1] = px0 | (px1 << 16);
			}
			for (; x < dstW; x++)
				dstRow[x] = srcRow[xLut[x]];
		}

		yAccum += yStep;
	}

	BitBlt(hdc, dstX, dstY, dstW, dstH, s_memDC, 0, 0, SRCCOPY);

	ReleaseDC(hwnd, hdc);

	s_lastImgW = dstW;
	s_lastImgH = dstH;

	perfElapsed = (unsigned)(GetTickCount() - perfStart);
#ifdef CE_PERF_DIAG
	s_blitAccumMs += perfElapsed;
#endif
	s_blitPerfAccumMs += perfElapsed;
	if (perfElapsed > s_blitPerfMaxMs)
		s_blitPerfMaxMs = perfElapsed;
	if (++s_blitPerfCount >= CE_BLIT_PERF_WINDOW)
	{
		s_blitPerfLastAvg = s_blitPerfAccumMs / s_blitPerfCount;
		s_blitPerfLastMax = s_blitPerfMaxMs;
		s_blitPerfAccumMs = 0;
		s_blitPerfMaxMs   = 0;
		s_blitPerfCount   = 0;
	}
}

/* GAPI's exclusive full-screen surface used to bypass hwnd's own paint
 * pipeline entirely once vid_init() had run, so a plain WM_PAINT (a
 * modal dialog covering part of the game window, then closing) never
 * needed to redraw anything - the last BlitToScreen() call's pixels
 * simply stayed put underneath. This GDI backend draws directly into
 * hwnd's own client area instead, so it's a normal window as far as
 * invalidation/repaint is concerned: ce_main.c's WM_PAINT handler calls
 * this to redraw the still-current s_nativeBuf content into the last
 * computed destination rect (see BlitToScreen()) whenever a ROM is
 * loaded, on top of its own plain black fill (which alone is enough
 * before any ROM has loaded, and also correctly covers the letterbox
 * border regardless of which part of the window WM_PAINT invalidated).
 * Bypasses Frame Skip on purpose - a forced repaint should never be
 * silently dropped. */
void CeVideoForceRepaint(void)
{
	BlitToScreen();
}

void CeVideoInvalidateLastImage(void)
{
	s_lastImgValid = 0;
}

int CeVideoGetLastImage(const void **pixels, unsigned *width, unsigned *height, unsigned *pitch)
{
	if (!s_lastImgValid || !s_dibBits || s_lastImgW <= 0 || s_lastImgH <= 0)
		return 0;
	*pixels = s_dibBits;
	*width  = (unsigned)s_lastImgW;
	*height = (unsigned)s_lastImgH;
	*pitch  = (unsigned)s_dibPitch;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Occupancy-driven frame skip (see this file's header comment)        */
/* ------------------------------------------------------------------ */

/* 0 = off (never skips); 1..30 = the maximum number of consecutive
 * frames vid_end() is allowed to skip the physical blit for while the
 * audio ring is comfortably ahead. */
static int s_frameSkip = 0;
static int s_skipStreak = 0;

/* Skip while the ring is at or below this fraction full - a rendered
 * frame is comparatively expensive (scale blit + GXBeginDraw/GXEndDraw),
 * so only skip when audio genuinely has a cushion to spare, not on
 * every dip. */
#define CE_FRAMESKIP_OCCUPANCY_THRESHOLD 50

void vid_begin()
{
	/* fb.enabled is gnuboy's own "do I have a live surface to draw
	 * into" flag - every other sys/ port (sdl2.c, sdl.c, x11, fbdev,
	 * ...) only ever flips it off for a genuinely absent/hidden
	 * window, never per-frame. An earlier version of this file
	 * repurposed it as the frame-skip gate instead: setting it to 0
	 * made lcd_refreshline() (lcd.c) bail out before updatepatpix()
	 * ever ran for that frame, so the tile-pattern cache and every
	 * other piece of gnuboy's own per-scanline state fell behind by
	 * however many frames got skipped in a row - a real, separate bug
	 * (see the port's dev notes). fb.enabled is now always kept at 1 while the
	 * display is open (see vid_init()/vid_close()) - gnuboy renders
	 * every single GB/GBC frame into s_nativeBuf exactly like every
	 * other port, unconditionally, regardless of what Frame Skip below
	 * decides to do with the *physical* screen. */
}

/* 2026-08-24 (the port's dev notes, session 16): this file briefly had a second
 * piece here - a per-pixel multi-frame accumulate/average step run on
 * every skipped frame, meant to approximate real LCD persistence
 * blending for GBC titles that dither across frames. It was removed:
 * the hardware report that motivated it turned out (per the port's dev notes' own
 * correction note) to have been about a *different* CE port entirely,
 * not gnuboy, so there's no confirmed gnuboy case that actually needs
 * it; and CE_PERF_DIAG measurement (the same investigation) showed its
 * full-buffer per-pixel cost, run unconditionally on every skipped
 * frame, was comparable to the BlitToScreen() cost it was trying to let
 * Frame Skip avoid - so Frame Skip was buying essentially no real
 * speedup with it in place. Frame Skip below is back to the simple
 * original design: just decide whether *this* frame's already-fully-
 * rendered s_nativeBuf gets physically blitted or not. */
void vid_end()
{
	if (s_frameSkip <= 0)
	{
		BlitToScreen();
		s_lastImgValid = 1;
		return;
	}

	if (s_skipStreak < s_frameSkip &&
	    CeAudioGetBufferOccupancyPercent() <= CE_FRAMESKIP_OCCUPANCY_THRESHOLD)
	{
		s_skipStreak++;
		return;
	}

	s_skipStreak = 0;
	BlitToScreen();
	s_lastImgValid = 1;
}

/* Only called when fb.indexed is set (see lcd.c's updatepalette()) - the
 * native buffer here is always RGB565 truecolor, so this never runs. */
void vid_setpal(int i, int r, int g, int b)
{
	(void)i; (void)r; (void)g; (void)b;
}

void vid_settitle(char *title)
{
	HWND hwnd = CeGetWindow();
	if (hwnd && title)
	{
		wchar_t wtitle[256];
		MultiByteToWideChar(CP_ACP, 0, title, -1, wtitle, 256);
		SetWindowTextW(hwnd, wtitle);
	}
}

/* ------------------------------------------------------------------ */
/* Video Config dialog                                                 */
/* ------------------------------------------------------------------ */

/* Direct pointer into lcd.c's `colorfilter` rcvar (RCV_BOOL, a plain
 * `int` under the hood - see rc.h) - resolved once CeVideoInit() runs,
 * which this port's ce_main.c call order guarantees happens after
 * rc_exportvars(lcd_exports) has registered it. `scale` is also read
 * once here, just to force it to 1 (see this file's header comment) -
 * gnuboy's own scale rcvar plays no further part in this port, Scale is
 * now this file's own s_scaleMode instead. */
static int *s_pColorFilter = NULL;

/* User report: toggling the Color Filter checkbox had no visible effect
 * on an already-running game. Flipping *s_pColorFilter alone only
 * changes what lcd.c's updatepalette() computes for palette entries
 * written *after* this point - it does nothing to the GB/GBC palette
 * slots (PAL1/PAL2/PAL4) already cached from whatever the game wrote
 * earlier, which for most games only happens once during its own
 * startup and never again during normal play. lcd.c's pal_dirty()
 * (already used by lcd_reset() for exactly this reason) recomputes all
 * 64 slots from the current `usefilter`/CGB register state immediately,
 * which is what actually makes the toggle visible right away instead of
 * only on the next full palette write (if the game ever makes one
 * again) or the next ROM load. Safe to call with no ROM loaded (Video
 * Config is reachable from the main menu either way) - lcd.c's static
 * state is all zero-initialized until the first lcd_reset(), and
 * pal_dirty()/updatepalette() only touch that same static state. */
static void ToggleColorFilter(void)
{
	if (!s_pColorFilter)
		return;
	*s_pColorFilter = !*s_pColorFilter;
	pal_dirty();
}

void CeVideoInit(void)
{
	int *pScale = (int *)rc_getmem("scale");
	int savedMode;

	if (pScale)
		*pScale = 1; /* delegate_scaling always draws native 1x - see vid_init() */

	s_pColorFilter = (int *)rc_getmem("colorfilter");
	if (s_pColorFilter)
		*s_pColorFilter = CeConfigGetInt("VideoColorFilter", *s_pColorFilter);

	savedMode = CeConfigGetInt("VideoScaleMode", (int)s_scaleMode);
	switch (savedMode)
	{
	case CE_SCALE_1TO1:
	case CE_SCALE_2X:
	case CE_SCALE_HALFSTRETCH:
	case CE_SCALE_EXPAND:
		s_scaleMode = (CeScaleMode)savedMode;
		break;
	default:
		s_scaleMode = CE_SCALE_EXPAND; /* unrecognised value in an old/corrupt config file */
		break;
	}

	s_frameSkip = CeConfigGetInt("VideoFrameSkip", s_frameSkip);
	s_scaleGeomDirty = 1; /* force BlitToScreen() to (re)compute geometry for whichever mode was just loaded */

	/* "Enable Debug Log" checkbox (user request) - applied here because
	 * this is the first point after CeConfigLoad() where a persisted
	 * flag can be read (WinMain calls CeVideoInit() right after
	 * CeConfigLoad()). Off by default (user request) - see ce_log.h. */
	CeLogSetEnabled(CeConfigGetInt("DebugLogEnabled", 0));

	CeLog("CeVideoInit: loaded scaleMode=%d colorfilter=%d frameSkip=%d debugLog=%d from config file",
	      (int)s_scaleMode, s_pColorFilter ? *s_pColorFilter : -1, s_frameSkip, CeLogIsEnabled());
}

static void CeVideoSaveConfig(void)
{
	if (s_pColorFilter)
		CeConfigSetInt("VideoColorFilter", *s_pColorFilter);
	CeConfigSetInt("VideoScaleMode", (int)s_scaleMode);
	CeConfigSetInt("VideoFrameSkip", s_frameSkip);
	CeConfigSetInt("DebugLogEnabled", CeLogIsEnabled());
	CeConfigSave();

	CeLog("CeVideoSaveConfig: saved scaleMode=%d colorfilter=%d frameSkip=%d debugLog=%d",
	      (int)s_scaleMode, s_pColorFilter ? *s_pColorFilter : -1, s_frameSkip, CeLogIsEnabled());
}

static void UpdateScaleLabel(HWND hDlg)
{
	SetWindowTextW(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE), kScaleLabels[ScaleModeToIndex(s_scaleMode)]);
}

static void StepScaleMode(HWND hDlg, int delta)
{
	int idx = (ScaleModeToIndex(s_scaleMode) + delta + CE_SCALE_CHOICE_COUNT) % CE_SCALE_CHOICE_COUNT;
	s_scaleMode = kScaleOrder[idx];
	s_scaleGeomDirty = 1; /* force BlitToScreen() to recompute geometry and reclear the border once - see its declaration */
	UpdateScaleLabel(hDlg);
}

/* Shows the currently active language's own name, matching the other
 * VC_* value labels (Scale:, Frame Skip:) which display the current
 * setting rather than the choice you'd switch to. The Shinonome
 * bitmap font (ce_bmpfont.c, baked into the binary rather than loaded
 * from a file) renders the Japanese glyphs directly, so no romaji
 * fallback is needed any more. */
static void UpdateLanguageLabel(HWND hDlg)
{
	SetWindowTextW(GetDlgItem(hDlg, IDC_VC_JAPANESE),
	               CeLangIsJapanese() ? L"\x65e5\x672c\x8a9e" : L"English");
}

static void UpdateFrameSkipLabel(HWND hDlg)
{
	wchar_t text[8];
	if (s_frameSkip <= 0)
		_snwprintf(text, 8, L"Off");
	else
		_snwprintf(text, 8, L"%d", s_frameSkip);
	SetWindowTextW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_LABEL), text);
}

#define FRAMESKIP_REPEAT_TIMER_ID     1
#define FRAMESKIP_REPEAT_INITIAL_MS   500
#define FRAMESKIP_REPEAT_INTERVAL_MS  120

static WNDPROC s_origFrameSkipBtnProc = NULL;
static int     s_frameSkipRepeatDir   = 0;
static int     s_frameSkipRepeatFast  = 0;

static LRESULT CALLBACK FrameSkipButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_LBUTTONDOWN:
		s_frameSkipRepeatDir  = (GetDlgCtrlID(hwnd) == IDC_VC_FRAMESKIP_UP) ? 1 : -1;
		s_frameSkipRepeatFast = 0;
		SetTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID, FRAMESKIP_REPEAT_INITIAL_MS, NULL);
		break;

	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		KillTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID);
		s_frameSkipRepeatDir = 0;
		break;

	case WM_TIMER:
		if (wParam == FRAMESKIP_REPEAT_TIMER_ID && s_frameSkipRepeatDir != 0)
		{
			if (!s_frameSkipRepeatFast)
			{
				s_frameSkipRepeatFast = 1;
				SetTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID, FRAMESKIP_REPEAT_INTERVAL_MS, NULL);
			}

			if (s_frameSkipRepeatDir > 0)
			{
				if (s_frameSkip < 30) s_frameSkip++;
			}
			else
			{
				if (s_frameSkip > 0) s_frameSkip--;
			}
			UpdateFrameSkipLabel(GetParent(hwnd));
		}
		break;
	}

	return CallWindowProc(s_origFrameSkipBtnProc, hwnd, msg, wParam, lParam);
}

static void  RefreshVideoConfigLanguage(HWND hDlg);
static void  ToggleLanguage(HWND hDlg);

/* Labels repainted by WM_PAINT via CeBmpFontPaintLabel(). IDC_VC_LBL_SCALE
 * ("Scale:") is never translated but still needs a real ID/hide-and-
 * repaint cycle like ce_input.c's A/B labels - see ce_resource.h. The
 * PUSHBUTTONs and CHECKBOXes (IDC_VC_SCALE_VALUE, IDC_VC_COLORFILTER,
 * IDC_VC_FRAMESKIP_LABEL, IDC_VC_JAPANESE, IDC_VC_DEBUGLOG, IDOK)
 * are all BS_OWNERDRAW (ce_res.rc) and redraw themselves via
 * WM_DRAWITEM instead. */
static const int kVideoLabelIds[] = { IDC_VC_LBL_SCALE, IDC_VC_LBL_FRAMESKIP };
#define CE_VIDEO_LABEL_COUNT (sizeof(kVideoLabelIds) / sizeof(kVideoLabelIds[0]))

#define WM_SETVIDEOFOCUS (WM_APP + 202)

static int VideoNeighborDown(int id)
{
	switch (id)
	{
	case IDC_VC_SCALE_VALUE:       return IDC_VC_COLORFILTER;
	case IDC_VC_COLORFILTER:       return IDC_VC_FRAMESKIP_LABEL;
	case IDC_VC_FRAMESKIP_LABEL:   return IDC_VC_JAPANESE;
	case IDC_VC_JAPANESE:          return IDC_VC_DEBUGLOG;
	case IDC_VC_DEBUGLOG:          return IDOK;
	case IDOK:                     return IDC_VC_SCALE_VALUE;
	}
	return id;
}

static int VideoNeighborUp(int id)
{
	switch (id)
	{
	case IDC_VC_SCALE_VALUE:       return IDOK;
	case IDC_VC_COLORFILTER:       return IDC_VC_SCALE_VALUE;
	case IDC_VC_FRAMESKIP_LABEL:   return IDC_VC_COLORFILTER;
	case IDC_VC_JAPANESE:          return IDC_VC_FRAMESKIP_LABEL;
	case IDC_VC_DEBUGLOG:          return IDC_VC_JAPANESE;
	case IDOK:                     return IDC_VC_DEBUGLOG;
	}
	return id;
}

static WNDPROC s_pVideoOrigProc = NULL;

static LRESULT CALLBACK VideoCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int id = GetDlgCtrlID(hWnd);

	if (message == WM_GETDLGCODE)
	{
		return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
	}
	else if (message == WM_KEYDOWN)
	{
		switch (wParam)
		{
		case VK_UP:
			SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborUp(id)));
			return 0;

		case VK_DOWN:
			SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborDown(id)));
			return 0;

		case VK_LEFT:
		case VK_RIGHT:
			if (id == IDC_VC_SCALE_VALUE)
			{
				StepScaleMode(GetParent(hWnd), (wParam == VK_LEFT) ? -1 : 1);
			}
			else if (id == IDC_VC_FRAMESKIP_LABEL)
			{
				if (wParam == VK_LEFT)
				{
					if (s_frameSkip > 0) s_frameSkip--;
				}
				else
				{
					if (s_frameSkip < 30) s_frameSkip++;
				}
				UpdateFrameSkipLabel(GetParent(hWnd));
			}
			else if (id == IDC_VC_JAPANESE)
			{
				ToggleLanguage(GetParent(hWnd));
			}
			return 0;

		case VK_RETURN:
			if (id == IDC_VC_SCALE_VALUE || id == IDC_VC_FRAMESKIP_LABEL || id == IDC_VC_JAPANESE)
			{
				SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborDown(id)));
			}
			else if (id == IDC_VC_COLORFILTER)
			{
				/* Same reasoning as WM_COMMAND's IDC_VC_COLORFILTER case -
				 * toggle the real data (and force a palette refresh - see
				 * ToggleColorFilter()) and repaint, not BM_GETCHECK/
				 * BM_SETCHECK. */
				ToggleColorFilter();
				InvalidateRect(hWnd, NULL, TRUE);
			}
			else if (id == IDC_VC_DEBUGLOG)
			{
				CeLogSetEnabled(!CeLogIsEnabled());
				InvalidateRect(hWnd, NULL, TRUE);
			}
			else if (id == IDOK)
			{
				SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
			}
			return 0;

		case VK_ESCAPE:
			SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
			return 0;
		}
	}

	return CallWindowProc(s_pVideoOrigProc, hWnd, message, wParam, lParam);
}

static void ApplyVideoConfigLanguage(HWND hDlg)
{
	unsigned i;

	if (CeLangIsJapanese())
	{
		SetDlgItemTextW(hDlg, IDC_VC_COLORFILTER,    L"\x30ab\x30e9\x30fc\x30d5\x30a3\x30eb\x30bf\x30fc"); /* カラーフィルター */
		SetDlgItemTextW(hDlg, IDC_VC_LBL_FRAMESKIP,  L"\x30d5\x30ec\x30fc\x30e0\x30b9\x30ad\x30c3\x30d7\x3a"); /* フレームスキップ: */
		SetDlgItemTextW(hDlg, IDC_VC_DEBUGLOG,       L"\x30c7\x30d0\x30c3\x30b0\x30ed\x30b0\x3092\x6709\x52b9\x306b\x3059\x308b"); /* デバッグログを有効にする */
		SetDlgItemTextW(hDlg, IDOK,     L"\x6c7a\x5b9a");                            /* 決定 */
	}
	else
	{
		SetDlgItemTextW(hDlg, IDC_VC_COLORFILTER,    L"Color Filter");
		SetDlgItemTextW(hDlg, IDC_VC_LBL_FRAMESKIP,  L"Frame Skip:");
		SetDlgItemTextW(hDlg, IDC_VC_DEBUGLOG,       L"Enable Debug Log");
		SetDlgItemTextW(hDlg, IDOK,     L"OK");
	}

	/* IDC_VC_COLORFILTER/IDC_VC_DEBUGLOG (CHECKBOX)
	 * and IDOK (PUSHBUTTON) are BS_OWNERDRAW - their text above is read
	 * straight back out by WM_DRAWITEM via GetWindowTextW(), no hiding needed.
	 * IDC_VC_LBL_FRAMESKIP is a plain LTEXT, hidden here like
	 * ce_input.c's row captions. */
	for (i = 0; i < CE_VIDEO_LABEL_COUNT; i++)
		ShowWindow(GetDlgItem(hDlg, kVideoLabelIds[i]), SW_HIDE);
}

static void RefreshVideoConfigLanguage(HWND hDlg)
{
	UpdateLanguageLabel(hDlg);
	ApplyVideoConfigLanguage(hDlg);
}

static void ToggleLanguage(HWND hDlg)
{
	CeLangSetJapanese(!CeLangIsJapanese());
	RefreshVideoConfigLanguage(hDlg);
}

static INT_PTR CALLBACK VideoConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		/* No CheckDlgButton() here for IDC_VC_COLORFILTER/
		 * IDC_VC_DEBUGLOG - initial check state comes straight
		 * from s_pColorFilter/CeLogIsEnabled() the first
		 * time WM_DRAWITEM paints them, same as every later toggle. */
		UpdateScaleLabel(hDlg);
		UpdateFrameSkipLabel(hDlg);
		RefreshVideoConfigLanguage(hDlg);

		s_frameSkipRepeatDir = 0;
		s_origFrameSkipBtnProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_UP), GWLP_WNDPROC);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_UP), GWLP_WNDPROC, (LONG_PTR)FrameSkipButtonSubclassProc);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_DOWN), GWLP_WNDPROC, (LONG_PTR)FrameSkipButtonSubclassProc);

		s_pVideoOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE),       GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_COLORFILTER),       GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_LABEL),   GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_JAPANESE),          GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_DEBUGLOG),         GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDOK),                     GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);

		SetActiveWindow(hDlg);
		SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
		PostMessage(hDlg, WM_SETVIDEOFOCUS, 0, 0);
		return FALSE;
	}

	case WM_DRAWITEM:
	{
		const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
		if (dis->CtlID == IDC_VC_COLORFILTER)
			CeBmpFontDrawOwnerCheckbox(dis, s_pColorFilter && *s_pColorFilter);
		else if (dis->CtlID == IDC_VC_DEBUGLOG)
			CeBmpFontDrawOwnerCheckbox(dis, CeLogIsEnabled());
		else
			CeBmpFontDrawOwnerButton(dis);
		return TRUE;
	}

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hDlg, &ps);
		unsigned i;
		for (i = 0; i < CE_VIDEO_LABEL_COUNT; i++)
			CeBmpFontPaintLabel(hdc, hDlg, kVideoLabelIds[i]);
		EndPaint(hDlg, &ps);
		return TRUE;
	}

	case WM_ACTIVATE:
		if (LOWORD(wParam) != WA_INACTIVE)
		{
			SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
			PostMessage(hDlg, WM_SETVIDEOFOCUS, 0, 0);
		}
		break;

	case WM_SETVIDEOFOCUS:
		SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
		break;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_VC_SCALE_MINUS:
			StepScaleMode(hDlg, -1);
			return TRUE;

		case IDC_VC_SCALE_PLUS:
			StepScaleMode(hDlg, 1);
			return TRUE;

		case IDC_VC_COLORFILTER:
			/* Toggle the real backing data (and force a palette refresh
			 * - see ToggleColorFilter()) and just repaint - not
			 * CheckDlgButton()/BM_SETCHECK, see CeBmpFontDrawOwnerCheckbox()'s
			 * header comment for why those don't work as a state store
			 * once this control is BS_OWNERDRAW. */
			ToggleColorFilter();
			InvalidateRect(GetDlgItem(hDlg, IDC_VC_COLORFILTER), NULL, TRUE);
			return TRUE;

		case IDC_VC_LANG_MINUS:
		case IDC_VC_LANG_PLUS:
			ToggleLanguage(hDlg);
			return TRUE;

		case IDC_VC_DEBUGLOG:
			/* Toggle ce_log.c's enable flag directly and repaint - same
			 * BS_OWNERDRAW / direct-backing-data pattern as Color Filter
			 * above. Persisted by CeVideoSaveConfig()
			 * on OK. */
			CeLogSetEnabled(!CeLogIsEnabled());
			InvalidateRect(GetDlgItem(hDlg, IDC_VC_DEBUGLOG), NULL, TRUE);
			return TRUE;

		case IDC_VC_FRAMESKIP_DOWN:
			if (s_frameSkip > 0)
				s_frameSkip--;
			UpdateFrameSkipLabel(hDlg);
			return TRUE;

		case IDC_VC_FRAMESKIP_UP:
			if (s_frameSkip < 30)
				s_frameSkip++;
			UpdateFrameSkipLabel(hDlg);
			return TRUE;

		case IDOK:
		case IDCANCEL:
			/* Physical Back (IDCANCEL) commits and closes, same as OK -
			 * this device has no meaningful "discard changes" gesture.
			 * Scale/Color Filter/Open Last Folder are already applied
			 * live; this just persists everything to disk. */
			CeVideoSaveConfig();
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		return FALSE;

	default:
		return FALSE;
	}
	return FALSE;
}

void CeShowVideoConfigDialog(HWND owner)
{
	DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), MAKEINTRESOURCEW(IDD_VIDEOCONFIG),
	           owner, VideoConfigDlgProc);
}
