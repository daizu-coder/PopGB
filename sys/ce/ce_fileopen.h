/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Custom ROM picker (ce_res.rc's IDD_FILEOPEN) - a directory-browsing
 * listbox dialog that replaces GetOpenFileNameW(). Ported from Sample
 * Emulater's own core/app/ce_fileopen.h (see that file's header comment
 * for why this device needs a self-drawn listbox instead of the common
 * file-open dialog).
 */
#ifndef CE_FILEOPEN_H
#define CE_FILEOPEN_H

#include <windows.h>

/* Loads the "Open Last Folder" preference from the config file - call
 * once from WinMain, after CeConfigLoad(). */
void CeFileOpenInit(void);

/* Shows the picker. Starting folder depends on CeFileOpenGetRememberLast()
 * below: off (the default) always starts at "\Storage Card" (falling
 * back to "\" if that doesn't exist); on, and a folder was remembered
 * from a previous successful pick, starts there instead (falling back
 * the same way if it no longer exists) - see Video Config's "Open Last
 * Folder" checkbox (ce_video.c). On a successful pick, fills outPath
 * with the chosen file's full path, remembers its folder for next time,
 * and returns 1; returns 0 if the user backed out at the root without
 * picking anything. */
int CeShowFileOpenDialog(HWND owner, wchar_t *outPath, size_t outPathCount);

/* "Open Last Folder" - always on now (its Video Config checkbox was
 * removed, see ce_fileopen.c). Kept for completeness, currently unused. */
int  CeFileOpenGetRememberLast(void);
void CeFileOpenSetRememberLast(int enable);

#endif
