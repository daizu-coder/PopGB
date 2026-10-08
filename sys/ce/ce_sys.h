/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#ifndef CE_SYS_H
#define CE_SYS_H

/* Save-file checks for loader.c (implemented in ce_sys.c, which can't
 * include gnuboy's sys.h - its sys_elapsed() takes a DWORD*, not the
 * struct timeval* sys.h declares). loader.c includes this only when
 * UNDER_CE is defined, so the other gnuboy builds are unchanged.
 *
 * sys_file_state() tells "no such file" apart from "there is a file (or
 * something) there that fopen() might still fail to open", using
 * GetFileAttributesW on the UTF-8 path; the attributes and
 * GetLastError() are written to the debug log. sys_last_error() is
 * GetLastError(), for logging right after a failed fopen(). */
#define SYS_FILE_ABSENT   0
#define SYS_FILE_PRESENT  1
#define SYS_FILE_UNKNOWN (-1) /* GetFileAttributesW failed for another reason */

int sys_file_state(const char *path);
unsigned long sys_last_error(void);

#endif
