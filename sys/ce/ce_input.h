/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Physical keyboard -> Game Boy pad input for the CE frontend. Polling-
 * based (GetAsyncKeyState), not message-based, adapted from Sample
 * Emulater's core/app/ce_input.h/.c - see that file's header comment for
 * why polling (also reused by the Input Config remap-by-press dialog
 * below). gnuboy has no retro_input_state_t layer to sit behind: this
 * calls hw.h's pad_set() directly once per poll, once per each of the
 * Game Boy's 8 buttons (no X/Y/L/R - it doesn't have them), after first
 * accumulating every mapped physical key's contribution - including the
 * four diagonal D-pad combo rows (ported from the sister PopNES /
 * PopSG projects' own Input Config) that can each set two
 * of those 8 bits at once - so no bit gets silently overwritten.
 */
#ifndef CE_INPUT_H
#define CE_INPUT_H

#include <windows.h>

/* Loads any saved key mapping from the config file (falls back to the
 * built-in defaults for anything not saved yet). Call once from
 * WinMain before the first CeInputPoll(). */
void CeInputInit(void);

/* Samples all mapped keys once per frame and reflects them straight
 * into gnuboy's hw.pad via pad_set() - called from doevents() (see
 * ce_main.c), once per emu_run() frame. */
void CeInputPoll(void);

/* Call once, right after any menu/dialog (main menu or a Config sub-
 * dialog) closes and before gameplay resumes - see ce_input.c for why:
 * this device's keyboard driver can leave GetAsyncKeyState() reporting a
 * dialog-dismissing key (typically Enter, the same VK as the default A
 * binding) as still held down after the dialog that consumed its
 * WM_KEYDOWN is destroyed before the matching WM_KEYUP arrives. Every
 * mapped key is held "not pressed" for gameplay purposes until
 * CeInputPoll() actually observes it released at least once. */
void CeInputSuppressUntilReleased(void);

/* Modal native dialog (IDD_INPUTCONFIG): click a button, then press the
 * physical key to bind to that GB button. OK persists the mapping to
 * the config file; the physical Back key (IDCANCEL) does the same. */
void CeShowInputConfigDialog(HWND owner);

#endif
