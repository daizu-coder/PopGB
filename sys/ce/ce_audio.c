/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * ce_audio.c - waveOut audio backend for PopGB.
 *
 * Session-N rewrite (see the port's dev notes): the previous design had
 * sound_mix() (sound.c, core, unmodified) call pcm_submit() (this file)
 * synchronously, once per emu_run() frame, doing the Volume/Bits/Quality
 * copy *and* waveOutWrite() *and* a bounded wait for a free waveOut
 * header all on the same thread that's also running the CPU/PPU
 * interpreter. Two things made that design a real-hardware problem
 * instead of just an academic one:
 *   1. pcm_submit() submits *whatever partial amount* of pcm.pos is
 *      present, not just full pcm.len buffers - so it returns 1 (success)
 *      on essentially every single call, which meant emu.c's own
 *      `if (!pcm_submit()) { delay = framelen - sys_elapsed(...); ...}`
 *      frame-pacing branch (its *only* throttle) almost never ran.
 *   2. With that throttle effectively disabled, whatever real time
 *      waveOutWrite() and the header round-robin wait actually took on
 *      this device's driver became the *entire* per-frame budget -
 *      real-hardware logging showed a rock-steady ~170ms/frame
 *      (~10x the intended ~16.7ms) from the very first frame, on every
 *      ROM, unaffected by every other timing/threading fix tried.
 * This is the exact same class of bug an earlier prototype hit and
 * fixed (there, turning the sound off made games run abnormally fast -
 * frame pacing there also turned out to depend entirely on
 * the audio path creating backpressure) - its fix was to move actual
 * waveOut I/O to a dedicated thread fed by a ring buffer, decoupling it
 * from the emulation thread entirely. This file now does the same:
 *   - pcm_submit() (called from sound.c) just memcpy's into a
 *     single-producer/single-consumer ring buffer and returns - no
 *     waveOut call, no wait, on this thread, ever.
 *   - A dedicated thread (CeAudioThreadProc) drains the ring, does the
 *     Volume/Bits/Quality copy, and owns the waveOut round-robin +
 *     wait-for-free-header logic the old pcm_submit() used to do inline.
 *   - CeAudioGetBufferOccupancyPercent() now reports the *ring's* fill
 *     level (how much generated-but-not-yet-played audio is backed up),
 *     the same signal that earlier prototype's ring occupancy check used.
 *   - Because pcm_submit() no longer blocks, emu.c's own audio-coupled
 *     pacing branch is even less load-bearing than before - ce_main.c's
 *     doevents() now does its own independent GetTickCount()-based
 *     frame pacing (matching that prototype's fix for the same reason: don't let
 *     frame rate depend on the audio path's state at all).
 *
 * Volume/Bits/Quality post-processing on the copy into each waveOut
 * buffer (Sound Config, IDD_SOUNDCONFIG in ce_res.rc) and Rate
 * re-plumbing through gnuboy's own pcm.hz + sound_reset() (which changes
 * the *generation* rate, so the waveOut device is simply reopened at the
 * same rate - no resampler needed) are unchanged from before, just moved
 * from pcm_submit() into the audio thread's drain loop.
 *
 * The dialog's control mechanics (subclassed WS_TABSTOP "-/value/+"
 * spinners, physical-key focus loop, auto-repeat on Volume's -/+) are
 * ported from the sister ports' own SoundConfigDlgProc/SoundCtrlProc/
 * VolumeButtonSubclassProc essentially unchanged.
 */
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "ce_audio.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_resource.h"

#include "defs.h"
#include "rc.h"
#include "pcm.h"
#include "sound.h"

#define CE_AUDIO_BUF_LEN  4096   /* bytes - generous upper bound for one drain chunk of mono 8-bit PCM at any supported rate */
#define CE_AUDIO_NUM_BUFS 6      /* more than the previous minimal port's 3, for a finer occupancy reading */

struct pcm pcm;

static byte s_pcmbuf[CE_AUDIO_BUF_LEN];

typedef struct
{
    WAVEHDR hdr;
    int16_t data[CE_AUDIO_BUF_LEN]; /* wide enough for the 16-bit-output case; 8-bit output only uses the first CE_AUDIO_BUF_LEN bytes of it */
    int     queued;
} CeAudioBuf;

static HWAVEOUT   s_hWaveOut = NULL;
static CeAudioBuf s_bufs[CE_AUDIO_NUM_BUFS];
static unsigned   s_bufIdx = 0;

/* ------------------------------------------------------------------ */
/* Ring buffer + dedicated drain thread (see this file's header)       */
/* ------------------------------------------------------------------ */

/* Ring buffer, sized like that earlier prototype's own ring: give the drain thread
 * plenty of backlog so a transient scheduling delay never starves
 * waveOut. The *active* size is now runtime-selectable from Sound Config
 * (user request - "サウンドバッファ変更機能"): s_ring[] is allocated at
 * the maximum, s_ringBytes/s_ringMask pick the power-of-two portion
 * actually in use (kBufferChoices[] below - 32/64/128/256 KB). All ring
 * math uses s_ringMask / s_ringBytes rather than the compile-time max.
 * 256 KB (the max, and the previous fixed value) is ~5.9s of latency
 * ceiling at the highest supported rate (44100 B/s, 8-bit mono). The
 * default is 64 KB (~1.5s ceiling): the drain thread paces to the device
 * rate, so ring size is a latency *ceiling*, not a steady-state target -
 * 64 KB keeps that ceiling tight while still absorbing an ordinary
 * scheduling hiccup, and a bad hiccup at 64 KB drops a sample (a brief
 * click) rather than carrying seconds of extra lag until the next menu
 * open (which discards the ring - see pcm_pause()). User request. */
#define CE_AUDIO_RING_BYTES_MAX     262144u
#define CE_AUDIO_RING_BYTES_MIN     32768u
#define CE_AUDIO_RING_BYTES_DEFAULT 65536u

static byte s_ring[CE_AUDIO_RING_BYTES_MAX];
static unsigned s_ringBytes = CE_AUDIO_RING_BYTES_DEFAULT;
static unsigned s_ringMask  = CE_AUDIO_RING_BYTES_DEFAULT - 1;
/* Single-producer (pcm_submit(), the CPU/PPU thread)/single-consumer
 * (CeAudioThreadProc) ring: s_ringHead is only ever written by the
 * producer, s_ringTail only ever written by the consumer, both read
 * freely by the other side - the standard lock-free SPSC pattern, safe
 * here without an explicit barrier/critical section since both are
 * plain word-sized counters (monotonically increasing, wrapping via
 * unsigned overflow) on a single-core device. */
static volatile unsigned s_ringHead = 0;
static volatile unsigned s_ringTail = 0;

static HANDLE          s_audioThread    = NULL;
static volatile LONG   s_audioThreadRun = 0;

#ifdef CE_PERF_DIAG
/* Declared up here (not down by CeAudioGetAndResetTimeoutDropCount()
 * below) so CeAudioDrainAndSubmit() - which runs on the audio thread and
 * appears earlier in this file - can increment it. */
static volatile LONG s_timeoutDropCount = 0;
#endif

rcvar_t pcm_exports[] =
{
    RCV_END
};

/* ------------------------------------------------------------------ */
/* Persisted settings                                                  */
/* ------------------------------------------------------------------ */

static const unsigned kRateChoices[] = { 8000, 11025, 22050, 32000, 44100 };
#define CE_RATE_CHOICE_COUNT (sizeof(kRateChoices) / sizeof(kRateChoices[0]))

/* Sound buffer (ring) size choices - all powers of two so the ring's
 * `& s_ringMask` wraparound stays valid. See CE_AUDIO_RING_BYTES_MAX. */
static const unsigned kBufferChoices[] = { 32768, 65536, 131072, 262144 };
#define CE_BUFFER_CHOICE_COUNT (sizeof(kBufferChoices) / sizeof(kBufferChoices[0]))

static int s_volumeLevel = 10;   /* 0..10; 0 = silent */
static int s_volumeScale = 256;  /* 0..256 fixed-point, recomputed from the above */
static int s_rateHz      = 44100;
static int s_bitDepth    = 16;   /* 8 or 16 - waveOut output format only, pcm.buf itself is always 8-bit unsigned */
static int s_smooth      = 1;    /* 1 = Smooth (light 2-tap average), 0 = Fast (no filtering) */

/* Held pending until commit (OK/Back), same reasoning as Sample
 * Emulater's own s_pendingBitDepth (ce_audio.c): pcm_submit() reads
 * s_bitDepth on every buffer it fills, so flipping it mid-dialog while
 * the device is still open in the old format would feed mismatched
 * data into a still-old-format waveOut buffer. Declared up here (not
 * just above their own Update*Label functions) so CurrentRateStepIndex()
 * below can read s_pendingRateHz - it used to read the committed
 * s_rateHz instead, which meant every "-"/"+" press recomputed the same
 * step relative to the value the dialog was opened with instead of the
 * value it had just been moved to, so repeated presses got stuck one
 * step away from the opening value instead of walking further down/up
 * kRateChoices (hardware-reported: 8000/11025/32000 unreachable from a
 * dialog opened at 44100). */
static int s_pendingBitDepth;
static int s_pendingRateHz;
/* Held pending until commit, same reasoning as s_pendingRateHz: the
 * audio drain thread reads s_ringBytes/s_ringMask live. */
static int s_pendingBufferBytes;

static void RecomputeVolumeScale(void)
{
    s_volumeScale = (s_volumeLevel * 256) / 10;
}

void CeAudioInit(void)
{
    unsigned savedBuf;
    unsigned i;

    s_volumeLevel = CeConfigGetInt("SoundVolume", s_volumeLevel);
    s_rateHz      = CeConfigGetInt("SoundRate", s_rateHz);
    s_bitDepth    = CeConfigGetInt("SoundBits", s_bitDepth);
    s_smooth      = CeConfigGetInt("SoundSmooth", s_smooth);

    /* Applied straight to the plain globals here (no lock, no reset) -
     * this runs from WinMain before pcm_init() creates the drain thread,
     * so nothing else touches the ring yet. Later changes from Sound
     * Config go through ApplyRingBytes() instead. */
    savedBuf = (unsigned)CeConfigGetInt("SoundBufferBytes", (int)CE_AUDIO_RING_BYTES_DEFAULT);
    s_ringBytes = CE_AUDIO_RING_BYTES_DEFAULT;
    for (i = 0; i < CE_BUFFER_CHOICE_COUNT; i++)
        if (kBufferChoices[i] == savedBuf)
            s_ringBytes = savedBuf;
    s_ringMask = s_ringBytes - 1;

    RecomputeVolumeScale();

    CeLog("CeAudioInit: loaded volume=%d rate=%d bits=%d smooth=%d buffer=%u from config file",
          s_volumeLevel, s_rateHz, s_bitDepth, s_smooth, s_ringBytes);
}

static void CeAudioSaveConfig(void)
{
    CeConfigSetInt("SoundVolume", s_volumeLevel);
    CeConfigSetInt("SoundRate", s_rateHz);
    CeConfigSetInt("SoundBits", s_bitDepth);
    CeConfigSetInt("SoundSmooth", s_smooth);
    CeConfigSetInt("SoundBufferBytes", (int)s_ringBytes);
    CeConfigSave();

    CeLog("CeAudioSaveConfig: saved volume=%d rate=%d bits=%d smooth=%d buffer=%u",
          s_volumeLevel, s_rateHz, s_bitDepth, s_smooth, s_ringBytes);
}

/* ------------------------------------------------------------------ */
/* vid.h PCM hooks                                                     */
/* ------------------------------------------------------------------ */

/* Guards s_hWaveOut/s_bufs[]/s_bufIdx: OpenWaveOut() is called both
 * before the audio thread exists (pcm_init()) and while it's running and
 * concurrently touching the same state (pcm_pause(1) on a menu open,
 * Sound Config committing a Rate/Bits change) - both from the UI thread,
 * never the CPU/PPU thread, so contention here never affects emulation
 * speed either way. */
static CRITICAL_SECTION s_waveCs;

static void OpenWaveOut(int hz, int bits)
{
    WAVEFORMATEX wfx;
    unsigned i;

    EnterCriticalSection(&s_waveCs);

    if (s_hWaveOut)
    {
        waveOutReset(s_hWaveOut);
        for (i = 0; i < CE_AUDIO_NUM_BUFS; i++)
            waveOutUnprepareHeader(s_hWaveOut, &s_bufs[i].hdr, sizeof(WAVEHDR));
        waveOutClose(s_hWaveOut);
        s_hWaveOut = NULL;
    }

    memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = hz;
    wfx.wBitsPerSample  = bits;
    wfx.nBlockAlign     = bits / 8;
    wfx.nAvgBytesPerSec = hz * (bits / 8);

    if (waveOutOpen(&s_hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        s_hWaveOut = NULL;
        CeLog("OpenWaveOut: waveOutOpen failed for hz=%d bits=%d", hz, bits);
        LeaveCriticalSection(&s_waveCs);
        return;
    }

    for (i = 0; i < CE_AUDIO_NUM_BUFS; i++)
    {
        s_bufs[i].queued = 0;
        memset(&s_bufs[i].hdr, 0, sizeof s_bufs[i].hdr);
        s_bufs[i].hdr.lpData = (LPSTR)s_bufs[i].data;
        s_bufs[i].hdr.dwBufferLength = sizeof s_bufs[i].data;
        waveOutPrepareHeader(s_hWaveOut, &s_bufs[i].hdr, sizeof(WAVEHDR));
    }
    s_bufIdx = 0;

    LeaveCriticalSection(&s_waveCs);
}

/* Underrun click/pop countermeasure, ported from the sister PopGBA
 * port via PopSG - see the port's dev notes
 * (PopSG round 51). PopSG's own audio thread submits a fixed-size
 * waveOut buffer every iteration regardless of ring content, padding
 * the tail with a fade when the ring comes up short; this port's own
 * design instead only ever submits a buffer when the ring actually has
 * data (see this file's header comment), so there is no padded-tail
 * step to retrofit a fade into. The same abrupt-amplitude-step problem
 * still exists here, just at a different seam: when the ring runs dry,
 * CeAudioThreadProc previously just stopped submitting anything until
 * real audio resumed, so whatever sample level the last real buffer
 * ended on (not necessarily silence) held on the device's DAC until the
 * next real buffer started - an audible click both at the drop and the
 * resume. The fix: the moment the ring goes empty, synthesize and
 * submit one short chunk that fades from the last real sample down to
 * silence (closing the gap cleanly) instead of just stopping cold, and
 * fade the first real chunk back in once audio resumes - same 256/128-
 * sample fade lengths as the PopGBA/PopSG source (~6ms/~3ms at a
 * 44.1kHz-class rate; shorter at this port's lower Sound Config rates,
 * which only makes the fade quicker, not less effective).
 *
 * s_lastRawSample/s_hadUnderrun/s_underrunFlushed/s_underrunCount are
 * touched from both the audio thread (CeAudioDrainAndSubmit(),
 * CeAudioFlushUnderrun()) and the UI thread (pcm_pause(), which resets
 * them when the menu discards the ring - see that function), so unlike
 * the lock-free s_ringHead/s_ringTail pair, all reads/writes of these
 * four go through s_waveCs (the same lock already serializing
 * s_hWaveOut/s_bufs[] access between those two threads). */
static byte     s_lastRawSample   = 128; /* silence-centered (pcm.buf is unsigned 8-bit, 128 = 0) */
static int      s_hadUnderrun     = 0;   /* set once a fade-out has been emitted for the current empty spell; cleared once the next real chunk has been faded back in */
static int      s_underrunFlushed = 1;   /* true once the fade-out for the current empty spell has already been submitted (starts true: nothing real has played yet) */
static unsigned s_underrunCount   = 0;

/* Waits for a free waveOut header, applies Volume/Bits/Quality, and
 * submits raw[0..n) via the existing waveOut round-robin - the tail
 * half of what the old pcm_submit() did inline, shared by
 * CeAudioDrainAndSubmit() (real ring audio) and CeAudioFlushUnderrun()
 * (synthesized fade-out silence) below. Runs on CeAudioThreadProc's own
 * stack/thread, never on the CPU/PPU thread. */
static void CeAudioSubmitRaw(const byte *raw, unsigned n)
{
    CeAudioBuf *buf;
    unsigned waited = 0;
    unsigned i;
    int prev;

    EnterCriticalSection(&s_waveCs);

    if (!s_hWaveOut)
    {
        LeaveCriticalSection(&s_waveCs);
        return;
    }

    buf = &s_bufs[s_bufIdx];
    while (buf->queued && !(buf->hdr.dwFlags & WHDR_DONE))
    {
        if (waited >= 100) /* bounded wait, ms - this is the audio thread's own loop, so a stuck header only delays audio, never the emulation thread */
        {
            buf->queued = 0;
            CeLog("CeAudioSubmitRaw: idx=%u timed out waiting for WHDR_DONE after %ums - dropping this chunk",
                  s_bufIdx, waited);
#ifdef CE_PERF_DIAG
            InterlockedIncrement(&s_timeoutDropCount);
#endif
            LeaveCriticalSection(&s_waveCs);
            return;
        }
        Sleep(1);
        waited++;
    }

    /* Volume scaling, applied around the unsigned 8-bit PCM's 128
     * (silence) center point, plus Bits/Quality output formatting.
     * Smooth mode runs a light 2-tap moving average across the
     * (already volume-scaled) stream - a real, if simple, low-pass
     * filter rather than a purely cosmetic setting (see the design
     * discussion in the port's dev notes: gnuboy has no source/device rate
     * mismatch to resample, so "Quality" here just softens the
     * waveform's edges instead of picking a resampling algorithm). */
    prev = 128;
    if (s_bitDepth == 16)
    {
        int16_t *dst = buf->data;
        for (i = 0; i < n; i++)
        {
            int centered = (int)raw[i] - 128;
            int scaled = 128 + ((centered * s_volumeScale) >> 8);
            if (scaled < 0) scaled = 0;
            else if (scaled > 255) scaled = 255;
            if (s_smooth)
            {
                scaled = (scaled + prev) / 2;
                prev = scaled;
            }
            dst[i] = (int16_t)((scaled - 128) << 8);
        }
        buf->hdr.dwBufferLength = (DWORD)(n * sizeof(int16_t));
    }
    else
    {
        byte *dst = (byte *)buf->data;
        for (i = 0; i < n; i++)
        {
            int centered = (int)raw[i] - 128;
            int scaled = 128 + ((centered * s_volumeScale) >> 8);
            if (scaled < 0) scaled = 0;
            else if (scaled > 255) scaled = 255;
            if (s_smooth)
            {
                scaled = (scaled + prev) / 2;
                prev = scaled;
            }
            dst[i] = (byte)scaled;
        }
        buf->hdr.dwBufferLength = (DWORD)n;
    }

    buf->hdr.dwFlags &= ~WHDR_DONE;
    waveOutWrite(s_hWaveOut, &buf->hdr, sizeof(WAVEHDR));
    buf->queued = 1;

    s_bufIdx = (s_bufIdx + 1) % CE_AUDIO_NUM_BUFS;

    LeaveCriticalSection(&s_waveCs);
}

/* Drains up to CE_AUDIO_BUF_LEN bytes from the ring into a scratch
 * buffer (handling wraparound) and submits them via CeAudioSubmitRaw()
 * above - this is exactly what the old pcm_submit() did inline, just
 * moved onto the audio thread and fed from the ring instead of directly
 * from pcm.buf. Runs on CeAudioThreadProc's own stack/thread, never on
 * the CPU/PPU thread. */
static void CeAudioDrainAndSubmit(void)
{
    static byte s_scratch[CE_AUDIO_BUF_LEN];
    unsigned head, tail, avail, n, i;

    head = s_ringHead;
    tail = s_ringTail;
    avail = head - tail;
    if (avail == 0)
        return;

    n = (avail > CE_AUDIO_BUF_LEN) ? CE_AUDIO_BUF_LEN : avail;
    for (i = 0; i < n; i++)
        s_scratch[i] = s_ring[(tail + i) & s_ringMask];
    s_ringTail = tail + n;

    EnterCriticalSection(&s_waveCs);
    if (s_hadUnderrun)
    {
        /* First real chunk after an empty spell - fade it in from
         * silence over its first ~128 samples instead of jumping
         * straight to full amplitude, so the return isn't a click
         * either (see this section's header comment). */
        unsigned fin = (n < 128) ? n : 128;
        for (i = 0; i < fin; i++)
        {
            int32_t centered = (int32_t)s_scratch[i] - 128;
            s_scratch[i] = (byte)(128 + (centered * (int32_t)(i + 1)) / (int32_t)fin);
        }
        s_hadUnderrun = 0;
    }
    s_lastRawSample   = s_scratch[n - 1];
    s_underrunFlushed = 0; /* real audio is flowing again - the next empty spell should get its own fade-out */
    LeaveCriticalSection(&s_waveCs);

    CeAudioSubmitRaw(s_scratch, n);
}

/* Called from CeAudioThreadProc below whenever the ring is empty.
 * Synthesizes and submits one short fade-from-last-sample-to-silence
 * chunk the first time the ring goes dry (see this section's header
 * comment); a no-op on every subsequent empty-ring poll until real
 * audio flows again (CeAudioDrainAndSubmit() above clears
 * s_underrunFlushed as soon as that happens). */
static void CeAudioFlushUnderrun(void)
{
    byte fadeBuf[256];
    unsigned fade = sizeof fadeBuf, i;
    int32_t base;
    int alreadyFlushed;

    EnterCriticalSection(&s_waveCs);
    alreadyFlushed = s_underrunFlushed;
    base = (int32_t)s_lastRawSample - 128;
    LeaveCriticalSection(&s_waveCs);

    if (alreadyFlushed)
        return;

    for (i = 0; i < fade; i++)
        fadeBuf[i] = (byte)(128 + base - (base * (int32_t)(i + 1)) / (int32_t)fade);

    CeAudioSubmitRaw(fadeBuf, fade);

    EnterCriticalSection(&s_waveCs);
    s_hadUnderrun     = 1;
    s_lastRawSample   = 128;
    s_underrunFlushed = 1;
    LeaveCriticalSection(&s_waveCs);

    if ((++s_underrunCount % 500) == 1)
        CeLog("CeAudioFlushUnderrun: audio underrun (count=%u), fading to silence",
              s_underrunCount);
}

static DWORD WINAPI CeAudioThreadProc(LPVOID unused)
{
    (void)unused;
    while (s_audioThreadRun)
    {
        if (s_ringHead == s_ringTail)
        {
            /* Ring empty - nothing generated since the last drain (or
             * we're mid-menu with pcm_pause(1) having emptied it).
             * CeAudioFlushUnderrun() fades out once and then no-ops
             * until real audio resumes, so this is still a cheap poll,
             * not a growing backlog of silence buffers. Short sleep and
             * check again rather than spinning; this thread's own
             * timing has no bearing on emulation speed. */
            CeAudioFlushUnderrun();
            Sleep(5);
            continue;
        }
        CeAudioDrainAndSubmit();
    }
    return 0;
}

void pcm_init()
{
    pcm.hz = s_rateHz;
    pcm.stereo = 0;
    pcm.buf = s_pcmbuf;
    pcm.len = sizeof s_pcmbuf;
    pcm.pos = 0;

    InitializeCriticalSection(&s_waveCs);
    OpenWaveOut(s_rateHz, s_bitDepth);

    s_ringHead = 0;
    s_ringTail = 0;
    s_audioThreadRun = 1;
    s_audioThread = CreateThread(NULL, 0, CeAudioThreadProc, NULL, 0, NULL);
    if (s_audioThread)
        SetThreadPriority(s_audioThread, THREAD_PRIORITY_ABOVE_NORMAL);
    else
        CeLog("pcm_init: CreateThread failed for audio drain thread");
}

void pcm_close()
{
    unsigned i;

    if (s_audioThread)
    {
        s_audioThreadRun = 0;
        /* Bounded wait, not INFINITE - see the port's dev notes' account of the
         * earlier prototype hanging process exit on an INFINITE
         * WaitForSingleObject for a worker thread; CeShutdown() below is
         * on its way to ExitProcess() regardless, so this is just being
         * tidy, not load-bearing. */
        WaitForSingleObject(s_audioThread, 500);
        CloseHandle(s_audioThread);
        s_audioThread = NULL;
    }

    if (s_hWaveOut)
    {
        waveOutReset(s_hWaveOut);
        for (i = 0; i < CE_AUDIO_NUM_BUFS; i++)
            waveOutUnprepareHeader(s_hWaveOut, &s_bufs[i].hdr, sizeof(WAVEHDR));
        waveOutClose(s_hWaveOut);
        s_hWaveOut = NULL;
    }

    DeleteCriticalSection(&s_waveCs);
}

/* Called from sound.c's sound_mix() - see this file's header comment for
 * why this now just copies into the ring and returns immediately instead
 * of touching waveOut directly: this runs on the same thread as the
 * CPU/PPU interpreter, so anything it blocks on stalls emulation itself. */
#ifdef CE_PERF_DIAG
/* All CPU/PPU-thread-only (pcm_submit() is never called from the audio
 * thread), so plain statics, no Interlocked* needed here - unlike the
 * timeout-drop counter below, which the audio thread also touches. */
static unsigned s_submitAccumMs = 0;
static unsigned s_overflowDropBytes = 0;
static unsigned s_overflowDropCount = 0;

unsigned CeAudioGetAndResetSubmitMs(void)
{
    unsigned ms = s_submitAccumMs;
    s_submitAccumMs = 0;
    return ms;
}

unsigned CeAudioGetAndResetOverflowDropBytes(void)
{
    unsigned n = s_overflowDropBytes;
    s_overflowDropBytes = 0;
    return n;
}

unsigned CeAudioGetAndResetOverflowDropCount(void)
{
    unsigned n = s_overflowDropCount;
    s_overflowDropCount = 0;
    return n;
}

unsigned CeAudioGetAndResetTimeoutDropCount(void)
{
    return (unsigned)InterlockedExchange(&s_timeoutDropCount, 0);
}
#endif

int pcm_submit()
{
    unsigned n, head, tail, free, i;
#ifdef CE_PERF_DIAG
    DWORD perfStart = GetTickCount();
#endif

    if (pcm.pos == 0)
        return 0;

    n = (unsigned)pcm.pos;
    head = s_ringHead;
    tail = s_ringTail;
    free = s_ringBytes - (head - tail);
    if (n > free)
    {
        /* ring backed up (audio thread starved or device not open) - drop
         * the overflow rather than block the emulation thread */
#ifdef CE_PERF_DIAG
        s_overflowDropBytes += (n - free);
        s_overflowDropCount++;
#endif
        n = free;
    }

    for (i = 0; i < n; i++)
        s_ring[(head + i) & s_ringMask] = s_pcmbuf[i];
    s_ringHead = head + n;

    pcm.pos = 0;
#ifdef CE_PERF_DIAG
    s_submitAccumMs += (unsigned)(GetTickCount() - perfStart);
#endif
    return 1;
}

void pcm_pause(int dopause)
{
    if (!s_hWaveOut)
        return;
    if (dopause)
    {
        /* Drop whatever's backed up in the ring so the audio thread
         * doesn't keep draining stale pre-pause audio into a freshly
         * reopened device, then reopen waveOut - full unprepare/close/
         * reopen/re-prepare, not just waveOutReset(), because a bare
         * reset was hardware-confirmed insufficient on this device's
         * driver (see the port's dev notes) even before this ring-buffer rewrite. */
        s_ringTail = s_ringHead;
        OpenWaveOut(s_rateHz, s_bitDepth);

        /* Treat the freshly-reopened, empty ring as already "flushed to
         * silence" rather than a real underrun - otherwise the audio
         * thread's very next empty-ring poll would synthesize and emit
         * a spurious fade-out chunk (and count/log an underrun) purely
         * because the menu just discarded whatever was queued, not
         * because playback actually ran dry. See this file's underrun
         * countermeasure comment above CeAudioSubmitRaw(). Guarded by
         * s_waveCs like every other cross-thread access of these three -
         * this runs on the UI thread, CeAudioDrainAndSubmit()/
         * CeAudioFlushUnderrun() on the dedicated audio thread, which
         * keeps running independently of pcm_pause()/emu_run(). */
        EnterCriticalSection(&s_waveCs);
        s_lastRawSample   = 128;
        s_hadUnderrun     = 0;
        s_underrunFlushed = 1;
        LeaveCriticalSection(&s_waveCs);

        CeLog("pcm_pause: reopened waveOut device (hz=%d bits=%d)", s_rateHz, s_bitDepth);
    }
}

/* Real-hardware follow-up to the pacing/underrun-fade work above
 * (dev notes): a hardware log showed exactly one genuine
 * CeAudioFlushUnderrun() firing per session, always at the very first
 * frame(s) right after a ROM loads / a dialog closes and emu_run()
 * (re)starts - not during steady gameplay (that same log's "perf:
 * retro_run avg" stayed at 6-8ms, well under the 16.7ms frame budget,
 * ruling out sustained CPU overload). Root cause: pcm_pause(1) just
 * reset the ring to fully empty, so it has zero cushion the instant
 * emu_run() resumes; a one-off slow first frame (cold GDI/DIB/cache
 * effects right after a dialog closes - the "doevents: slow frame"
 * log's own long-documented, previously-harmless one-off cost) is
 * enough to starve it before real audio has built up any backlog of
 * its own. Fix: give the ring a small head start of true silence
 * before calling emu_run() (see WinMain's own call site) so the audio
 * thread has something to drain while that first frame runs, whatever
 * it costs - by the time the primed silence itself runs low, real
 * audio should already be flowing. Ordinary silence, not a fade
 * target: the ring treats it exactly like any other queued audio
 * (CeAudioThreadProc doesn't need to know or care it was synthesized),
 * so no interaction with s_hadUnderrun/s_lastRawSample/
 * s_underrunFlushed is needed here. */
void CeAudioPrimeSilence(unsigned ms)
{
    unsigned n, i, head;

    if (!s_hWaveOut)
        return;

    n = (ms * (unsigned)s_rateHz) / 1000u;
    if (n > s_ringBytes)
        n = s_ringBytes;

    /* Called from the main thread only, between ShowMainMenu() returning
     * and the next emu_run() call - the same thread pcm_submit() runs on
     * (gnuboy has no separate CPU/PPU thread; emu_run() executes
     * synchronously on WinMain's own thread) and never concurrently with
     * pcm_submit() itself, so writing s_ringHead directly here preserves
     * the single-writer SPSC invariant the same way pcm_submit() does -
     * no lock needed. */
    head = s_ringHead;
    for (i = 0; i < n; i++)
        s_ring[(head + i) & s_ringMask] = 128; /* silence-centered */
    s_ringHead = head + n;
}

unsigned CeAudioGetBufferOccupancyPercent(void)
{
    unsigned backlog;

    if (!s_hWaveOut)
        return 0;

    backlog = s_ringHead - s_ringTail;
    return (backlog * 100) / s_ringBytes;
}

int CeAudioIsActive(void)
{
    return s_hWaveOut != NULL;
}

unsigned CeAudioGetBufferedMs(void)
{
    unsigned backlog;

    if (!s_hWaveOut)
        return 0;

    backlog = s_ringHead - s_ringTail;
    /* backlog <= s_ringBytes (<= CE_AUDIO_RING_BYTES_MAX = 262144), so
     * backlog*1000 stays well inside 32-bit - no 64-bit math needed
     * (same bound as sister PopSG's own CeAudioGetBufferedMs()). */
    return (backlog * 1000u) / (unsigned)s_rateHz;
}

/* Swap the ring's active size. Called only from Sound Config's commit
 * path, which runs on the UI thread while ShowMainMenu()'s pcm_pause(1)
 * has already emptied the ring (s_ringTail = s_ringHead) and the
 * CPU/PPU producer thread is not running (emu_run() has returned) - so
 * the only concurrent reader is the drain thread, which sees head==tail
 * and does nothing while this runs. Re-asserting the empty state and
 * holding s_waveCs keeps it robust regardless. */
static void ApplyRingBytes(unsigned n)
{
    EnterCriticalSection(&s_waveCs);
    s_ringTail  = s_ringHead;
    s_ringBytes = n;
    s_ringMask  = n - 1;
    LeaveCriticalSection(&s_waveCs);
    CeLog("ApplyRingBytes: ring now %u bytes", n);
}

/* ------------------------------------------------------------------ */
/* Sound Config dialog (Volume/Rate/Bits/Quality "-/value/+" spinners) */
/* ------------------------------------------------------------------ */

static void UpdateVolumeLabel(HWND hDlg)
{
    wchar_t text[8];
    _snwprintf(text, 8, L"%d", s_volumeLevel);
    SetWindowTextW(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE), text);
}

static int CurrentRateStepIndex(void)
{
    unsigned i;
    for (i = 0; i < CE_RATE_CHOICE_COUNT; i++)
        if ((int)kRateChoices[i] == s_pendingRateHz)
            return (int)i;
    return (int)CE_RATE_CHOICE_COUNT - 1;
}

static void UpdateRateLabel(HWND hDlg)
{
    wchar_t text[16];
    _snwprintf(text, 16, L"%d", s_pendingRateHz);
    SetWindowTextW(GetDlgItem(hDlg, IDC_SC_RATE_VALUE), text);
}

static int CurrentBufferStepIndex(void)
{
    unsigned i;
    for (i = 0; i < CE_BUFFER_CHOICE_COUNT; i++)
        if ((int)kBufferChoices[i] == s_pendingBufferBytes)
            return (int)i;
    return (int)CE_BUFFER_CHOICE_COUNT - 1;
}

static void UpdateBufferLabel(HWND hDlg)
{
    wchar_t text[16];
    _snwprintf(text, 16, L"%u KB", (unsigned)s_pendingBufferBytes / 1024);
    SetWindowTextW(GetDlgItem(hDlg, IDC_SC_BUFFER_VALUE), text);
}

static void UpdateBitsLabel(HWND hDlg)
{
    if (CeLangIsJapanese())
        SetWindowTextW(GetDlgItem(hDlg, IDC_SC_BITS_VALUE),
                        s_pendingBitDepth == 8 ? L"8\x30d3\x30c3\x30c8" /* 8ビット */
                                               : L"16\x30d3\x30c3\x30c8"); /* 16ビット */
    else
        SetWindowTextW(GetDlgItem(hDlg, IDC_SC_BITS_VALUE),
                        s_pendingBitDepth == 8 ? L"8-bit" : L"16-bit");
}

static void UpdateQualityLabel(HWND hDlg)
{
    if (CeLangIsJapanese())
        SetWindowTextW(GetDlgItem(hDlg, IDC_SC_QUALITY_VALUE),
                        s_smooth ? L"\x9ad8\x97f3\x8cea" /* 高音質 */
                                 : L"\x4f4e\x97f3\x8cea"); /* 低音質 */
    else
        SetWindowTextW(GetDlgItem(hDlg, IDC_SC_QUALITY_VALUE),
                        s_smooth ? L"Smooth" : L"Fast");
}

#define WM_SETSOUNDFOCUS (WM_APP + 203)

static int SoundNeighborDown(int id)
{
    switch (id)
    {
    case IDC_SC_VOLUME_VALUE:  return IDC_SC_RATE_VALUE;
    case IDC_SC_RATE_VALUE:    return IDC_SC_BITS_VALUE;
    case IDC_SC_BITS_VALUE:    return IDC_SC_QUALITY_VALUE;
    case IDC_SC_QUALITY_VALUE: return IDC_SC_BUFFER_VALUE;
    case IDC_SC_BUFFER_VALUE:  return IDOK;
    case IDOK:                 return IDC_SC_VOLUME_VALUE;
    }
    return id;
}

static int SoundNeighborUp(int id)
{
    switch (id)
    {
    case IDC_SC_VOLUME_VALUE:  return IDOK;
    case IDC_SC_RATE_VALUE:    return IDC_SC_VOLUME_VALUE;
    case IDC_SC_BITS_VALUE:    return IDC_SC_RATE_VALUE;
    case IDC_SC_QUALITY_VALUE: return IDC_SC_BITS_VALUE;
    case IDC_SC_BUFFER_VALUE:  return IDC_SC_QUALITY_VALUE;
    case IDOK:                 return IDC_SC_BUFFER_VALUE;
    }
    return id;
}

static WNDPROC s_pSoundOrigProc = NULL;

static LRESULT CALLBACK SoundCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
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
            SetFocus(GetDlgItem(GetParent(hWnd), SoundNeighborUp(id)));
            return 0;

        case VK_DOWN:
            SetFocus(GetDlgItem(GetParent(hWnd), SoundNeighborDown(id)));
            return 0;

        case VK_RETURN:
            if (id == IDC_SC_VOLUME_VALUE || id == IDC_SC_RATE_VALUE ||
                id == IDC_SC_BITS_VALUE || id == IDC_SC_QUALITY_VALUE ||
                id == IDC_SC_BUFFER_VALUE)
                SetFocus(GetDlgItem(GetParent(hWnd), SoundNeighborDown(id)));
            else if (id == IDOK)
                SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_LEFT:
            if (id == IDC_SC_VOLUME_VALUE)
            {
                if (s_volumeLevel > 0) s_volumeLevel--;
                RecomputeVolumeScale();
                UpdateVolumeLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_RATE_VALUE)
            {
                int idx = CurrentRateStepIndex();
                if (idx > 0) s_pendingRateHz = (int)kRateChoices[idx - 1];
                UpdateRateLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_BITS_VALUE)
            {
                s_pendingBitDepth = (s_pendingBitDepth == 8) ? 16 : 8;
                UpdateBitsLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_QUALITY_VALUE)
            {
                s_smooth = !s_smooth;
                UpdateQualityLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_BUFFER_VALUE)
            {
                int idx = CurrentBufferStepIndex();
                if (idx > 0) s_pendingBufferBytes = (int)kBufferChoices[idx - 1];
                UpdateBufferLabel(GetParent(hWnd));
            }
            return 0;

        case VK_RIGHT:
            if (id == IDC_SC_VOLUME_VALUE)
            {
                if (s_volumeLevel < 10) s_volumeLevel++;
                RecomputeVolumeScale();
                UpdateVolumeLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_RATE_VALUE)
            {
                int idx = CurrentRateStepIndex();
                if (idx < (int)CE_RATE_CHOICE_COUNT - 1) s_pendingRateHz = (int)kRateChoices[idx + 1];
                UpdateRateLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_BITS_VALUE)
            {
                s_pendingBitDepth = (s_pendingBitDepth == 8) ? 16 : 8;
                UpdateBitsLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_QUALITY_VALUE)
            {
                s_smooth = !s_smooth;
                UpdateQualityLabel(GetParent(hWnd));
            }
            else if (id == IDC_SC_BUFFER_VALUE)
            {
                int idx = CurrentBufferStepIndex();
                if (idx < (int)CE_BUFFER_CHOICE_COUNT - 1) s_pendingBufferBytes = (int)kBufferChoices[idx + 1];
                UpdateBufferLabel(GetParent(hWnd));
            }
            return 0;

        case VK_ESCAPE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pSoundOrigProc, hWnd, message, wParam, lParam);
}

#define VOLUME_REPEAT_TIMER_ID     1
#define VOLUME_REPEAT_INITIAL_MS   500
#define VOLUME_REPEAT_INTERVAL_MS  120

static WNDPROC s_origVolumeBtnProc = NULL;
static int     s_volumeRepeatDir   = 0;
static int     s_volumeRepeatFast  = 0;

static LRESULT CALLBACK VolumeButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        s_volumeRepeatDir  = (GetDlgCtrlID(hwnd) == IDC_SC_VOLUME_PLUS) ? 1 : -1;
        s_volumeRepeatFast = 0;
        SetTimer(hwnd, VOLUME_REPEAT_TIMER_ID, VOLUME_REPEAT_INITIAL_MS, NULL);
        break;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        KillTimer(hwnd, VOLUME_REPEAT_TIMER_ID);
        s_volumeRepeatDir = 0;
        break;

    case WM_TIMER:
        if (wParam == VOLUME_REPEAT_TIMER_ID && s_volumeRepeatDir != 0)
        {
            if (!s_volumeRepeatFast)
            {
                s_volumeRepeatFast = 1;
                SetTimer(hwnd, VOLUME_REPEAT_TIMER_ID, VOLUME_REPEAT_INTERVAL_MS, NULL);
            }

            if (s_volumeRepeatDir > 0)
            {
                if (s_volumeLevel < 10) s_volumeLevel++;
            }
            else
            {
                if (s_volumeLevel > 0) s_volumeLevel--;
            }
            RecomputeVolumeScale();
            UpdateVolumeLabel(GetParent(hwnd));
        }
        break;
    }

    return CallWindowProc(s_origVolumeBtnProc, hwnd, msg, wParam, lParam);
}

/* Labels repainted by WM_PAINT via CeBmpFontPaintLabel() - the four
 * "-/value/+" spinner VALUE buttons (IDC_SC_*_VALUE) are BS_OWNERDRAW
 * (ce_res.rc) instead and redraw themselves via WM_DRAWITEM. */
static const int kSoundLabelIds[] = {
    IDC_SC_LBL_VOLUME, IDC_SC_LBL_RATE, IDC_SC_LBL_BITS, IDC_SC_LBL_QUALITY,
    IDC_SC_LBL_BUFFER,
};
#define CE_SOUND_LABEL_COUNT (sizeof(kSoundLabelIds) / sizeof(kSoundLabelIds[0]))

static void ApplySoundConfigLanguage(HWND hDlg)
{
    unsigned i;

    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_SC_LBL_VOLUME,    L"\x97f3\x91cf\x3a");       /* 音量: */
        SetDlgItemTextW(hDlg, IDC_SC_LBL_RATE,      L"\x30ec\x30fc\x30c8\x3a"); /* レート: */
        SetDlgItemTextW(hDlg, IDC_SC_LBL_BITS,      L"\x30d3\x30c3\x30c8\x3a"); /* ビット: */
        SetDlgItemTextW(hDlg, IDC_SC_LBL_QUALITY,   L"\x97f3\x8cea\x3a");       /* 音質: */
        SetDlgItemTextW(hDlg, IDC_SC_LBL_BUFFER,    L"\x30d0\x30c3\x30d5\x30a1"); /* バッファ */
        SetDlgItemTextW(hDlg, IDOK,     L"\x6c7a\x5b9a");                       /* 決定 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_SC_LBL_VOLUME,    L"Volume:");
        SetDlgItemTextW(hDlg, IDC_SC_LBL_RATE,      L"Rate:");
        SetDlgItemTextW(hDlg, IDC_SC_LBL_BITS,      L"Bits:");
        SetDlgItemTextW(hDlg, IDC_SC_LBL_QUALITY,   L"Quality:");
        SetDlgItemTextW(hDlg, IDC_SC_LBL_BUFFER,    L"Buffer:");
        SetDlgItemTextW(hDlg, IDOK,     L"OK");
    }

    for (i = 0; i < CE_SOUND_LABEL_COUNT; i++)
        ShowWindow(GetDlgItem(hDlg, kSoundLabelIds[i]), SW_HIDE);
}

static INT_PTR CALLBACK SoundConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        s_pendingBitDepth = s_bitDepth;
        s_pendingRateHz = s_rateHz;
        s_pendingBufferBytes = (int)s_ringBytes;

        UpdateVolumeLabel(hDlg);
        UpdateRateLabel(hDlg);
        UpdateBitsLabel(hDlg);
        UpdateQualityLabel(hDlg);
        UpdateBufferLabel(hDlg);
        ApplySoundConfigLanguage(hDlg);

        s_pSoundOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE),  GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_RATE_VALUE),    GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_BITS_VALUE),    GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_QUALITY_VALUE), GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_BUFFER_VALUE),  GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK),                 GWLP_WNDPROC, (LONG_PTR)SoundCtrlProc);

        s_volumeRepeatDir = 0;
        s_origVolumeBtnProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_VOLUME_MINUS), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_VOLUME_MINUS), GWLP_WNDPROC, (LONG_PTR)VolumeButtonSubclassProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SC_VOLUME_PLUS),  GWLP_WNDPROC, (LONG_PTR)VolumeButtonSubclassProc);

        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE));
        PostMessage(hDlg, WM_SETSOUNDFOCUS, 0, 0);
        return FALSE;
    }

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        unsigned i;
        for (i = 0; i < CE_SOUND_LABEL_COUNT; i++)
            CeBmpFontPaintLabel(hdc, hDlg, kSoundLabelIds[i]);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            SetFocus(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE));
            PostMessage(hDlg, WM_SETSOUNDFOCUS, 0, 0);
        }
        break;

    case WM_SETSOUNDFOCUS:
        SetFocus(GetDlgItem(hDlg, IDC_SC_VOLUME_VALUE));
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SC_VOLUME_MINUS:
            if (s_volumeLevel > 0) s_volumeLevel--;
            RecomputeVolumeScale();
            UpdateVolumeLabel(hDlg);
            return TRUE;

        case IDC_SC_VOLUME_PLUS:
            if (s_volumeLevel < 10) s_volumeLevel++;
            RecomputeVolumeScale();
            UpdateVolumeLabel(hDlg);
            return TRUE;

        case IDC_SC_RATE_MINUS:
        {
            int idx = CurrentRateStepIndex();
            if (idx > 0) s_pendingRateHz = (int)kRateChoices[idx - 1];
            UpdateRateLabel(hDlg);
            return TRUE;
        }

        case IDC_SC_RATE_PLUS:
        {
            int idx = CurrentRateStepIndex();
            if (idx < (int)CE_RATE_CHOICE_COUNT - 1) s_pendingRateHz = (int)kRateChoices[idx + 1];
            UpdateRateLabel(hDlg);
            return TRUE;
        }

        case IDC_SC_BITS_MINUS:
        case IDC_SC_BITS_PLUS:
            s_pendingBitDepth = (s_pendingBitDepth == 8) ? 16 : 8;
            UpdateBitsLabel(hDlg);
            return TRUE;

        case IDC_SC_QUALITY_MINUS:
        case IDC_SC_QUALITY_PLUS:
            s_smooth = !s_smooth;
            UpdateQualityLabel(hDlg);
            return TRUE;

        case IDC_SC_BUFFER_MINUS:
        {
            int idx = CurrentBufferStepIndex();
            if (idx > 0) s_pendingBufferBytes = (int)kBufferChoices[idx - 1];
            UpdateBufferLabel(hDlg);
            return TRUE;
        }

        case IDC_SC_BUFFER_PLUS:
        {
            int idx = CurrentBufferStepIndex();
            if (idx < (int)CE_BUFFER_CHOICE_COUNT - 1) s_pendingBufferBytes = (int)kBufferChoices[idx + 1];
            UpdateBufferLabel(hDlg);
            return TRUE;
        }

        case IDOK:
        case IDCANCEL:
        {
            /* Physical Back (IDCANCEL) commits and closes, same as OK -
             * this device has no meaningful "discard changes" gesture,
             * only "go back". Only a Rate/Bits change needs gnuboy's
             * generation rate re-derived and the waveOut device
             * reopened; Volume/Quality apply live in pcm_submit() and
             * never touch either. */
            int reopenNeeded = (s_pendingRateHz != s_rateHz || s_pendingBitDepth != s_bitDepth);

            s_rateHz = s_pendingRateHz;
            s_bitDepth = s_pendingBitDepth;

            /* Buffer size: swap the ring in before saving so
             * CeAudioSaveConfig() persists the new s_ringBytes. Safe
             * here - see ApplyRingBytes()'s comment (ring already
             * emptied by ShowMainMenu()'s pcm_pause(1), producer idle). */
            if (s_pendingBufferBytes != (int)s_ringBytes)
                ApplyRingBytes((unsigned)s_pendingBufferBytes);

            CeAudioSaveConfig();

            if (reopenNeeded)
            {
                pcm.hz = s_rateHz;
                sound_reset();
                OpenWaveOut(s_rateHz, s_bitDepth);
            }

            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        }
        return FALSE;

    default:
        return FALSE;
    }
    return FALSE;
}

void CeShowSoundConfigDialog(HWND owner)
{
    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), MAKEINTRESOURCEW(IDD_SOUNDCONFIG),
               owner, SoundConfigDlgProc);
}
