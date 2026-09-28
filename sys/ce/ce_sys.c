/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * ce_sys.c - Windows CE misc glue: timing, path setup, joystick no-ops.
 *
 * sys_initpath() ignores the argv[0] passed in (WinMain has no real argv,
 * see ce_main.c) and instead asks the OS for the exe's own absolute path
 * via GetModuleFileNameW(). rcpath/savedir are pointed at that directory
 * so save files are found next to the .exe regardless of whatever the
 * process's current directory happens to be.
 *
 * Narrow-converted with CP_UTF8, not CP_ACP: a real-hardware log caught
 * this producing "\Storage Card\???\PopGB" for an app installed under
 * a Japanese-named folder - CP_ACP couldn't represent those characters
 * and silently replaced them with '?', which is not reversible, so
 * anything built from this string (loader.c's saveprefix) pointed at a
 * folder that doesn't exist. sys/ce/compat/stdio.h redirects loader.c's
 * fopen() calls to ce_fopen_utf8() (below), which decodes this UTF-8
 * narrow path back to wide and calls the real _wfopen() - see that
 * header for the full writeup. This must stay CP_UTF8 to match.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "rc.h"
#include "ce_log.h"

/* cpu.c (kept unmodified - see the port's dev notes) unconditionally references
 * debug_trace/debug_disassemble() (`if (debug_trace) debug_disassemble(...)`
 * in cpu_emulate()) - normally supplied by debug.c, the CLI disassembler
 * this port drops along with main.c's command line (see the port's dev notes'
 * "完全に廃止" file list). debug_trace stays 0 forever (nothing in this
 * port ever sets it), so debug_disassemble() itself is never actually
 * called - these two symbols only need to exist to satisfy the linker. */
int debug_trace = 0;
void debug_disassemble(addr a, int c)
{
	(void)a;
	(void)c;
}

/* This device has no real POSIX signal delivery - main.c's
 * catch_signals()/fatalsignal() best-effort handlers (see
 * compat/signal.h) are simply never invoked. */
void *signal(int sig, void *handler)
{
	(void)sig;
	(void)handler;
	return (void *)0;
}

int raise(int sig)
{
	(void)sig;
	return 0;
}

rcvar_t joy_exports[] =
{
	RCV_END
};

void joy_init()
{
}

void joy_close()
{
}

void joy_poll()
{
}

void sys_sleep(int us)
{
	/* Defensive guard, matching sys/nix/nix.c's own sys_sleep(): casting a
	 * negative `us` to DWORD would otherwise wrap to a huge unsigned
	 * Sleep() argument. Kept even though real-hardware logging (see
	 * the port's dev notes) later showed this guard alone doesn't explain the
	 * ~170ms/frame slowdown under investigation - it's still correct on
	 * its own terms. */
	/* Logged only outside the normal range (0..~20000, comfortably
	 * covering framelen's ~16743) - settles exactly what emu_run()'s
	 * frame-pacing math is asking for on whichever frames the doevents()
	 * gap log in ce_main.c flags as slow: a large positive `us` here would
	 * mean emu_run() itself thinks it's already ~170ms behind before it
	 * even reaches this call (so sys_sleep() is just obeying a bad
	 * request); a negative one means sys_sleep() is NOT where the 170ms
	 * goes at all, and cpu_emulate()/vid_end()/sound_mix() upstream of it
	 * are the real cost - a question four different fixes elsewhere
	 * haven't answered yet. */
	if (us < 0 || us > 20000)
		CeLog("sys_sleep: us=%d", us);
	if (us <= 0)
		return;
	Sleep((DWORD)(us / 1000));
}

/* CE paths use backslashes throughout (and gnuboy's own path_search()/
 * fopen() calls handle backslash paths fine) - nothing to sanitize. */
void sys_sanitize(char *s)
{
	(void)s;
}

/* savedir defaults to "." (see main.c's defaultconfig[]); the exe's own
 * directory is not guaranteed to be the process's current directory on
 * this device, so ce_main.c's WinMain calls SetCurrentDirectoryW() at
 * startup to make "." resolve there. Nothing else to check here - the
 * directory already exists (it's where the .exe itself lives). */
void sys_checkdir(char *path, int wr)
{
	(void)path;
	(void)wr;
}

void *sys_timer()
{
	DWORD *tv = malloc(sizeof *tv);
	*tv = GetTickCount() * 1000; /* microseconds */
	return tv;
}

int sys_elapsed(DWORD *cl)
{
	DWORD now = GetTickCount() * 1000;
	DWORD usecs = now - *cl;
	*cl = now;
	/* Unconditional (unlike sys_sleep()'s threshold-gated log): sys_sleep()
	 * never once logged an out-of-range `us` despite the doevents() gap
	 * log in ce_main.c showing ~170ms every frame, which only makes sense
	 * if either (a) emu_run()'s frame-pacing branch (and therefore
	 * sys_sleep()) isn't actually being reached most frames - e.g.
	 * pcm_submit() returning 1 far more often than its buffer-fill math
	 * suggests it should - or (b) this function itself is under-reporting
	 * elapsed time relative to the independent GetTickCount() reads in
	 * ce_main.c's doevents(). Logging every call (not just anomalies)
	 * settles both at once: compare its timestamped values directly
	 * against the doevents() gap log for the same stretch. */
	CeLog("sys_elapsed: usecs=%d", (int)usecs);
	return (int)usecs;
}

void sys_initpath(char *exe)
{
	wchar_t wpath[MAX_PATH];
	char *narrow, *slash, *buf;

	(void)exe; /* WinMain's fake argv[0] is not a real path - see ce_main.c */

	GetModuleFileNameW(NULL, wpath, MAX_PATH);

	narrow = malloc(MAX_PATH * 2);
	WideCharToMultiByte(CP_UTF8, 0, wpath, -1, narrow, MAX_PATH * 2, NULL, NULL);

	slash = strrchr(narrow, '\\');
	if (slash) *slash = 0;

	buf = malloc(strlen(narrow) + 2);
	sprintf(buf, "%s;", narrow);
	rc_setvar("rcpath", 1, &buf);

	rc_setvar("savedir", 1, &narrow);

	free(narrow);
	free(buf);
}

/* See sys/ce/compat/stdio.h - loader.c's fopen() calls are redirected
 * here so its narrow (UTF-8) paths get decoded to wide and opened via
 * the real _wfopen() instead of the CRT's own CP_ACP-based narrow
 * fopen(), which mangles any path component the device's ANSI code page
 * can't represent (see that header's comment for the real-hardware
 * symptom this fixes). Mode strings passed in here are always one of
 * gnuboy's own literal C strings ("r"/"rb"/"w"/"wb") - plain ASCII,
 * valid as-is in any code page - so CP_ACP is fine for that half. */
FILE *ce_fopen_utf8(const char *utf8path, const char *mode)
{
	wchar_t wpath[MAX_PATH];
	wchar_t wmode[8];

	MultiByteToWideChar(CP_UTF8, 0, utf8path, -1, wpath, MAX_PATH);
	MultiByteToWideChar(CP_ACP, 0, mode, -1, wmode, 8);

	return _wfopen(wpath, wmode);
}
