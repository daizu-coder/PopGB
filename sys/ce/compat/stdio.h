/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for <stdio.h> on this cegcc toolchain - unlike compat/signal.h,
 * this one does NOT replace the real header (nothing broken about
 * #include_next here, unlike signal.h's __COREDLL__ situation): it pulls
 * in the toolchain's own stdio.h unchanged and only overrides fopen().
 *
 * Real-hardware bug (SHARP Brain PW-G5200, PopGB): the CRT's
 * narrow fopen() has to convert its char* path to wchar_t* internally to
 * call CreateFileW - the only file API this OS actually has - and it
 * does that using CP_ACP. gnuboy's loader.c builds every save/SRAM/RTC/
 * ROM path as a narrow (char*) string (it's shared, portable core code,
 * not CE-specific), and this port's own narrow<->wide conversions
 * (sys_initpath()'s savedir, PickAndLoadRom()'s romfile) used to go
 * through CP_ACP too, on the theory that matching fopen()'s own
 * assumption would make the round trip lossless.
 *
 * That reasoning turned out to only be self-consistent, not correct: a
 * real device log caught fopen() being asked to open
 * "\Storage Card\???\PopGB/...\Storage Card\GB\Druaga no Tou
 * (Japan).000" - the "???" is a Japanese folder name in the app's own
 * install path, mangled to literal question marks by
 * WideCharToMultiByte(CP_ACP, ...) (this device's ANSI code page can't
 * represent it, or the app isn't picking up CP932 the way a Japanese-
 * locale user would expect). Converting it back to wide via that same
 * CP_ACP on the way into fopen() doesn't undo the damage - "???" isn't
 * the real folder name, so the lookup fails no matter how faithfully
 * both ends agree on CP_ACP. An earlier prototype hit the identical
 * symptom and fixed it by keeping paths in UTF-8/wide form instead of
 * round-tripping through CP_ACP; the sister PopSG port does the same
 * (its own compat/stdio.h redirects fopen() to ce_fopen_utf8()).
 *
 * ce_config.c and ce_log.c already avoid this by staying in wchar_t
 * throughout and calling _wfopen() directly. loader.c can't do that
 * (portable core, narrow strings only) - so instead, this port now
 * treats every narrow path it builds (savedir/rcpath in
 * sys_initpath(), romfile in PickAndLoadRom()) as UTF-8, and fopen()
 * itself is redirected here to a wrapper (ce_fopen_utf8(), ce_sys.c)
 * that decodes UTF-8 to wide and calls the real _wfopen() - sidestepping
 * the CRT's own lossy CP_ACP conversion entirely. ASCII paths (the
 * common case - ROM files themselves, English folder names) are valid
 * UTF-8 unchanged, so this is a strict improvement with no behavior
 * change for the paths that already worked.
 */
#ifndef CE_COMPAT_STDIO_H
#define CE_COMPAT_STDIO_H

#include_next <stdio.h>

FILE *ce_fopen_utf8(const char *utf8path, const char *mode);
#define fopen ce_fopen_utf8

#endif
