/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * ce_main.c - Windows CE entry point for PopGB's native dialog-based
 * frontend (see the port's dev notes for the architecture this replaces:
 * gnuboy's own main.c/menu.c are no longer used at all - this file is
 * gnuboy's "main()" now, built directly against the CPU/PPU/APU/loader
 * core files (cpu.c, lcd.c, sound.c, loader.c, emu.c, ...) the same way
 * main.c used to be, just with a native window/dialog UI in place of
 * menu.c's embedded text menu).
 *
 * Lifecycle: WinMain creates the fullscreen window and opens the video/
 * audio devices once, shows the custom ROM picker (ce_fileopen.c) to
 * get a first ROM, then alternates between emu_run() (gnuboy's own
 * frame loop, emu.c) and the touch-to-reveal main menu
 * (IDD_MAINMENU in ce_res.rc) for as long as the app runs - the same
 * "resume by physical Back key" model as every dialog in this port.
 * doevents() (called once per frame from inside emu_run(), see defs.h's
 * forward declaration and emu.c) is gnuboy's only per-frame hook into
 * this file: it pumps the window's message queue, polls the keyboard
 * into hw.pad (ce_input.c's CeInputPoll), and asks emu_run() to return
 * (via emu_pause(1)) the moment a screen tap requests the menu - ROM
 * switching/save-state/config dialogs all happen back in WinMain's own
 * loop, never while emu_run() is still on the call stack.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ce_app_config.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_fileopen.h"
#include "ce_input.h"
#include "ce_audio.h"
#include "ce_video.h"
#include "ce_resource.h"

#include "defs.h"
#include "rc.h"
#include "hw.h"
#include "loader.h"
#include "emu.h"
#include "fb.h"
#include "sys.h"

/* Registered once at startup (see WinMain) so gnuboy's own rcvar-backed
 * settings (lcd.c's scale/colorfilter, loader.c's savedir/savename, ...)
 * exist for ce_video.c/ce_sys.c to read and write - see the port's dev notes'
 * "残す/引き続き使うファイル" list for why these particular tables
 * (and not rcfile_exports/debug_exports/menu_exports, which belonged to
 * the discarded text-.rc/CLI-debugger/embedded-menu files) are the ones
 * still exported. */
extern rcvar_t loader_exports[], lcd_exports[], rtc_exports[],
	sound_exports[], vid_exports[], joy_exports[], pcm_exports[],
	emu_exports[];

/* loader.c defines these but only declares sram_save() in loader.h -
 * rtc_save() has no header declaration anywhere in the untouched core
 * (see the port's dev notes: loader.c is one of the "cpu周り無改修" files, so this
 * declares its existing external symbol here instead of editing that
 * header). Both are called directly (not via loader.c's own atexit()
 * cleanup, which never runs - see CeShutdown()'s comment below) so a
 * manual Exit and a crash-path die() both flush SRAM/RTC the same way
 * loader_unload()'s caller-triggered path already does. */
extern void rtc_save(void);

static HWND g_hwnd = NULL;
static volatile int g_menuRequested = 0;
static int g_romLoaded = 0;

/* Wide path of the ROM currently loaded (set by PickAndLoadRom() on a
 * successful load) - only used to name screenshot files. */
static wchar_t g_romPathW[MAX_PATH] = L"";

/* WinMain's own hInstance - LoadBitmapW(IDB_MAINMENU) uses this, same as
 * the sister PopSG. An earlier cut passed GetModuleHandleW(NULL) instead
 * and the mascot never appeared on hardware even though the RT_BITMAP
 * resource was confirmed embedded, so don't go back to that. */
static HINSTANCE g_hInstance = NULL;

HWND CeGetWindow(void)
{
	return g_hwnd;
}

static HWND CeFindTaskBar(void)
{
	return FindWindowW(L"HHTaskBar", NULL);
}

/* Direct ExitProcess(), not a normal WinMain return / WM_CLOSE /
 * PostQuitMessage() chain (a real hang on this exact device/toolchain
 * was traced to that normal path) - which
 * means the CRT's own atexit() handlers (loader.c's cleanup(), wired up
 * by loader_init()'s atexit(cleanup) call) never run either. sram_save()/
 * rtc_save() are therefore called explicitly here before tearing down,
 * same as die() below. */
static void CeShutdown(int code)
{
	if (g_romLoaded)
	{
		sram_save();
		rtc_save();
	}

	vid_close();
	pcm_close();

	{
		HWND hTaskBar = CeFindTaskBar();
		if (hTaskBar)
			ShowWindow(hTaskBar, SW_SHOW);
	}

	CeLogClose();
	ExitProcess((UINT)code);
}

/* stuff from defs.h - gnuboy's fatal-error path, called from lcd.c/
 * cpu.c/emu.c. main.c's own die() called exit(1) (letting atexit()
 * flush SRAM); this device's ExitProcess()-only shutdown discipline
 * (see CeShutdown() above) applies here too - sram_save()/rtc_save()
 * are called directly instead of relying on atexit(). */
void die(char *fmt, ...)
{
	char msg[512];
	wchar_t wmsg[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg - 1, fmt, ap);
	va_end(ap);
	msg[sizeof msg - 1] = 0;

	CeLog("die: %s", msg);

	if (g_romLoaded)
	{
		sram_save();
		rtc_save();
	}

	MultiByteToWideChar(CP_ACP, 0, msg, -1, wmsg, 512);
	MessageBoxW(g_hwnd, wmsg, CE_APP_TITLE, MB_OK | MB_ICONERROR);

	CeLogClose();
	ExitProcess(1);
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                         */
/* ------------------------------------------------------------------ */

static int LoadRom(const char *path)
{
	char *heapPath = strdup(path);
	char *romDir, *slash;

	if (g_romLoaded)
	{
		loader_unload();
		g_romLoaded = 0;
	}

	/* Point loader.c's savedir rcvar at this ROM's own folder instead of
	 * sys_initpath()'s default (this app's own .exe folder) - user
	 * request: Save State/SRAM(.sav)/RTC(.rtc) files should live next to
	 * the ROM, not next to AppMain.exe. loader_init() below builds
	 * saveprefix as "<savedir>/<romname>" (see loader.c), so this must be
	 * set before that call, and it needs to be redone on every ROM load
	 * (not just the first) since each ROM can live in a different
	 * folder. rc_setvar() strdup()s the value into the rcvar itself (see
	 * rcvars.c's rc_setvar_n(), rcv_string case), so romDir only needs to
	 * survive until that call returns. */
	romDir = strdup(path);
	slash = strrchr(romDir, '\\');
	if (!slash) slash = strrchr(romDir, '/'); /* defensive - CeShowFileOpenDialog always hands back backslash paths on this device */
	if (slash)
	{
		*slash = 0;
		rc_setvar("savedir", 1, &romDir);
	}
	/* else: no separator found (shouldn't happen from the file picker) -
	 * leave savedir as whatever it already was rather than setting it to
	 * something meaningless like "". */
	free(romDir);

	/* loader_init() stores this pointer directly (loader.c's `romfile =
	 * s`) and later free()s it itself via loader_unload() - it must be a
	 * heap allocation, not a pointer into our own caller's buffer, same
	 * as main.c's own load_rom_and_rc() (strdup(rom) before
	 * loader_init()). Not freed here on failure either, matching that
	 * function's own failure path exactly - loader.c's `romfile` static
	 * still points at it either way, so freeing it out from under that
	 * static would leave a dangling pointer instead of just an unused
	 * allocation. */
	if (loader_init(heapPath))
	{
		char *err = loader_get_error();
		wchar_t wmsg[512];
		MultiByteToWideChar(CP_ACP, 0, err && *err ? err : "ROM load failed", -1, wmsg, 512);
		/* MB_SETFOREGROUND | MB_TOPMOST - see the Save/Load State boxes'
		 * comment in MainMenuDlgProc: this is shown from the same
		 * nested-under-IDD_MAINMENU context. */
		MessageBoxW(g_hwnd, wmsg, CE_APP_TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
		return 0;
	}

	emu_reset();
	CeVideoInvalidateLastImage(); /* screenshot: don't save the previous ROM's picture */
	g_romLoaded = 1;
	return 1;
}

/* Returns 0 if the user backed out of the picker without choosing
 * anything, or if the chosen ROM failed to load (LoadRom() has already
 * shown the error) - g_romLoaded is untouched by a plain cancel (stays
 * whatever it already was), so the caller can tell "nothing changed"
 * apart from "the ROM that was running is now gone" by checking it. */
static int PickAndLoadRom(void)
{
	wchar_t wpath[MAX_PATH];
	char path[MAX_PATH * 2];

	if (!CeShowFileOpenDialog(g_hwnd, wpath, MAX_PATH))
		return 0;

	/* CP_UTF8, not CP_ACP - loader.c's saveprefix (used for SRAM/RTC/
	 * state files) is built from this narrow path, and sys/ce/compat/
	 * stdio.h's fopen() redirect (ce_fopen_utf8(), ce_sys.c) expects
	 * UTF-8 narrow strings throughout. See that header's comment for
	 * the real-hardware CP_ACP mangling this avoids. */
	WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof path, NULL, NULL);
	if (!LoadRom(path))
		return 0;
	wcsncpy(g_romPathW, wpath, MAX_PATH - 1);
	g_romPathW[MAX_PATH - 1] = L'\0';
	return 1;
}

/* ------------------------------------------------------------------ */
/* Generic message box (IDD_MSGBOX)                                    */
/* ------------------------------------------------------------------ */

/* See ce_res.rc's IDD_MSGBOX comment: MessageBoxW() draws with whatever
 * system font Windows CE finds, and this device has no CJK-capable one
 * any more (session 21 dropped the separate TrueType font file in favor
 * of fonts built into the binary) - Japanese text through it now comes
 * back as tofu. This
 * dialog instead paints its own text with the Shinonome bitmap font
 * (ce_bmpfont.c), the same way every other piece of Japanese UI text in
 * this port already does. Used for the Save/Load State result messages
 * below - the only MessageBoxW() calls in this port that ever carried
 * Japanese text. */
#define CE_MSGBOX_MAX_TEXT 128
static wchar_t s_msgBoxText[CE_MSGBOX_MAX_TEXT];

#define CE_MSGBOX_MAX_LINE 32

/* Splits text into at most two lines that each fit within maxWidth
 * (real pixels - see WM_PAINT below for why this can just trust
 * GetWindowRect() rather than reasoning about ce_res.rc's dialog-unit
 * width itself). User request: IDD_MSGBOX was narrowed to 2/3 of its
 * original width once the tofu bug (see this section's own header
 * comment) was confirmed fixed, which left this port's longer Japanese
 * messages (e.g. "state save failed") too wide for one line - Japanese
 * text has no spaces to break on, so this just greedily packs as many
 * characters as fit onto line1 and puts the remainder verbatim on
 * line2, the same simple fill approach ordinary CJK text wrapping uses.
 * If the whole string already fits, line2 comes back empty and the
 * caller draws a single centered line exactly as before. None of this
 * port's messages are long enough to ever need a third line. */
static void WrapMsgBoxText(const wchar_t *text, int maxWidth,
                            wchar_t *line1, wchar_t *line2, int lineCap)
{
	int n = (int)wcslen(text);
	int split, i;
	wchar_t probe[CE_MSGBOX_MAX_LINE];

	if (CeBmpFontGetTextWidth(text) <= maxWidth)
	{
		wcsncpy(line1, text, lineCap - 1);
		line1[lineCap - 1] = L'\0';
		line2[0] = L'\0';
		return;
	}

	split = 1; /* always keep at least one character on line1, even if it alone overflows - avoids an empty first line */
	for (i = 1; i <= n && i < CE_MSGBOX_MAX_LINE - 1; i++)
	{
		wcsncpy(probe, text, i);
		probe[i] = L'\0';
		if (CeBmpFontGetTextWidth(probe) > maxWidth)
			break;
		split = i;
	}

	wcsncpy(line1, text, split);
	line1[split] = L'\0';
	wcsncpy(line2, text + split, lineCap - 1);
	line2[lineCap - 1] = L'\0';
}

static WNDPROC s_pMsgBoxOkOrigProc = NULL;

/* Same DLGC_WANTALLKEYS/VK_RETURN/VK_SPACE/VK_ESCAPE subclass pattern as
 * every other BS_OWNERDRAW OK button in this port (MainMenuBtnCtrlProc
 * above, SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc) - BS_OWNERDRAW
 * breaks IsDialogMessage()'s normal DEFPUSHBUTTON Enter routing and
 * Escape-to-Cancel handling alike, so both have to be reimplemented by
 * hand here too. */
static LRESULT CALLBACK MsgBoxBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_GETDLGCODE)
		return DLGC_WANTALLKEYS;

	if (message == WM_KEYDOWN)
	{
		switch (wParam)
		{
		case VK_RETURN:
		case VK_SPACE:
			SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
			return 0;

		case VK_ESCAPE:
			SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
			return 0;
		}
	}

	return CallWindowProc(s_pMsgBoxOkOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MsgBoxDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		/* IDC_MB_TEXT stays a plain hidden LTEXT, same as IDC_MM_HINT/
		 * IDC_FO_PATH elsewhere in this port - repainted by WM_PAINT
		 * below instead of drawn by the control itself. */
		ShowWindow(GetDlgItem(hDlg, IDC_MB_TEXT), SW_HIDE);
		s_pMsgBoxOkOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC, (LONG_PTR)MsgBoxBtnCtrlProc);
		return TRUE;

	case WM_DRAWITEM:
		CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
		return TRUE;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc;
		RECT rc;
		wchar_t line1[CE_MSGBOX_MAX_LINE], line2[CE_MSGBOX_MAX_LINE];
		COLORREF fg;
		int rectW, rectH;

		hdc = BeginPaint(hDlg, &ps);
		GetWindowRect(GetDlgItem(hDlg, IDC_MB_TEXT), &rc);
		/* Real pixels once the dialog has actually been created (unlike
		 * ce_res.rc's own IDD_MSGBOX coordinates, which are dialog
		 * units) - see ce_res.rc's IDD_FILEOPEN comment for why those
		 * two aren't interchangeable on this device. Wrapping against
		 * this rect's real width instead of a hardcoded pixel guess
		 * means it stays correct however the dialog-unit-to-pixel
		 * conversion actually lands on real hardware. */
		MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
		rectW = rc.right - rc.left;
		rectH = rc.bottom - rc.top;

		WrapMsgBoxText(s_msgBoxText, rectW, line1, line2, CE_MSGBOX_MAX_LINE);

		fg = GetSysColor(COLOR_WINDOWTEXT);
		SetBkMode(hdc, TRANSPARENT);

		if (line2[0] == L'\0')
		{
			int x = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
			int y = rc.top + (rectH - CE_BMPFONT_HEIGHT) / 2;
			CeBmpFontDrawTextW(hdc, x, y, line1, fg);
		}
		else
		{
			int lineH = CE_BMPFONT_HEIGHT + 2; /* same row-height convention as ce_fileopen.c's listbox rows */
			int y0 = rc.top + (rectH - (CE_BMPFONT_HEIGHT + lineH)) / 2;
			int x1 = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
			int x2 = rc.left + (rectW - CeBmpFontGetTextWidth(line2)) / 2;
			CeBmpFontDrawTextW(hdc, x1, y0, line1, fg);
			CeBmpFontDrawTextW(hdc, x2, y0 + lineH, line2, fg);
		}

		EndPaint(hDlg, &ps);
		return TRUE;
	}

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hDlg, IDOK);
			return TRUE;
		}
		return FALSE;

	default:
		return FALSE;
	}
}

/* ------------------------------------------------------------------ */
/* Yes/No confirmation (IDD_CONFIRM) - user request: Save State asks     */
/* first. Shares WrapMsgBoxText()/s_msgBoxText with CeShowMsgBox above   */
/* (the two are never on screen at the same time), and the same         */
/* BS_OWNERDRAW-button key handling; just two buttons instead of one.    */
/* ------------------------------------------------------------------ */

static WNDPROC s_pConfirmBtnOrigProc = NULL;
static int     s_ssPhase             = 0; /* 0 = asking, 1 = showing result */

static LRESULT CALLBACK ConfirmBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int id = GetDlgCtrlID(hWnd);
	HWND hDlg = GetParent(hWnd);

	if (message == WM_GETDLGCODE)
		return DLGC_WANTALLKEYS | DLGC_WANTARROWS;

	if (message == WM_KEYDOWN)
	{
		switch (wParam)
		{
		case VK_RETURN:
		case VK_SPACE:
			SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
			return 0;

		case VK_ESCAPE:
			SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_CF_NO, 0), (LPARAM)hWnd);
			return 0;

		case VK_LEFT:
		case VK_RIGHT:
		case VK_UP:
		case VK_DOWN:
			if (s_ssPhase == 0) /* the result phase has only the OK button */
				SetFocus(GetDlgItem(hDlg, id == IDC_CF_YES ? IDC_CF_NO : IDC_CF_YES));
			return 0;
		}
	}

	return CallWindowProc(s_pConfirmBtnOrigProc, hWnd, message, wParam, lParam);
}

/* Save State dialog: asks, runs state_save() in place on Yes, then swaps
 * its own text/buttons to the result - never opening a second (nested)
 * modal, which is what made the main menu's "ステートセーブ" button flash
 * to the front during the old confirm -> message-box hand-off. */
static INT_PTR CALLBACK SaveStateDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		s_ssPhase = 0;
		ShowWindow(GetDlgItem(hDlg, IDC_CF_TEXT), SW_HIDE);
		SetDlgItemTextW(hDlg, IDC_CF_YES, CeLangIsJapanese() ? L"\x306f\x3044"       /* はい */ : L"Yes");
		SetDlgItemTextW(hDlg, IDC_CF_NO,  CeLangIsJapanese() ? L"\x3044\x3044\x3048" /* いいえ */ : L"No");
		s_pConfirmBtnOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC, (LONG_PTR)ConfirmBtnCtrlProc);
		SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_NO),  GWLP_WNDPROC, (LONG_PTR)ConfirmBtnCtrlProc);
		SetActiveWindow(hDlg);
		SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
		return FALSE;

	case WM_DRAWITEM:
		CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
		return TRUE;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc;
		RECT rc;
		wchar_t line1[CE_MSGBOX_MAX_LINE], line2[CE_MSGBOX_MAX_LINE];
		COLORREF fg;
		int rectW, rectH;

		hdc = BeginPaint(hDlg, &ps);
		GetWindowRect(GetDlgItem(hDlg, IDC_CF_TEXT), &rc);
		MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
		rectW = rc.right - rc.left;
		rectH = rc.bottom - rc.top;

		WrapMsgBoxText(s_msgBoxText, rectW, line1, line2, CE_MSGBOX_MAX_LINE);

		fg = GetSysColor(COLOR_WINDOWTEXT);
		SetBkMode(hdc, TRANSPARENT);

		if (line2[0] == L'\0')
		{
			int x = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
			int y = rc.top + (rectH - CE_BMPFONT_HEIGHT) / 2;
			CeBmpFontDrawTextW(hdc, x, y, line1, fg);
		}
		else
		{
			int lineH = CE_BMPFONT_HEIGHT + 2;
			int y0 = rc.top + (rectH - (CE_BMPFONT_HEIGHT + lineH)) / 2;
			int x1 = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
			int x2 = rc.left + (rectW - CeBmpFontGetTextWidth(line2)) / 2;
			CeBmpFontDrawTextW(hdc, x1, y0, line1, fg);
			CeBmpFontDrawTextW(hdc, x2, y0 + lineH, line2, fg);
		}

		EndPaint(hDlg, &ps);
		return TRUE;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_CF_YES:
			if (s_ssPhase == 0)
			{
				/* state_save()'s return value is the only way to tell
				 * here since fopen() failing (read-only media, missing
				 * directory, ...) is otherwise silent. */
				int ok = state_save(-1);
				CeLog("IDC_MM_SAVESTATE: state_save() -> %d, path=\"%s\"",
				      ok, loader_get_last_state_path());
				wcsncpy(s_msgBoxText,
				        ok ? (CeLangIsJapanese() ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3057\x305f\x3002" /* セーブしました。 */ : L"State saved.")
				           : (CeLangIsJapanese() ? L"\x30bb\x30fc\x30d6\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* セーブに失敗しました。 */ : L"Save failed."),
				        CE_MSGBOX_MAX_TEXT - 1);
				s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
				s_ssPhase = 1;
				ShowWindow(GetDlgItem(hDlg, IDC_CF_NO), SW_HIDE);
				SetDlgItemTextW(hDlg, IDC_CF_YES, L"OK");
				{   /* recentre the lone OK button */
					RECT rc, rb;
					GetClientRect(hDlg, &rc);
					GetWindowRect(GetDlgItem(hDlg, IDC_CF_YES), &rb);
					MapWindowPoints(NULL, hDlg, (POINT *)&rb, 2);
					SetWindowPos(GetDlgItem(hDlg, IDC_CF_YES), NULL,
					             (rc.right - (rb.right - rb.left)) / 2, rb.top,
					             0, 0, SWP_NOSIZE | SWP_NOZORDER);
				}
				InvalidateRect(hDlg, NULL, TRUE);
				SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
			}
			else
			{
				EndDialog(hDlg, 1);
			}
			return TRUE;
		case IDC_CF_NO:
		case IDCANCEL:
			EndDialog(hDlg, s_ssPhase ? 1 : 0);
			return TRUE;
		}
		return FALSE;

	default:
		return FALSE;
	}
}

/* Save State: asks, runs state_save(), acknowledges the result - all in
 * one self-drawn dialog (never a nested second modal). Returns 1 if the
 * save ran (user pressed Yes), 0 if declined. */
static int CeConfirmAndSaveState(HWND owner)
{
	wcsncpy(s_msgBoxText, CeLangIsJapanese()
	        ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3059\x304b\xff1f" /* セーブしますか？ */
	        : L"Save state?",
	        CE_MSGBOX_MAX_TEXT - 1);
	s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

	return (int)DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
	                       MAKEINTRESOURCEW(IDD_CONFIRM), owner, SaveStateDlgProc) == 1;
}

static void CeShowMsgBox(HWND owner, const wchar_t *text)
{
	wcsncpy(s_msgBoxText, text, CE_MSGBOX_MAX_TEXT - 1);
	s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

	/* Same MB_SETFOREGROUND/MB_TOPMOST-equivalent reasoning as the old
	 * MessageBoxW() calls this replaces: shown nested under IDD_MAINMENU,
	 * so it needs to actually come to the front on this device's window
	 * manager - see ShowMainMenu()'s own SetForegroundWindow/
	 * SetActiveWindow/SetFocus calls for the documented history here.
	 * DialogBoxW() itself already activates the dialog it creates, which
	 * covers the same ground MB_SETFOREGROUND/MB_TOPMOST covered for a
	 * real MessageBoxW(), so nothing extra is needed after it returns. */
	DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
	           MAKEINTRESOURCEW(IDD_MSGBOX), owner, MsgBoxDlgProc);
}

/* ------------------------------------------------------------------ */
/* Screenshot (ported from the sister PopSG / PopSNES)                 */
/* ------------------------------------------------------------------ */

static void PutLE16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void PutLE32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

/* Saves the paused game image as a 16-bit RGB565 BMP (BI_BITFIELDS -
 * the DIB's own pixel format, so no conversion: one fwrite per row).
 * Uses the on-screen image at the current Scale (x1/x2/Wide/Full, from
 * ce_video.c's DIB via CeVideoGetLastImage()) so the file matches what
 * the user sees. Written to "<exe-dir>\Screenshots\<ROM name>_NNN.bmp",
 * creating the folder on first use and taking the first unused number.
 * The header is built byte by byte rather than from BITMAPFILEHEADER so
 * struct packing can't shift it. _wfopen() directly (wide path, so
 * Japanese folder/ROM names are safe - the compat/stdio.h fopen()
 * redirect is only for gnuboy core's narrow UTF-8 paths). Returns 1 on
 * success. */
static int CeSaveScreenshot(void)
{
	wchar_t dir[MAX_PATH];
	wchar_t romName[MAX_PATH];
	wchar_t path[MAX_PATH + 16];
	wchar_t *p;
	const wchar_t *base;
	unsigned n, y, rowBytes, padBytes;
	unsigned long imageBytes;
	unsigned char hdr[66];
	static const unsigned char pad[4] = { 0, 0, 0, 0 };
	const void *img;
	unsigned imgW, imgH, imgPitch;
	FILE *f;

	if (!CeVideoGetLastImage(&img, &imgW, &imgH, &imgPitch))
	{
		CeLog("CeSaveScreenshot: no rendered frame yet");
		return 0;
	}

	if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
		return 0;
	p = wcsrchr(dir, L'\\');
	if (!p)
		return 0;
	p[1] = L'\0';
	if (wcslen(dir) + 12 >= MAX_PATH)
		return 0;
	wcscat(dir, L"Screenshots");
	CreateDirectoryW(dir, NULL); /* already existing is fine */
	if (GetFileAttributesW(dir) == 0xFFFFFFFF)
	{
		CeLog("CeSaveScreenshot: can't create Screenshots folder");
		return 0;
	}

	base = wcsrchr(g_romPathW, L'\\');
	wcsncpy(romName, base ? base + 1 : g_romPathW, MAX_PATH - 1);
	romName[MAX_PATH - 1] = L'\0';
	p = wcsrchr(romName, L'.');
	if (p)
		*p = L'\0';
	if (!romName[0])
		wcscpy(romName, L"PopGB");

	for (n = 1; n <= 999; n++)
	{
		_snwprintf(path, MAX_PATH + 16, L"%s\\%s_%03u.bmp", dir, romName, n);
		path[MAX_PATH + 15] = L'\0';
		if (GetFileAttributesW(path) == 0xFFFFFFFF)
			break;
	}
	if (n > 999)
	{
		CeLog("CeSaveScreenshot: all 999 numbers used");
		return 0;
	}

	rowBytes = imgW * 2;
	padBytes = (4 - (rowBytes & 3)) & 3;
	imageBytes = (unsigned long)(rowBytes + padBytes) * imgH;

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B';
	hdr[1] = 'M';
	PutLE32(hdr + 2, sizeof(hdr) + imageBytes);  /* file size */
	PutLE32(hdr + 10, sizeof(hdr));              /* pixel data offset */
	PutLE32(hdr + 14, 40);                       /* BITMAPINFOHEADER size */
	PutLE32(hdr + 18, imgW);
	PutLE32(hdr + 22, imgH);                     /* positive = bottom-up */
	PutLE16(hdr + 26, 1);                        /* planes */
	PutLE16(hdr + 28, 16);                       /* bits per pixel */
	PutLE32(hdr + 30, 3);                        /* BI_BITFIELDS */
	PutLE32(hdr + 34, imageBytes);
	PutLE32(hdr + 38, 2835);                     /* 72 dpi */
	PutLE32(hdr + 42, 2835);
	PutLE32(hdr + 54, 0xF800);                   /* R mask */
	PutLE32(hdr + 58, 0x07E0);                   /* G mask */
	PutLE32(hdr + 62, 0x001F);                   /* B mask */

	f = _wfopen(path, L"wb");
	if (!f)
	{
		CeLog("CeSaveScreenshot: can't open output file");
		return 0;
	}
	fwrite(hdr, 1, sizeof(hdr), f);
	for (y = imgH; y-- > 0; )
	{
		fwrite((const unsigned char *)img + (size_t)y * imgPitch, 1, rowBytes, f);
		if (padBytes)
			fwrite(pad, 1, padBytes, f);
	}
	if (ferror(f))
	{
		fclose(f);
		DeleteFileW(path);
		CeLog("CeSaveScreenshot: write failed");
		return 0;
	}
	fclose(f);

	CeLog("CeSaveScreenshot: saved %ux%u as #%03u", imgW, imgH, n);
	return 1;
}

static void CeScreenshotAndReport(HWND owner)
{
	if (CeSaveScreenshot())
		CeShowMsgBox(owner, CeLangIsJapanese()
			? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x3092\x4fdd\x5b58\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットを保存しました。 */
			: L"Screenshot saved.");
	else
		CeShowMsgBox(owner, CeLangIsJapanese()
			? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x306e\x4fdd\x5b58\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットの保存に失敗しました。 */
			: L"Screenshot failed.");
}

/* ------------------------------------------------------------------ */
/* Main menu dialog (IDD_MAINMENU)                                     */
/* ------------------------------------------------------------------ */

static void ApplyMainMenuLanguage(HWND hDlg)
{
	if (CeLangIsJapanese())
	{
		SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"ROM\x3092\x958b\x304f...");                     /* ROMを開く... */
		SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"\x30b9\x30c6\x30fc\x30c8\x30bb\x30fc\x30d6");    /* ステートセーブ */
		SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"\x30b9\x30c6\x30fc\x30c8\x30ed\x30fc\x30c9");    /* ステートロード */
		SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"\x30dc\x30bf\x30f3\x8a2d\x5b9a");                /* ボタン設定 */
		SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"\x30b5\x30a6\x30f3\x30c9\x8a2d\x5b9a");          /* サウンド設定 */
		SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"\x753b\x9762\x8a2d\x5b9a");                      /* 画面設定 */
		SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"\x753b\x9762\x4fdd\x5b58\x3059\x308b");          /* 画面保存する */
		SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"\x7d42\x4e86");                                  /* 終了 */
		SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"\x623b\x308b\x30ad\x30fc\x3067\x30b2\x30fc\x30e0\x518d\x958b"); /* 戻るキーでゲーム再開 */
	}
	else
	{
		SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"Open ROM...");
		SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"Save State");
		SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"Load State");
		SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"Input Cfg");
		SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"Sound Cfg");
		SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"Video Cfg");
		SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"Screenshot");
		SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"Exit");
		SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"Press Back to resume the game.");
	}

	/* The PUSHBUTTONs above are BS_OWNERDRAW (ce_res.rc) and repaint
	 * themselves from the text just set (WM_DRAWITEM below, via
	 * CeBmpFontDrawOwnerButtonTheme() with this dialog's own pastel
	 * colors - see kMainMenuTheme below and ce_bmpfont.c). IDC_MM_HINT is a
	 * plain LTEXT - STATIC controls have no ownerdraw style - so it's
	 * hidden here instead and repainted by this dialog's own WM_PAINT
	 * via CeBmpFontPaintLabel(), which still reads the text set above
	 * with GetWindowTextW() even though the control itself is hidden. */
	ShowWindow(GetDlgItem(hDlg, IDC_MM_HINT), SW_HIDE);

	/* User report: after switching languages from Video Config, the hint
	 * label sometimes showed the old and new language mixed together
	 * (e.g. the top half of the glyphs from one language, the bottom
	 * half from the other). CeBmpFontPaintLabel() (ce_bmpfont.c) only
	 * SetPixel()s the "on" bits of each glyph with a transparent
	 * background - it never clears the label's rect first, so it
	 * depends entirely on WM_ERASEBKGND having already wiped that area
	 * before WM_PAINT runs. The only redraw that was happening here came
	 * incidentally from the Video Config sub-dialog closing over part of
	 * this dialog, which only invalidates the region it actually
	 * covered - if that region didn't fully contain the hint label's
	 * rect (it doesn't for this layout), the stale glyph bits from the
	 * old-language text stayed on screen, overdrawn in place by the new
	 * text with no erase pass in between. Forcing a full erase+redraw of
	 * the whole dialog here (called every time this text can change -
	 * WM_INITDIALOG and after Video Config returns) makes the label's
	 * own repaint unconditional instead of relying on that leftover
	 * invalidation. */
	InvalidateRect(hDlg, NULL, TRUE);
}

/* Pastel/rounded main-menu skin, ported as-is from the sister PopSG
 * (its kMainMenuTheme after the on-device-confirmed redesign rounds -
 * pastel fills with darker borders, a deep-ivory window background,
 * per-button vector icons, blue/white reverse video on focus; see
 * CeBmpFontDrawOwnerButtonTheme() in ce_bmpfont.c). Applies only to this
 * dialog's own buttons via WM_DRAWITEM and its own client background via
 * WM_ERASEBKGND - every other dialog keeps the plain gray
 * CeBmpFontDrawOwnerButton() look. */
#define CE_MENU_BG_CREAM   RGB(0xF0, 0xE1, 0xBC)
#define CE_MENU_TEXT_DARK  RGB(0x2A, 0x2C, 0x30)

typedef struct { int id; COLORREF bg, border; CeMenuIcon icon; int stacked; } CeMenuButtonTheme;

static const CeMenuButtonTheme kMainMenuTheme[] = {
	{ IDC_MM_OPEN,       RGB(0x6F, 0xA8, 0xDC), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_OPEN,       0 }, /* blue */
	{ IDC_MM_SAVESTATE,  RGB(0xF5, 0xEC, 0x9E), RGB(0x6E, 0x66, 0x12), CE_MENU_ICON_SAVE,       0 }, /* pastel lemon */
	{ IDC_MM_LOADSTATE,  RGB(0xBF, 0xE3, 0xD0), RGB(0x1D, 0x5A, 0x3C), CE_MENU_ICON_LOAD,       0 }, /* mint */
	{ IDC_MM_VIDEO,      RGB(0xC9, 0xE4, 0xB0), RGB(0x3C, 0x5A, 0x1B), CE_MENU_ICON_VIDEO,      1 }, /* green */
	{ IDC_MM_SOUND,      RGB(0xB9, 0xD7, 0xEE), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_SOUND,      1 }, /* blue */
	{ IDC_MM_INPUT,      RGB(0xF2, 0xB8, 0xC6), RGB(0x7A, 0x2E, 0x4C), CE_MENU_ICON_INPUT,      1 }, /* pink */
	{ IDC_MM_SCREENSHOT, RGB(0xD9, 0xCC, 0xF0), RGB(0x4E, 0x34, 0x80), CE_MENU_ICON_SCREENSHOT, 0 }, /* lavender */
	{ IDC_MM_EXIT,       RGB(0xF0, 0xA9, 0xA0), RGB(0x7A, 0x23, 0x18), CE_MENU_ICON_EXIT,       0 }, /* coral */
};

/* sys/ce/icon/popgb_mascot.bmp, embedded as IDB_MAINMENU (ce_res.rc). Loaded
 * lazily in WM_INITDIALOG, blitted by WM_PAINT into the blank strip
 * right of the IDC_MM_HINT text, freed in WM_DESTROY. */
static HBITMAP s_hMainMenuBmp = NULL;

static WNDPROC s_pMainMenuOrigProc = NULL;

/* Reading order matching ce_res.rc's IDD_MAINMENU layout (top-to-
 * bottom, left-to-right within a row): Open (full width), Save/Load
 * State (2-up), Video/Sound/Input (3-up), Screenshot/Exit (2-up). Used both
 * for WM_INITDIALOG's subclassing loop and MainMenuNeighbor()'s
 * wraparound arrow-key cycling below. */
static const int kMainMenuButtonIds[] = {
	IDC_MM_OPEN, IDC_MM_SAVESTATE, IDC_MM_LOADSTATE,
	IDC_MM_VIDEO, IDC_MM_SOUND, IDC_MM_INPUT, IDC_MM_SCREENSHOT, IDC_MM_EXIT,
};
#define CE_MAINMENU_BUTTON_COUNT (sizeof(kMainMenuButtonIds) / sizeof(kMainMenuButtonIds[0]))

/* Skips disabled buttons (Save/Load State while g_romLoaded is still 0 -
 * user request: focus used to land on them anyway and just sit there,
 * since EnableWindow() alone only blocks activation, not this dialog's
 * own custom arrow-key cycling below). Bounded to CE_MAINMENU_BUTTON_COUNT
 * steps so it can't spin forever if every button were ever disabled at
 * once (never happens in practice - Open/Exit are always enabled). */
static int MainMenuNeighbor(HWND hDlg, int id, int delta)
{
	int idx, step;
	for (idx = 0; idx < (int)CE_MAINMENU_BUTTON_COUNT; idx++)
		if (kMainMenuButtonIds[idx] == id)
			break;
	if (idx >= (int)CE_MAINMENU_BUTTON_COUNT)
		return id;

	for (step = 1; step <= (int)CE_MAINMENU_BUTTON_COUNT; step++)
	{
		int nextIdx = ((idx + delta * step) % (int)CE_MAINMENU_BUTTON_COUNT + (int)CE_MAINMENU_BUTTON_COUNT) % (int)CE_MAINMENU_BUTTON_COUNT;
		int nextId = kMainMenuButtonIds[nextIdx];
		if (IsWindowEnabled(GetDlgItem(hDlg, nextId)))
			return nextId;
	}
	return id;
}

/* PUSHBUTTON's built-in WM_KEYDOWN -> BN_CLICKED conversion for
 * VK_RETURN/VK_SPACE stopped firing on this device once these buttons
 * became BS_OWNERDRAW (ce_res.rc, this session): real hardware
 * confirmed touch/stylus taps still worked (WM_LBUTTONUP path is
 * unaffected) but the physical decide key did nothing on a focused
 * button. A first attempt left WM_GETDLGCODE unhandled, hoping the
 * button class's default response would still route WM_KEYDOWN here -
 * real hardware confirmed that did NOT fix it, which points at
 * IsDialogMessage() never delivering WM_KEYDOWN to this control at all
 * in the first place (most likely because IDD_MAINMENU has no
 * DEFPUSHBUTTON for Enter to fall back to, unlike Sound/Video/Input
 * Config's own IDOK). This subclass now claims WM_GETDLGCODE outright
 * (DLGC_WANTARROWS | DLGC_WANTALLKEYS, the same claim
 * SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc already make) and
 * implements its own arrow-key focus cycling via MainMenuNeighbor()
 * above instead of leaning on the standard dialog navigation that
 * claim steals control of - the same tradeoff those three dialogs
 * already made. */
static LRESULT CALLBACK MainMenuBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int id = GetDlgCtrlID(hWnd);
	HWND hDlg = GetParent(hWnd);

	if (message == WM_GETDLGCODE)
		return DLGC_WANTARROWS | DLGC_WANTALLKEYS;

	if (message == WM_KEYDOWN)
	{
		switch (wParam)
		{
		case VK_UP:
		case VK_LEFT:
			SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, -1)));
			return 0;

		case VK_DOWN:
		case VK_RIGHT:
			SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, 1)));
			return 0;

		case VK_RETURN:
		case VK_SPACE:
			SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
			return 0;

		case VK_ESCAPE:
			/* Physical Back - user report: claiming DLGC_WANTALLKEYS
			 * above (needed so VK_RETURN/VK_SPACE reach this button at
			 * all - see this proc's own header comment) also steals
			 * Escape away from IsDialogMessage()'s normal Cancel-key
			 * handling, which is what used to turn it into IDCANCEL and
			 * resume the game. Every other DLGC_WANTALLKEYS subclass in
			 * this port (SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc)
			 * already forwards Escape to IDCANCEL by hand for the same
			 * reason - this one just never did. */
			SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
			return 0;
		}
	}

	return CallWindowProc(s_pMainMenuOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MainMenuDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		unsigned i;
		EnableWindow(GetDlgItem(hDlg, IDC_MM_SAVESTATE), g_romLoaded);
		EnableWindow(GetDlgItem(hDlg, IDC_MM_LOADSTATE), g_romLoaded);
		EnableWindow(GetDlgItem(hDlg, IDC_MM_SCREENSHOT), g_romLoaded);
		ApplyMainMenuLanguage(hDlg);

		s_pMainMenuOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_MM_OPEN), GWLP_WNDPROC);
		for (i = 0; i < CE_MAINMENU_BUTTON_COUNT; i++)
			SetWindowLongPtrW(GetDlgItem(hDlg, kMainMenuButtonIds[i]), GWLP_WNDPROC, (LONG_PTR)MainMenuBtnCtrlProc);

		if (!s_hMainMenuBmp)
		{
			s_hMainMenuBmp = LoadBitmapW(g_hInstance, MAKEINTRESOURCEW(IDB_MAINMENU));
			if (!s_hMainMenuBmp)
				CeLog("MainMenu: LoadBitmapW(IDB_MAINMENU) failed, err=%lu", (unsigned long)GetLastError());
		}
		return TRUE;
	}

	case WM_DRAWITEM:
	{
		const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
		unsigned i;
		for (i = 0; i < sizeof(kMainMenuTheme) / sizeof(kMainMenuTheme[0]); i++)
		{
			if (kMainMenuTheme[i].id == (int)dis->CtlID)
			{
				CeBmpFontDrawOwnerButtonTheme(dis, kMainMenuTheme[i].bg, kMainMenuTheme[i].border, CE_MENU_TEXT_DARK,
				                              kMainMenuTheme[i].icon, kMainMenuTheme[i].stacked, CE_MENU_BG_CREAM);
				return TRUE;
			}
		}
		CeBmpFontDrawOwnerButton(dis); /* fallback, shouldn't hit any control here */
		return TRUE;
	}

	/* Cream client background for the pastel skin - painted here rather
	 * than via a class brush so it's scoped to this dialog. Created once
	 * and kept for the process lifetime (fires on every erase, e.g. each
	 * time a sub-dialog closes over part of this one). */
	case WM_ERASEBKGND:
	{
		static HBRUSH s_hCreamBrush = NULL;
		RECT rc;
		if (!s_hCreamBrush)
			s_hCreamBrush = CreateSolidBrush(CE_MENU_BG_CREAM);
		GetClientRect(hDlg, &rc);
		FillRect((HDC)wParam, &rc, s_hCreamBrush);
		return TRUE;
	}

	case WM_DESTROY:
		if (s_hMainMenuBmp)
		{
			DeleteObject(s_hMainMenuBmp);
			s_hMainMenuBmp = NULL;
		}
		return FALSE;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hDlg, &ps);
		CeBmpFontPaintLabel(hdc, hDlg, IDC_MM_HINT);

		/* icon/popgb_mascot.bmp (IDB_MAINMENU) in the blank strip to the right
		 * of the "戻るキーでゲーム再開" hint - measured off the hint
		 * control's own rect plus the Shinonome text width, so it starts
		 * just past the text and runs to the dialog's right/bottom edge,
		 * aspect ratio preserved, bottom-right aligned (same as PopSG). */
		if (s_hMainMenuBmp)
		{
			HWND hHint = GetDlgItem(hDlg, IDC_MM_HINT);
			RECT rcHint, rcClient;
			wchar_t hintText[128];
			BITMAP bm;

			hintText[0] = 0;
			GetWindowTextW(hHint, hintText, 128);
			GetWindowRect(hHint, &rcHint);
			MapWindowPoints(NULL, hDlg, (POINT *)&rcHint, 2);
			GetClientRect(hDlg, &rcClient);

			if (GetObject(s_hMainMenuBmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
			{
				long boxL = rcHint.left + CeBmpFontGetTextWidth(hintText) + 8;
				long boxR = rcClient.right - 4;
				long boxT = rcHint.top;
				long boxB = rcClient.bottom - 2;
				long boxW = boxR - boxL;
				long boxH = boxB - boxT;

				if (boxW > 8 && boxH > 8)
				{
					long drawW = boxW;
					long drawH = drawW * bm.bmHeight / bm.bmWidth;
					HDC memDC;
					HGDIOBJ oldBmp;

					if (drawH > boxH)
					{
						drawH = boxH;
						drawW = drawH * bm.bmWidth / bm.bmHeight;
					}

					/* No SetStretchBltMode() - this coredll doesn't export
					 * it; CE's default (COLORONCOLOR) is fine for this
					 * decorative shrink. */
					memDC = CreateCompatibleDC(hdc);
					oldBmp = SelectObject(memDC, s_hMainMenuBmp);
					StretchBlt(hdc, (int)(boxR - drawW), (int)(boxB - drawH),
					           (int)drawW, (int)drawH,
					           memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
					SelectObject(memDC, oldBmp);
					DeleteDC(memDC);
				}
			}
		}

		EndPaint(hDlg, &ps);
		return TRUE;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_MM_OPEN:
			PickAndLoadRom();
			EndDialog(hDlg, IDC_MM_OPEN);
			return TRUE;

		case IDC_MM_SAVESTATE:
			/* Confirm, save, and acknowledge the result in one
			 * self-drawn dialog (never a nested second modal). Yes
			 * closes back to gameplay; No leaves the menu open. */
			if (g_romLoaded && CeConfirmAndSaveState(hDlg))
				EndDialog(hDlg, IDC_MM_SAVESTATE);
			return TRUE;

		case IDC_MM_LOADSTATE:
			if (g_romLoaded)
			{
				/* Same reasoning as Save above - a missing save file
				 * (nothing saved yet) or a read failure used to look
				 * exactly like "the button did nothing" from the
				 * player's side, with the menu just closing back to
				 * gameplay in whatever state it was already in. */
				int ok = state_load(-1);
				CeLog("IDC_MM_LOADSTATE: state_load() -> %d, path=\"%s\"",
				      ok, loader_get_last_state_path());
				CeShowMsgBox(hDlg,
				             ok ? (CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x3057\x307e\x3057\x305f\x3002" /* ロードしました。 */ : L"State loaded.")
				                : (CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */ : L"Load failed."));
			}
			EndDialog(hDlg, IDC_MM_LOADSTATE);
			return TRUE;

		case IDC_MM_SCREENSHOT:
			if (g_romLoaded)
				CeScreenshotAndReport(hDlg);
			return TRUE; /* stays open, like PopSG */

		case IDC_MM_INPUT:
			CeShowInputConfigDialog(hDlg);
			return TRUE;

		case IDC_MM_SOUND:
			CeShowSoundConfigDialog(hDlg);
			return TRUE;

		case IDC_MM_VIDEO:
			CeShowVideoConfigDialog(hDlg);
			ApplyMainMenuLanguage(hDlg); /* Video Config is where the language toggle lives */
			return TRUE;

		case IDC_MM_EXIT:
			EndDialog(hDlg, IDC_MM_EXIT);
			return TRUE;

		case IDCANCEL: /* physical Back - resume the game */
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		}
		return FALSE;

	default:
		return FALSE;
	}
}

/* Returns 0 if the user chose Exit, 1 otherwise (resume, or any config/
 * Open ROM/save-state action - g_romLoaded may have changed, WinMain's
 * loop re-checks it before calling emu_run() again). */
static int ShowMainMenu(void)
{
	int result;

	pcm_pause(1); /* stop any still-queued audio immediately - nothing is producing more of it while the menu is up */

	/* Historically needed because GXOpenDisplay() (the old GAPI backend's
	 * vid_init()) claimed exclusive full-screen access, and showing a
	 * modal dialog on top of that without suspending first was against
	 * GAPI's documented contract. ce_video.c now uses plain GDI (session
	 * 20 - see its header comment), so CeVideoSuspend()/CeVideoResume()
	 * are no-ops, but the call sites are left in place since they're
	 * harmless and match every other modal dialog's wrapping below. */
	CeVideoSuspend();

	result = (int)DialogBoxW((HINSTANCE)GetWindowLongPtrW(g_hwnd, GWLP_HINSTANCE),
	                          MAKEINTRESOURCEW(IDD_MAINMENU), g_hwnd, MainMenuDlgProc);

	CeVideoResume();

	/* The menu (or one of the Config sub-dialogs it launched) may have
	 * left a dialog-dismissing key looking still-held to
	 * GetAsyncKeyState() - see ce_input.c's CeInputSuppressUntilReleased()
	 * for why. Do this for every path back to gameplay, not just Exit,
	 * since Sound/Video/Input Config are reachable from here too. */
	CeInputSuppressUntilReleased();

	/* Belt-and-braces alongside the above: DialogBoxW() destroys the
	 * dialog on EndDialog(), but this device's window manager doesn't
	 * always hand real input focus back to g_hwnd afterward on its own -
	 * and on this hardware GetAsyncKeyState() has a documented history
	 * (see ce_input.c) of tracking whichever window actually
	 * has focus rather than being a true focus-independent global poll.
	 * Reasserting focus here, once, right as gameplay resumes, is cheap
	 * and can only help if that's what's making CeInputPoll() read stale
	 * key state after the menu closes. */
	CeLog("ShowMainMenu: before refocus - fg=0x%08X active=0x%08X focus=0x%08X g_hwnd=0x%08X",
	      (unsigned)(UINT_PTR)GetForegroundWindow(), (unsigned)(UINT_PTR)GetActiveWindow(),
	      (unsigned)(UINT_PTR)GetFocus(), (unsigned)(UINT_PTR)g_hwnd);

	SetForegroundWindow(g_hwnd);
	SetActiveWindow(g_hwnd);
	SetFocus(g_hwnd);

	CeLog("ShowMainMenu: after refocus  - fg=0x%08X active=0x%08X focus=0x%08X g_hwnd=0x%08X",
	      (unsigned)(UINT_PTR)GetForegroundWindow(), (unsigned)(UINT_PTR)GetActiveWindow(),
	      (unsigned)(UINT_PTR)GetFocus(), (unsigned)(UINT_PTR)g_hwnd);

	return (result != IDC_MM_EXIT);
}

/* ------------------------------------------------------------------ */
/* Window / message pump                                               */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_LBUTTONDOWN:
		/* Touch-to-reveal menu (see the port's dev notes - the Game Boy pad has no
		 * spare button for a dedicated menu key). Deferred to doevents()
		 * instead of showing the dialog right here: this runs from
		 * inside emu_run()'s own DispatchMessageW() call (see doevents()
		 * below), and Open ROM/state load need loader_unload()+
		 * loader_init()+emu_reset() to happen with emu_run() off the
		 * call stack entirely, not nested inside it. */
		g_menuRequested = 1;
		return 0;

	case WM_DESTROY:
		CeShutdown(0);
		return 0;

	case WM_PAINT:
	{
		/* Before any ROM has loaded, this plain black fill is all
		 * there is to redraw. Once gnuboy is actually running, the
		 * GDI video backend (ce_video.c, session 20) draws straight
		 * into this window's own client area, so a WM_PAINT here -
		 * e.g. a modal dialog covering part of the game window, then
		 * closing - needs the last rendered frame redrawn too, or
		 * the exposed region stays black until the next real emulated
		 * frame (which won't happen at all while emu_run() is paused
		 * for that same dialog). See CeVideoForceRepaint()'s own
		 * comment for why the old GAPI backend never needed this. */
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
		EndPaint(hwnd, &ps);
		if (g_romLoaded)
			CeVideoForceRepaint();
		return 0;
	}

	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

/* Dynamically resolved (not every coredll.lib import set used by this
 * toolchain exports it)
 * and called once a frame below. This port's dev notes record a prior,
 * inconclusive test of this same call: it was tried against what turned
 * out to be a *different* bug (a dialog-dismissing key left reading
 * "held" by GetAsyncKeyState - fixed separately, see
 * CeInputSuppressUntilReleased()), so reverting it then didn't actually
 * rule this out for the "resume feels heavy" symptom real-hardware
 * logging now shows: a near-constant ~170-180ms/frame that two different
 * app-side timing/audio fixes (this file's own doevents() gap log,
 * ce_audio.c's pcm_pause()) left completely unchanged, but that a full
 * ROM reload temporarily cleared - consistent with this device's power
 * manager throttling the CPU clock during continuous gameplay (whose
 * frame loop calls Sleep() every frame, i.e. looks idle to it) and only
 * un-throttling on real disk I/O / heavier bursts of work. */
typedef BOOL (WINAPI *PFN_SystemIdleTimerReset)(void);
static PFN_SystemIdleTimerReset s_pSystemIdleTimerReset = NULL;
static int s_idleTimerResolved = 0;

static void CeResetIdleTimer(void)
{
	if (!s_idleTimerResolved)
	{
		HMODULE hCoredll = GetModuleHandleW(L"coredll.dll");
		if (hCoredll)
			s_pSystemIdleTimerReset = (PFN_SystemIdleTimerReset)
				GetProcAddressA(hCoredll, "SystemIdleTimerReset");
		s_idleTimerResolved = 1;
		/* One-shot: confirms whether this experiment is even exercised on
		 * this device, since a silent resolution failure would otherwise
		 * look identical in the doevents() slow-frame log to "we called it
		 * and it didn't help". */
		CeLog("CeResetIdleTimer: SystemIdleTimerReset %s",
		      s_pSystemIdleTimerReset ? "resolved" : "NOT FOUND in coredll.dll");
	}
	if (s_pSystemIdleTimerReset)
		s_pSystemIdleTimerReset();
}

#ifdef CE_PERF_DIAG
/* Diagnostic-only per-frame timing breakdown (see sys.h/Makefile.ce's own
 * comments) - answers "is a real-hardware slowdown/audio dropout coming
 * from LR35902 execution (+ its piggybacked LCD scanline rendering) or
 * from APU mixing / this port's own screen blit / audio ring copy?"
 * ce_perf_begin()/ce_perf_end() are called from emu.c (core, guarded by
 * the same #ifdef) in matched pairs; this file just accumulates ms per
 * slot and, once every CE_PERF_LOG_INTERVAL_FRAMES frames, writes one
 * summary line to PopGB_debug.log and resets. All of this runs on the
 * single CPU/PPU thread (same thread that calls doevents()), so no
 * locking is needed here. */
#define CE_PERF_NUM_SLOTS 2
#define CE_PERF_LOG_INTERVAL_FRAMES 60

static DWORD    s_perfSlotStart[CE_PERF_NUM_SLOTS];
static unsigned s_perfSlotAccumMs[CE_PERF_NUM_SLOTS];
static DWORD    s_perfWindowStartTick = 0;
static unsigned s_perfFrameCount = 0;

/* 2026-08-24 follow-up: the first CE_PERF_DIAG pass showed cpu+lcd/
 * apuMix/blit/audioSubmit all behaving normally even during real-hardware
 * doevents() gap spikes of 887/179/162ms during actual gameplay (not
 * menu transitions) - i.e. the stall isn't in any of those, it's
 * somewhere inside doevents() itself. This second pass times every
 * individual piece of doevents()'s own body (CeResetIdleTimer, the
 * PeekMessage/DispatchMessage pump, CeInputPoll, this frame's own
 * Sleep(), and - the leading suspect - the CeLog() calls themselves,
 * since CeLog() opens/writes/closes the log file on every single call
 * and flash I/O stalls are a plausible source of the exact kind of rare,
 * large, otherwise-unexplained pause seen in the first pass's log). */
static unsigned s_perfIdleTimerMs  = 0;
static unsigned s_perfMsgPumpMs    = 0;
static unsigned s_perfInputPollMs  = 0;
static unsigned s_perfSleepMs      = 0;
static unsigned s_perfSlowLogMs    = 0; /* the conditional "slow frame" CeLog() call */
static unsigned s_perfSlowLogCount = 0;
static unsigned s_perfSummaryLogMs = 0; /* this file's own "perf:" CeLog() call, timed and folded into the *next* window's report */

void ce_perf_begin(int slot)
{
	s_perfSlotStart[slot] = GetTickCount();
}

void ce_perf_end(int slot)
{
	s_perfSlotAccumMs[slot] += (unsigned)(GetTickCount() - s_perfSlotStart[slot]);
}

/* Called once per frame from doevents() below, after this frame's own
 * work (emu.c's slot timing, ce_video.c's blit timing, ce_audio.c's
 * submit/drop counters) has already happened for this frame. */
static void CePerfMaybeLogSummary(void)
{
	DWORD now;
	unsigned totalMs, cpuLcdMs, apuMs, blitMs, submitMs;
	unsigned overflowDropBytes, overflowDropCount, timeoutDropCount;
	unsigned idleTimerMs, msgPumpMs, inputPollMs, sleepMs, slowLogMs, slowLogCount, summaryLogMs;
	unsigned otherMs;
	DWORD selfLogStart;

	if (s_perfWindowStartTick == 0)
	{
		/* First call ever: every one of this frame's own accumulators -
		 * this file's doevents()-stage ones (idleTimer/msgPump/inputPoll/
		 * sleep/slowLog) *and* ce_video.c's/ce_audio.c's own get-and-reset
		 * ones (blit/audioSubmit/drop counters), plus emu.c's cpu+lcd/
		 * apuMix slots below - already started accumulating this same
		 * frame's work *before* this function ever runs (they're
		 * unconditional, this function is the one place with a "have I
		 * started a window yet" gate). A first attempt at this fix only
		 * zeroed the doevents()-stage ones and wrongly assumed the rest
		 * had no equivalent problem - real-hardware logging then caught
		 * the exact same wraparound again ("other=4294967264ms(4110013%)"
		 * on the first perf: line of a later session), because cpu+lcd/
		 * blit/audioSubmit were still leaking frame 1's time in unpaired
		 * with any slice of totalMs. Discarding *all* of them here (not
		 * just resetting - the video/audio ones only expose a
		 * get-and-reset accessor, so call and throw away) makes every
		 * bucket's clock start exactly in sync with s_perfWindowStartTick. */
		s_perfWindowStartTick = GetTickCount();
		s_perfSlotAccumMs[CE_PERF_SLOT_CPU_LCD] = 0;
		s_perfSlotAccumMs[CE_PERF_SLOT_APU_MIX] = 0;
		(void)CeVideoGetAndResetBlitMs();
		(void)CeAudioGetAndResetSubmitMs();
		(void)CeAudioGetAndResetOverflowDropBytes();
		(void)CeAudioGetAndResetOverflowDropCount();
		(void)CeAudioGetAndResetTimeoutDropCount();
		s_perfIdleTimerMs  = 0;
		s_perfMsgPumpMs    = 0;
		s_perfInputPollMs  = 0;
		s_perfSleepMs      = 0;
		s_perfSlowLogMs    = 0;
		s_perfSlowLogCount = 0;
		return;
	}

	s_perfFrameCount++;
	if (s_perfFrameCount < CE_PERF_LOG_INTERVAL_FRAMES)
		return;

	now = GetTickCount();
	totalMs  = (unsigned)(now - s_perfWindowStartTick);
	cpuLcdMs = s_perfSlotAccumMs[CE_PERF_SLOT_CPU_LCD];
	apuMs    = s_perfSlotAccumMs[CE_PERF_SLOT_APU_MIX];
	blitMs   = CeVideoGetAndResetBlitMs();
	submitMs = CeAudioGetAndResetSubmitMs();
	overflowDropBytes = CeAudioGetAndResetOverflowDropBytes();
	overflowDropCount = CeAudioGetAndResetOverflowDropCount();
	timeoutDropCount  = CeAudioGetAndResetTimeoutDropCount();
	idleTimerMs  = s_perfIdleTimerMs;
	msgPumpMs    = s_perfMsgPumpMs;
	inputPollMs  = s_perfInputPollMs;
	sleepMs      = s_perfSleepMs;
	slowLogMs    = s_perfSlowLogMs;
	slowLogCount = s_perfSlowLogCount;
	summaryLogMs = s_perfSummaryLogMs;
	/* Whatever's still left over after every named bucket above -
	 * GetTickCount() rounding and rtc_tick() (core, not worth its own
	 * slot - always a handful of integer ops). Should sit near 0 now;
	 * if it's still large, the stall is happening somewhere this second
	 * pass still doesn't name explicitly. */
	otherMs = totalMs - cpuLcdMs - apuMs - blitMs - submitMs
	          - idleTimerMs - msgPumpMs - inputPollMs - sleepMs - slowLogMs - summaryLogMs;

	selfLogStart = GetTickCount();
	CeLog("perf: %ums/%uframes (avg %ums/frame) cpu+lcd=%ums(%u%%) apuMix=%ums(%u%%) "
	      "blit=%ums(%u%%) audioSubmit=%ums(%u%%) idleTimer=%ums(%u%%) msgPump=%ums(%u%%) "
	      "inputPoll=%ums(%u%%) sleep=%ums(%u%%) slowLog=%ums(%u%%)/%uevents "
	      "summaryLog=%ums(%u%%) other=%ums(%u%%) "
	      "audioOverflowDrop=%uB/%uevents audioTimeoutDrop=%uevents",
	      totalMs, s_perfFrameCount, totalMs / s_perfFrameCount,
	      cpuLcdMs, totalMs ? (cpuLcdMs * 100) / totalMs : 0,
	      apuMs,    totalMs ? (apuMs    * 100) / totalMs : 0,
	      blitMs,   totalMs ? (blitMs   * 100) / totalMs : 0,
	      submitMs, totalMs ? (submitMs * 100) / totalMs : 0,
	      idleTimerMs, totalMs ? (idleTimerMs * 100) / totalMs : 0,
	      msgPumpMs,   totalMs ? (msgPumpMs   * 100) / totalMs : 0,
	      inputPollMs, totalMs ? (inputPollMs * 100) / totalMs : 0,
	      sleepMs,     totalMs ? (sleepMs     * 100) / totalMs : 0,
	      slowLogMs,   totalMs ? (slowLogMs   * 100) / totalMs : 0, slowLogCount,
	      summaryLogMs, totalMs ? (summaryLogMs * 100) / totalMs : 0,
	      otherMs,  totalMs ? (otherMs  * 100) / totalMs : 0,
	      overflowDropBytes, overflowDropCount, timeoutDropCount);
	/* This call's own duration couldn't be included in the line it just
	 * wrote (chicken-and-egg), so fold it into next window's summaryLog
	 * bucket instead of losing it. */
	s_perfSummaryLogMs = (unsigned)(GetTickCount() - selfLogStart);

	s_perfSlotAccumMs[CE_PERF_SLOT_CPU_LCD] = 0;
	s_perfSlotAccumMs[CE_PERF_SLOT_APU_MIX] = 0;
	s_perfIdleTimerMs  = 0;
	s_perfMsgPumpMs    = 0;
	s_perfInputPollMs  = 0;
	s_perfSleepMs      = 0;
	s_perfSlowLogMs    = 0;
	s_perfSlowLogCount = 0;
	s_perfWindowStartTick = now;
	s_perfFrameCount = 0;
}
#endif /* CE_PERF_DIAG */

/* Frame limiter high-water mark, ported from the sister PopSG
 * port's own main loop (dev notes round 38): while a
 * waveOut device is open, hold a frame back while the audio ring has
 * more than this many ms of not-yet-played backlog queued ahead of real
 * time, instead of pacing off GetTickCount() alone. PopSG introduced
 * this to fix light cores free-running ahead of real time once the GDI
 * blit lost its implicit vsync wait, and noted it also damps the
 * chronic ring-overrun/underrun drift a plain wall-clock limiter leaves
 * to WinCE's coarse Sleep()/GetTickCount() granularity - the same drift
 * this port's own doevents() limiter is exposed to. Same starting value
 * as PopSG (unvalidated on this port's own hardware - see the dev
 * notes' account of PopSG's own tuning tradeoff: lower = less latency/
 * less underrun headroom, higher = the reverse). */
#define CE_FRAME_PACING_HIGH_MS 64

/* gnuboy's only per-frame hook into this file (emu.c's emu_run(), once
 * per frame - see defs.h's forward declaration).
 *
 * Also does this port's actual frame-rate limiting now. emu.c's own
 * pacing (`if (!pcm_submit()) { delay = framelen - sys_elapsed(...); ...
 * }`) turned out to almost never run: pcm_submit() (ce_audio.c) returns
 * 1 (success) on essentially every call, so that branch - emu.c's only
 * throttle - was close to dead code, and with it, real hardware ran
 * every frame back-to-back with nothing pacing it (see ce_audio.c's own
 * header comment - an earlier prototype hit the
 * identical bug class: frame rate accidentally depending entirely on
 * the audio path's backpressure). Now that ce_audio.c's pcm_submit() is
 * a fast, non-blocking ring-buffer copy, that coupling is gone entirely,
 * so this file paces frames itself:
 *   - Audio device open (the common case): ring-buffer-backed pacing
 *     (see CE_FRAME_PACING_HIGH_MS above) - the audio thread
 *     (CeAudioThreadProc, ce_audio.c) drains the ring at exactly the
 *     configured output rate = real time, so holding here until the
 *     backlog drops back under the high-water mark locks emulation
 *     speed to actual playback rather than to this device's own,
 *     independently-drifting Sleep()/GetTickCount() clock.
 *   - No audio device open (waveOutOpen failed): falls back to the
 *     previous plain GetTickCount() limiter, Sleep()ing out whatever's
 *     left of framelen after this frame's own work.
 * The slow-frame log below is kept as an ongoing sanity check (should
 * stay near-silent outside of real menu/dialog pauses) - CeLog() opens/
 * closes the log file on every call, so it's still threshold-gated
 * rather than unconditional. */
void doevents(void)
{
	MSG msg;
	static DWORD s_lastTick = 0;
	DWORD now = GetTickCount();
	DWORD targetMs;
#ifdef CE_PERF_DIAG
	DWORD stageStart;
#endif

	/* Always-on unified cross-core "perf:" summary line (every CE port
	 * emits this exact shape for side-by-side comparison). gnuboy has no
	 * libretro retro_run(); its equivalent per-frame core-emulation cost
	 * is one emu_run() loop-body iteration - CPU + LCD scanline render +
	 * APU mix + vid_end()'s BlitToScreen() + rtc/pcm_submit glue -
	 * measured as the wall time between consecutive doevents() entries
	 * (the frame-rate-pacing Sleep() below is taken *after* this delta,
	 * so it is excluded). "blit avg/max" is the BlitToScreen()
	 * sub-component's own last-60 snapshot (ce_video.c), i.e. already
	 * part of the retro_run figure, same as sister PopNES. Cheap
	 * GetTickCount() math; runs regardless of CE_PERF_DIAG, whose
	 * detailed breakdown line (CePerfMaybeLogSummary) is unchanged. */
	static DWORD    s_uPerfWorkStart  = 0;
	static unsigned s_uPerfRunAccumMs = 0;
	static unsigned s_uPerfRunMaxMs   = 0;
	static unsigned s_uPerfFrames     = 0;

	if (s_lastTick != 0)
	{
		DWORD gap = now - s_lastTick;
		if (gap > 40) /* ~2.5x a normal ~16.7ms frame */
		{
#ifdef CE_PERF_DIAG
			stageStart = GetTickCount();
#endif
			CeLog("doevents: slow frame gap=%lums framelen=%d",
			      (unsigned long)gap, rc_getint("framelen"));
#ifdef CE_PERF_DIAG
			s_perfSlowLogMs += (unsigned)(GetTickCount() - stageStart);
			s_perfSlowLogCount++;
#endif
		}

#ifdef CE_PERF_DIAG
		stageStart = GetTickCount();
#endif
		if (CeAudioIsActive())
		{
			/* Ring-buffer-backed pacing (see CE_FRAME_PACING_HIGH_MS
			 * above). Capped at 120 iterations as a safety valve, same
			 * as PopSG - a healthy ring drains in a few ms and never
			 * gets close; this only actually loops when real playback
			 * has fallen behind wall-clock time. */
			unsigned guard;
			for (guard = 0; guard < 120; guard++)
			{
				if (CeAudioGetBufferedMs() <= CE_FRAME_PACING_HIGH_MS)
					break;
				Sleep(1);
			}
		}
		else
		{
			targetMs = (DWORD)(rc_getint("framelen") / 1000);
			if (gap < targetMs)
				Sleep(targetMs - gap);
		}
#ifdef CE_PERF_DIAG
		s_perfSleepMs += (unsigned)(GetTickCount() - stageStart);
#endif
	}
	s_lastTick = GetTickCount();

#ifdef CE_PERF_DIAG
	stageStart = GetTickCount();
#endif
	CeResetIdleTimer();
#ifdef CE_PERF_DIAG
	s_perfIdleTimerMs += (unsigned)(GetTickCount() - stageStart);
	stageStart = GetTickCount();
#endif

	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
#ifdef CE_PERF_DIAG
	s_perfMsgPumpMs += (unsigned)(GetTickCount() - stageStart);
	stageStart = GetTickCount();
#endif

	CeInputPoll();
#ifdef CE_PERF_DIAG
	s_perfInputPollMs += (unsigned)(GetTickCount() - stageStart);
#endif

#ifdef CE_PERF_DIAG
	CePerfMaybeLogSummary();
#endif

	if (s_uPerfWorkStart != 0)
	{
		unsigned frameWork = (unsigned)(now - s_uPerfWorkStart);

		/* Skip the frame that straddles a menu/dialog pause (or any
		 * other multi-hundred-ms stall) - same noise class the
		 * slow-frame log above just notes and moves on. */
		if (frameWork < 1500)
		{
			s_uPerfRunAccumMs += frameWork;
			if (frameWork > s_uPerfRunMaxMs)
				s_uPerfRunMaxMs = frameWork;
			if (++s_uPerfFrames >= 60)
			{
				unsigned blitAvg = 0, blitMax = 0;
				CeVideoGetBlitPerf(&blitAvg, &blitMax);
				CeLog("perf: retro_run avg=%ums max=%ums blit avg=%ums max=%ums",
				      s_uPerfRunAccumMs / s_uPerfFrames, s_uPerfRunMaxMs,
				      blitAvg, blitMax);
				s_uPerfRunAccumMs = 0;
				s_uPerfRunMaxMs   = 0;
				s_uPerfFrames     = 0;
			}
		}
	}

	if (g_menuRequested)
	{
		g_menuRequested = 0;
		emu_pause(1);
		s_uPerfWorkStart = 0; /* next emu_run() resume: skip the straddling frame */
		return;
	}

	s_uPerfWorkStart = GetTickCount();
}

/* ------------------------------------------------------------------ */
/* WinMain                                                              */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	WNDCLASSW wc;
	HANDLE hMutex;
	HWND hTaskBar;

	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nCmdShow;

	g_hInstance = hInstance;

	hMutex = CreateMutexW(NULL, TRUE, CE_APP_MUTEX_NAME);
	if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS)
	{
		HWND existing = FindWindowW(CE_APP_WND_CLASS, NULL);
		if (existing)
		{
			ShowWindow(existing, SW_SHOW);
			SetForegroundWindow(existing);
		}
		ExitProcess(0);
	}

	memset(&wc, 0, sizeof wc);
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInstance;
	wc.lpszClassName = CE_APP_WND_CLASS;
	wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN));

	if (!RegisterClassW(&wc))
	{
		MessageBoxW(NULL, L"RegisterClassW failed", CE_APP_TITLE, MB_OK);
		ExitProcess(1);
	}

	/* WS_POPUP (not just WS_VISIBLE): a plain overlapped window doesn't
	 * reliably reclaim the taskbar's screen space on this device once
	 * it's hidden below. */
	g_hwnd = CreateWindowW(CE_APP_WND_CLASS, CE_APP_TITLE, WS_VISIBLE | WS_POPUP,
	                        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
	                        NULL, NULL, hInstance, NULL);
	if (!g_hwnd)
	{
		MessageBoxW(NULL, L"CreateWindowW failed", CE_APP_TITLE, MB_OK);
		ExitProcess(1);
	}

	hTaskBar = CeFindTaskBar();
	if (hTaskBar)
		ShowWindow(hTaskBar, SW_HIDE);

	/* Re-assert visibility/layout after hiding the taskbar - without
	 * this the window doesn't re-layout to cover the space the taskbar
	 * just vacated. */
	ShowWindow(g_hwnd, SW_SHOWNORMAL);
	UpdateWindow(g_hwnd);

	CeConfigLoad();

	/* Registers gnuboy's own rcvar-backed settings - must happen before
	 * anything below touches them (sys_initpath()'s rc_setvar("savedir",
	 * ...), CeVideoInit()'s rc_getmem("scale")/rc_getmem("colorfilter")). */
	rc_exportvars(loader_exports);
	rc_exportvars(lcd_exports);
	rc_exportvars(rtc_exports);
	rc_exportvars(sound_exports);
	rc_exportvars(vid_exports);
	rc_exportvars(joy_exports);
	rc_exportvars(pcm_exports);
	rc_exportvars(emu_exports);

	sys_initpath(NULL); /* WinMain has no real argv[0] - see ce_sys.c, ignores its argument */

	CeVideoInit();
	CeAudioInit();
	CeInputInit();
	CeFileOpenInit();
	CeLangInit(); /* loads the persisted Japanese/English UI flag - see ce_lang.h */

	/* Opened once, independent of any particular ROM - lcd.c's fb.w/
	 * fb.h (set by vid_init()) need to exist before the first
	 * emu_reset()->lcd_reset() runs, so this must come before the first
	 * LoadRom() call below. */
	vid_preinit();
	vid_init();
	pcm_init();

	/* Main menu shown first, before any ROM is loaded (hardware-reported
	 * request: the app used to jump straight into the ROM picker on
	 * launch) - EnableWindow() in MainMenuDlgProc's WM_INITDIALOG already
	 * greys out Save/Load State while g_romLoaded is still 0, so Open ROM
	 * and Exit are the only meaningful choices at this point. Pressing
	 * the physical Back key with nothing loaded just reopens the menu
	 * (there is nothing to "resume" to yet) instead of exiting. */
	for (;;)
	{
		if (!ShowMainMenu())
			break; /* Exit chosen */

		if (g_romLoaded)
		{
			emu_pause(0);
			/* Give the ring (just reset to empty by ShowMainMenu()'s own
			 * pcm_pause(1)) a small head start before the CPU/PPU work
			 * resumes - see ce_audio.h's CeAudioPrimeSilence() comment
			 * and the dev notes: a hardware log caught the ring
			 * genuinely starving (one real CeAudioFlushUnderrun(), not
			 * a false one) during the first frame or two right after a
			 * ROM loads / a dialog closes, not during steady gameplay. */
			CeAudioPrimeSilence(120);
			emu_run(); /* returns when doevents() calls emu_pause(1) for a menu request; die() exits the process directly and never returns here */
		}
	}

	CeShutdown(0);
	return 0;
}
