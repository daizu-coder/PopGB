/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#ifndef CE_LOG_H
#define CE_LOG_H

/* Crash-safe diagnostic logger: appends a line to
 * "<exe-dir>\" CE_APP_LOG_FILENAME (see ce_app_config.h), fflush()ing
 * after every call so each line is durable even if the process dies on
 * the very next line (see ce_log.c for why this no longer means
 * reopening the file every call). There is no debugger on this device,
 * so this (plus Windows CE's own BER crash log) is the only way to see
 * what happened after a real-hardware run. */
void CeLog(const char *fmt, ...);

/* Video Config's "Enable Debug Log" checkbox (user request) - lets this
 * device's occasional flash-I/O cost (see ce_log.c) be turned off
 * entirely when a log isn't being collected. Disabled by default (user
 * request); the persisted flag is applied once at startup (ce_video.c's
 * CeVideoInit()). s_enabled starts at 0, so the handful of lines that
 * would otherwise be written before that call - CeConfigLoad()'s own
 * summary, sys_initpath() - are suppressed too on a fresh config; enable
 * the checkbox in Video Config to capture startup diagnostics. */
void CeLogSetEnabled(int enabled);
int  CeLogIsEnabled(void);

/* Closes the log file handle CeLog() keeps open across calls. Call once
 * from an explicit shutdown path (this device's ExitProcess()-only
 * discipline means atexit() never runs - see ce_main.c's CeShutdown()/
 * die()); harmless to skip since ExitProcess() closes any still-open
 * handle anyway, this is just tidiness. Safe to call even if CeLog()
 * was never called (no-op). */
void CeLogClose(void);

#endif
