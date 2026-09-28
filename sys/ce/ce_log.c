/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "ce_log.h"
#include "ce_app_config.h"

/* Session 19 (the port's dev notes): a real-hardware CE_PERF_DIAG capture caught
 * one CeLog() call costing 651ms - this device's flash storage
 * apparently has occasional multi-hundred-ms latency spikes on file
 * open (likely directory/FAT-chain lookups that grow with distance
 * from the start of the file, since this is an append). The original
 * design reopened the file (open/append/close) on every single call for
 * crash safety - each line durable even if the very next line crashes
 * the process. That property is kept here without paying the open/close
 * cost every time: the file is opened once and kept open, and every
 * call still ends with fflush() (pushes the CRT's buffer into the OS's
 * own file buffer via WriteFile - the same durability fclose() gave
 * per-call before, just without repeating the open/seek-to-end/close
 * around it). CeLogClose() is called from the explicit shutdown paths
 * (ce_main.c's CeShutdown()/die() - this device's ExitProcess()-only
 * discipline means atexit() never runs, so nothing closes this
 * automatically) purely for tidiness; even without it, ExitProcess()
 * closing the still-open handle flushes the same OS-level buffer. */
static FILE *s_logFile = NULL;
static int s_pathResolved = 0;
static int s_enabled = 0; /* Video Config's "Enable Debug Log" - off by default (user request), see ce_log.h */
static wchar_t s_logPath[MAX_PATH];

void CeLogSetEnabled(int enabled)
{
	s_enabled = enabled ? 1 : 0;
}

int CeLogIsEnabled(void)
{
	return s_enabled;
}

static FILE *EnsureLogFile(void)
{
	if (s_logFile)
		return s_logFile;

	if (!s_pathResolved)
	{
		wchar_t exePath[MAX_PATH];
		wchar_t *slash;

		GetModuleFileNameW(NULL, exePath, MAX_PATH);
		slash = wcsrchr(exePath, L'\\');
		if (slash)
			*slash = L'\0';
		_snwprintf(s_logPath, MAX_PATH, L"%s\\" CE_APP_LOG_FILENAME, exePath);
		s_pathResolved = 1;
	}

	/* Retried on every call until it succeeds (e.g. the storage card
	 * isn't mounted yet at the very first call) - same as the original
	 * per-call fopen()'s implicit retry behavior. */
	s_logFile = _wfopen(s_logPath, L"a");
	return s_logFile;
}

void CeLog(const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	FILE *f;

	if (!s_enabled)
		return;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
	va_end(ap);

	f = EnsureLogFile();
	if (!f)
		return;

	/* GetTickCount() (ms since boot, wraps at ~49.7 days - irrelevant for
	 * a single debug session) rather than wall-clock time: lets a symptom
	 * the user reports right after it happens ("just now, X ms since
	 * boot per the device clock") be matched against a specific line
	 * without needing RTC/timezone handling. */
	fprintf(f, "[%10lu] ", (unsigned long)GetTickCount());
	fputs(msg, f);
	fputc('\n', f);
	fflush(f);
}

void CeLogClose(void)
{
	if (s_logFile)
	{
		fclose(s_logFile);
		s_logFile = NULL;
	}
}
