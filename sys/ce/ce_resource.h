/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Dialog/control IDs for PopGB's native UI, adapted from the sister
 * ports' shared ce_resource.h. Same DialogBoxW-based approach and
 * same reasoning (this coredll doesn't export SetMenu - see that file's
 * comment for IDD_MAINMENU) - only the control layout differs, since the
 * Game Boy has 8 buttons (no X/Y/L/R, no per-core transparency option)
 * instead of a 12-button SNES-style pad.
 */
#ifndef CE_RESOURCE_H
#define CE_RESOURCE_H

/* App icon (ce_res.rc, sys/ce/icon/popgb.ico). Keep it the lowest ICON ID. */
#define IDI_MAIN 100

/* Main menu (ce_res.rc's IDD_MAINMENU): 210x154, same layout as the
 * sister PopSG's pastel-skin menu - Open ROM full width, Save/Load State
 * 2-up, the three config dialogs 3-up (icon over label), Screenshot |
 * Exit 2-up, footer hint + mascot art. Physical Back key (IDCANCEL)
 * resumes the game. */
#define IDD_MAINMENU      3000
#define IDC_MM_OPEN       3002
#define IDC_MM_EXIT       3003
#define IDC_MM_INPUT      3004
#define IDC_MM_SOUND      3005
#define IDC_MM_VIDEO      3006
#define IDC_MM_SAVESTATE  3008
#define IDC_MM_LOADSTATE  3009
#define IDC_MM_HINT       3010
#define IDC_MM_SCREENSHOT 3012 /* 3011 is IDB_MAINMENU */

/* Main-menu decoration bitmap (ce_res.rc's IDB_MAINMENU, from
 * sys/ce/icon/popgb_mascot.bmp - popgb_mascot_C_kyoudai.png alpha-composited
 * onto the menu's cream background #F0E1BC and saved as a 260x98 24bpp
 * BMP, since this device's GDI has no transparent blit). Drawn by
 * MainMenuDlgProc's WM_PAINT to the right of the IDC_MM_HINT text. */
#define IDB_MAINMENU      3011

/* Input Config dialog (ce_res.rc's IDD_INPUTCONFIG) - two-column grid: a
 * label plus a remap PUSHBUTTON showing the currently-bound key (click
 * it, then press the physical key to bind - see ce_input.c). Left column
 * is the 8 GB pad rows (Up/Down/Left/Right/A/B/Select/Start - hw.h's
 * PAD_* bits, no X/Y/L/R since the Game Boy doesn't have them) plus four
 * diagonal D-pad combo rows (ported from the sister PopNES /
 * PopSG projects' Input Config, user request): the one
 * physical key bound to each of these rows drives two directions at
 * once - see ce_input.c's CeInputMapEntry.idB and CeInputPoll(). No
 * Cancel button (see IDD_SOUNDCONFIG's comment on why) - OK plus the
 * physical Back key (IDCANCEL) both commit. */
#define IDD_INPUTCONFIG   3100
#define IDC_IC_BTN_UP     3101
#define IDC_IC_BTN_DOWN   3102
#define IDC_IC_BTN_LEFT   3103
#define IDC_IC_BTN_RIGHT  3104
#define IDC_IC_BTN_SELECT 3105
#define IDC_IC_BTN_START  3106
#define IDC_IC_BTN_A      3107
#define IDC_IC_BTN_B      3108

/* Diagonal D-pad combos - same ctrlId numbering the sister PopNES /
 * PopSG projects use for the equivalent four rows. */
#define IDC_IC_BTN_UPRIGHT   3113
#define IDC_IC_BTN_RIGHTDOWN 3114
#define IDC_IC_BTN_DOWNLEFT  3115
#define IDC_IC_BTN_LEFTUP    3116

/* Row captions - Up/Down/Left/Right/Select/Start are addressed
 * individually for the Japanese UI toggle. A/B and the four diagonal
 * combo labels stay untranslated (same "A"/"B"/"Up R"/... in both UI
 * languages, same call the sister ports made for their own single-letter
 * button labels) but still need real control IDs - not plain "-1"
 * LTEXTs any more - because the Shinonome bitmap-font repaint (see
 * ce_bmpfont.c's CeBmpFontPaintLabel(), called from WM_PAINT) needs
 * GetDlgItem() to find them. */
#define IDC_IC_LBL_UP         3120
#define IDC_IC_LBL_DOWN       3121
#define IDC_IC_LBL_LEFT       3122
#define IDC_IC_LBL_RIGHT      3123
#define IDC_IC_LBL_SELECT     3124
#define IDC_IC_LBL_START      3125
#define IDC_IC_LBL_A          3126
#define IDC_IC_LBL_B          3127
#define IDC_IC_LBL_UPRIGHT    3128
#define IDC_IC_LBL_RIGHTDOWN  3129
#define IDC_IC_LBL_DOWNLEFT   3130
#define IDC_IC_LBL_LEFTUP     3131

/* Sound Config dialog (ce_res.rc's IDD_SOUNDCONFIG) - see ce_audio.c.
 * Volume/Rate/Bits/Quality "-/value/+" spinners, same pattern and same
 * IDs as the sister ports' own Sound Config (their ce_resource.h's comment on
 * IDD_SOUNDCONFIG explains the WS_TABSTOP-on-the-value-button design).
 * Rate steps through gnuboy's pcm.hz (sound_reset() applies it); Bits/
 * Quality only affect the waveOut output format/resampler, gnuboy's own
 * PCM stays native 8-bit either way. */
#define IDD_SOUNDCONFIG      3200
#define IDC_SC_VOLUME_MINUS  3202
#define IDC_SC_VOLUME_VALUE  3203
#define IDC_SC_VOLUME_PLUS   3220
#define IDC_SC_RATE_MINUS    3221
#define IDC_SC_RATE_VALUE    3222
#define IDC_SC_RATE_PLUS     3223
#define IDC_SC_BITS_MINUS    3224
#define IDC_SC_BITS_VALUE    3225
#define IDC_SC_BITS_PLUS     3226
#define IDC_SC_QUALITY_MINUS 3227
#define IDC_SC_QUALITY_VALUE 3228
#define IDC_SC_QUALITY_PLUS  3229

#define IDC_SC_LBL_VOLUME    3210
#define IDC_SC_LBL_RATE      3211
#define IDC_SC_LBL_BITS      3212
#define IDC_SC_LBL_QUALITY   3213
#define IDC_SC_LBL_BUFFER    3214

/* Sound buffer (ring) size "-/value/+" spinner (user request) - steps
 * ce_audio.c's runtime ring size through kBufferChoices[]
 * (32/64/128/256 KB). Same held-pending-until-commit pattern as Rate/
 * Bits (s_pendingBufferBytes) since the audio drain thread reads the
 * size live. */
#define IDC_SC_BUFFER_MINUS  3230
#define IDC_SC_BUFFER_VALUE  3231
#define IDC_SC_BUFFER_PLUS   3232

/* Video Config dialog (ce_res.rc's IDD_VIDEOCONFIG) - see ce_video.c.
 * Scale is a "-/value/+" spinner stepping gnuboy's own `scale` rcvar
 * (1..4, lcd.c does the actual center+magnify blit into fb.ptr - no
 * GAPI-side scaling code needed, see the port's dev notes). Color Filter is
 * gnuboy's `colorfilter` rcvar (CGB-style faded look) - takes
 * IDD_VIDEOCONFIG's "No Sprite Limit" checkbox slot from Sample
 * Emulater's template. Frame Skip is the same "-/value/+" spinner
 * pattern, consumed by ce_video.c's occupancy-driven skip logic (not a
 * core option - gnuboy is not a libretro core). */
#define IDD_VIDEOCONFIG          3300
#define IDC_VC_SCALE_MINUS       3301
#define IDC_VC_SCALE_VALUE       3302
#define IDC_VC_SCALE_PLUS        3303
#define IDC_VC_COLORFILTER       3305
#define IDC_VC_FRAMESKIP_LABEL   3306
#define IDC_VC_FRAMESKIP_DOWN    3307
#define IDC_VC_FRAMESKIP_UP      3308

#define IDC_VC_LBL_FRAMESKIP     3309

/* Japanese/English UI toggle (see ce_lang.h) - same "-/value/+" spinner
 * pattern as the other VC_* settings above. The sister ports' shared
 * equivalent button used romaji text here since it had no CJK-capable
 * font to draw with; this port's Shinonome bitmap font (ce_bmpfont.c)
 * can render the real Japanese/English names directly instead - see
 * ce_video.c's UpdateLanguageLabel(). */
#define IDC_VC_JAPANESE          3310
#define IDC_VC_LANG_MINUS        3312
#define IDC_VC_LANG_PLUS         3313

/* Open Last Folder - checkbox removed from IDD_VIDEOCONFIG (always on,
 * see ce_fileopen.c); ID kept reserved. */
#define IDC_VC_OPENLASTFOLDER    3311

/* "Scale:" caption - same reason as IDD_INPUTCONFIG's IDC_IC_LBL_A etc.
 * above: needs a real ID so the Shinonome bitmap-font repaint can find
 * it, even though it's never translated. */
#define IDC_VC_LBL_SCALE         3314

/* "Enable Debug Log" checkbox (user request) - toggles ce_log.c's
 * CeLogSetEnabled(). Same BS_OWNERDRAW / direct-backing-data checkbox
 * handling as IDC_VC_COLORFILTER (CeLogIsEnabled() is the state store,
 * not BM_GETCHECK - see ce_bmpfont.c). Persisted as "DebugLogEnabled". */
#define IDC_VC_DEBUGLOG          3315

/* Custom ROM picker (ce_res.rc's IDD_FILEOPEN, ce_fileopen.c) - replaces
 * GetOpenFileNameW() (no Japanese folder/file name support). Ported from
 * the sister ports' shared IDD_FILEOPEN/DLGFileOpen; the extension filter
 * is gnuboy's own (.gb/.gbc/.dmg/.zip/.gz/.xz) rather than SNES ROM
 * extensions - see ce_fileopen.c. */
#define IDD_FILEOPEN      3400
#define IDC_FO_PATH       3401
#define IDC_FO_LIST       3402

/* Generic message box (ce_res.rc's IDD_MSGBOX, ce_main.c's
 * CeShowMsgBox()) - draws its text with the Shinonome bitmap font
 * instead of relying on MessageBoxW()'s system font, which has no
 * Japanese glyphs on this device any more (see ce_res.rc's comment).
 * Used for the Save/Load State result messages only - every other
 * MessageBoxW() call left in this port (ROM load failure, die()'s
 * fatal error box, RegisterClassW/CreateWindowW startup failures) is
 * always plain ASCII, so it doesn't need this. */
#define IDD_MSGBOX        3500
#define IDC_MB_TEXT       3501

/* Yes/No confirmation dialog (ce_res.rc's IDD_CONFIRM, ce_main.c's
 * CeConfirm()) - user request: Save State should ask "Save?" first. Same
 * Shinonome-bitmap-font self-drawn design as IDD_MSGBOX (MessageBoxW()
 * has no CJK glyphs on this device any more). Custom Yes/No control IDs
 * rather than IDYES/IDNO so nothing depends on those being present in
 * this toolchain's winuser.h. */
#define IDD_CONFIRM       3520
#define IDC_CF_TEXT       3521
#define IDC_CF_YES        3522
#define IDC_CF_NO         3523

#endif
