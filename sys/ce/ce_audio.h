/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * waveOut-backed audio output for PopGB, plus the native Sound
 * Config dialog. Design note vs. the sister ports' shared ce_audio.c,
 * which this dialog's UI mechanics were ported
 * from: that file needs a lock-protected ring buffer and a dedicated
 * drain thread because its libretro cores hand over audio in bulk
 * batches from inside retro_run(), at a fixed rate the device's own
 * output rate must be resampled to. gnuboy is different on both counts
 * - sound_mix() (sound.c) calls pcm_submit() (this file) directly,
 * synchronously, exactly once per pcm.buf's worth of samples, already
 * generated at whatever rate pcm.hz names (sound_reset() recomputes the
 * generation rate from it - see the port's dev notes) - so there is no separate
 * core rate to resample from, and the existing single-thread,
 * bounded-wait-for-a-free-waveOut-buffer design (proven on real
 * hardware by this port's previous minimal sys/ce/ce_audio.c) already
 * has everything the sister ports needed a thread for. This file keeps
 * that design and just adds more buffers (for a finer-grained occupancy
 * reading) plus Volume/Bits/Quality post-processing on the copy into
 * each waveOut buffer.
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

/* Fraction of this port's waveOut buffers currently queued/playing,
 * 0..100 - consumed by ce_video.c's vid_begin() to decide whether to
 * skip this frame's rendering (see the port's dev notes' frame-skip design:
 * skip while audio is comfortably ahead, force a render if the skip
 * streak hits the Frame Skip setting's cap so audio doesn't play
 * against a completely frozen screen). Safe to call every frame
 * regardless of whether a device is open (0 if not). */
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
