/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for <signal.h> on this cegcc toolchain.
 *
 * /opt/cegcc/arm-mingw32ce/include/signal.h does `#include_next <signal.h>`
 * whenever __COREDLL__ is defined - which the compiler always predefines
 * as 1 for this target - and there is no further signal.h in the search
 * chain, so the include_next fails ("no include path in which to search
 * for signal.h"). Same broken-#include_next situation as this port's own
 * sys/ce/compat/{errno.h,direct.h}, if added.
 *
 * main.c installs handlers for a handful of fatal signals purely as a
 * best-effort "print something and exit" safety net (see
 * catch_signals()/fatalsignal() in main.c) - this device has no real
 * POSIX signal delivery to hook into, so signal()/raise() below are
 * no-op stubs (implemented in ce_sys.c) rather than an attempt at real
 * SEH-based signal emulation. The SIG* values match the toolchain's own
 * (unreachable) definitions, in case anything ever compares them.
 */
#ifndef CE_COMPAT_SIGNAL_H
#define CE_COMPAT_SIGNAL_H

#define SIGINT   2
#define SIGILL   4
#define SIGFPE   8
#define SIGSEGV  11
#define SIGTERM  15
#define SIGABRT  22

typedef int sig_atomic_t;
typedef void (*__ce_sig_fn_t)(int);

#define SIG_DFL ((__ce_sig_fn_t)0)
#define SIG_IGN ((__ce_sig_fn_t)1)
#define SIG_ERR ((__ce_sig_fn_t)-1)

__ce_sig_fn_t signal(int sig, __ce_sig_fn_t handler);
int raise(int sig);

#endif
