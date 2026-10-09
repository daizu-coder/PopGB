/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * waveOut-backed audio output for PopGB, plus the native Sound
 * Config dialog.
 *
 * sound_mix() (sound.c, core, unmodified) hands its samples to
 * pcm_submit() (ce_audio.c), which only copies them into a
 * single-producer/single-consumer ring buffer and returns. A dedicated
 * thread (CeAudioThreadProc) drains the ring, applies Volume/Bits/
 * Quality and makes the waveOutWrite() calls, so the emulation thread
 * never waits on the waveOut driver - see ce_audio.c's header comment
 * for why. The ring size is Sound Config's Buffer setting (32/64/128/
 * 256 KB, 64 KB by default).
 *
 * There is no resampler: gnuboy generates samples at whatever rate
 * pcm.hz names, so a Rate change just sets pcm.hz, calls sound_reset()
 * and reopens the waveOut device at the new rate.
 *
 * The dialog's control mechanics (the "-/value/+" spinners, the
 * physical-key focus loop) are ported from the sister ports' Sound
 * Config.
 */
#ifndef CE_AUDIO_H
#define CE_AUDIO_H

#include <windows.h>

/* Loads saved Volume/Rate/Bits/Quality from the config file. Call once
 * from WinMain, before the first ROM load (pcm_init() reads these). */
void CeAudioInit(void);

void CeShowSoundConfigDialog(HWND owner);

/* Pre-fills the ring with `ms` milliseconds of true silence at the
 * currently configured rate. Call once, right before starting/resuming
 * emu_run() (WinMain's own loop - see ce_main.c) - covers the ring's
 * otherwise-zero cushion (pcm_pause(1) just reset it to empty while the
 * menu was up) against the first frame or two of freshly-(re)started
 * emulation taking longer than usual (cold GDI/DIB/cache effects right
 * after a ROM loads or a dialog closes - a known, previously-harmless
 * one-off cost, see the port's dev notes) without that turning into an
 * audible underrun before real audio has had a chance to build up its
 * own backlog. Safe to call every time gameplay (re)starts, regardless
 * of whether a device is open (no-op if not). */
void CeAudioPrimeSilence(unsigned ms);

/* How full the audio ring is - not-yet-played audio as a percentage of
 * the current ring size, 0..100. Consumed by ce_video.c's vid_end():
 * while this is at or below its threshold, vid_end() skips the blit to
 * the screen, up to the Frame Skip setting's count in a row, then forces
 * one so audio doesn't play against a completely frozen screen. Safe to
 * call every frame regardless of whether a device is open (0 if not). */
unsigned CeAudioGetBufferOccupancyPercent(void);

/* True while a waveOut device is actually open (pcm_init()'s
 * OpenWaveOut() succeeded and pcm_close() hasn't run yet). ce_main.c's
 * doevents() frame limiter uses this to decide between ring-backed
 * pacing and its GetTickCount() fallback - ported from the sister PopSG
 * port's own CeAudioIsActive() (dev notes round 38). */
int CeAudioIsActive(void);

/* Milliseconds of not-yet-played audio currently sitting in the ring
 * buffer, at the currently configured output rate (0 if no device is
 * open). The audio thread (CeAudioThreadProc) drains the ring at
 * exactly the output rate = real time, so ce_main.c's frame limiter
 * holds a frame back whenever this climbs above a small high-water mark
 * - same role as PopSG's own CeAudioGetBufferedMs() (dev notes round
 * 38), adapted to this port's ring: s_ring holds raw generation-rate
 * mono samples (1 byte = 1 sample at s_rateHz, pre-Volume/Bits/Quality),
 * so the backlog byte count *is* the backlog sample count already. */
unsigned CeAudioGetBufferedMs(void);

#ifdef CE_PERF_DIAG
/* Diagnostic-only (see sys.h/Makefile.ce), all "get and reset to 0":
 *   - CeAudioGetAndResetSubmitMs(): total ms spent inside pcm_submit()
 *     (the ring-buffer memcpy on the CPU/PPU thread) since the last call
 *     - expected to be near-zero every frame; a nonzero value here would
 *       mean the ring copy itself, not the audio thread, is stealing
 *       emulation time.
 *   - CeAudioGetAndResetOverflowDropBytes()/Count(): bytes/events where
 *     pcm_submit() found the ring already full and silently dropped the
 *     overflow rather than block (see this file's pcm_submit() comment)
 *     - a nonzero count here means the audio *drain* thread is falling
 *       behind generation, which would explain audible dropouts.
 *   - CeAudioGetAndResetTimeoutDropCount(): times CeAudioDrainAndSubmit()
 *     (the dedicated audio thread) gave up waiting 100ms for a free
 *     waveOut header and dropped that chunk - each occurrence already
 *     logs its own line immediately; this counter just lets the periodic
 *     summary report a count alongside the other diagnostics without
 *     re-parsing the log. Written from the audio thread, read/reset from
 *     the CPU/PPU thread, so it uses Interlocked* rather than plain
 *     reads/writes (unlike the ring head/tail counters, this one isn't
 *     safe as a lock-free monotonic counter across threads on its own). */
unsigned CeAudioGetAndResetSubmitMs(void);
unsigned CeAudioGetAndResetOverflowDropBytes(void);
unsigned CeAudioGetAndResetOverflowDropCount(void);
unsigned CeAudioGetAndResetTimeoutDropCount(void);
#endif

#endif
