/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Japanese/English UI text toggle - a persisted on/off flag consulted
 * by every dialog's own ApplyXLanguage() function (ce_main.c/
 * ce_input.c/ce_audio.c/ce_video.c/ce_fileopen.c) plus the Shinonome
 * bitmap-font renderer (ce_bmpfont.c/.h, sys/ce/ce_shinonome16.h) that
 * actually draws the text.
 *
 * This file used to also load a separate TrueType font file -
 * AddFontResourceW() on a background thread, plus a registry flag
 * guarding against repeat-registration corruption ("tofu" glyphs) on
 * every relaunch within the same power-on session (ported from the
 * earlier prototype), since removed: shipping a separate font file
 * alongside the binary is no longer needed. It's replaced by
 * CeBmpFontDrawTextW()
 * (ce_bmpfont.c) - a Shinonome bitmap font baked into the binary at
 * build time (sys/ce/tools/shinonome2c.py) instead of loaded from a
 * file at runtime, so there's no load-failure case to guard against
 * and no font resource to leak across relaunches any more. See
 * sys/ce/THIRDPARTY_LICENSES.txt for the font's license text and
 * author credit.
 */
#ifndef CE_LANG_H
#define CE_LANG_H

/* Loads the persisted UILanguageJapanese flag from CeConfigLoad()'s
 * table - call once from WinMain, after CeConfigLoad(). */
void CeLangInit(void);

/* Persisted preference (CeConfigLoad()'s table, key "UILanguageJapanese"),
 * default Japanese (1, user request) on a fresh config file - still a
 * user-visible toggle either way (Video Config's language spinner,
 * ce_video.c), it just starts on Japanese instead of English now. */
int  CeLangIsJapanese(void);
void CeLangSetJapanese(int japanese);

#endif
