/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * Physical keyboard -> Game Boy pad mapping (see ce_input.h) plus the
 * native Input Config remap dialog (IDD_INPUTCONFIG in ce_res.rc).
 * Adapted from the sister ports' shared ce_input.c - same remap-by-
 * press design (BeginWaitForKey/CaptureKeyAsBinding, WM_KEYDOWN-only
 * capture, subclassed buttons for a guaranteed physical-key focus loop -
 * see that file's own comments for the hardware-verified reasoning
 * behind each of these), reduced from a 12-button SNES-style pad down
 * to the Game Boy's 8 (hw.h's PAD_UP/DOWN/LEFT/RIGHT/A/B/SELECT/START),
 * and calling hw.h's pad_set() directly each poll instead of feeding a
 * libretro retro_input_state_t table (gnuboy is not a libretro core -
 * see the port's dev notes). Plus four diagonal D-pad combo rows (ported from the
 * sister PopNES / PopSG projects' own Input Config,
 * user request) - see CeInputMapEntry.idB and CeInputPoll() below.
 *
 * Default key assignments per the previous minimal CE port's keymap.c
 * (confirmed on real hardware there): arrow keys = D-pad, Enter = A,
 * Backspace = B, Tab = Select, 'M' = Start. The four diagonal combo rows
 * have no natural default physical key and start unbound until the user
 * remaps one.
 */
#include "ce_input.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_resource.h"

#include "hw.h"

#include <stdio.h>

#ifndef VK_OEM_MINUS
#define VK_OEM_MINUS 0xBD
#endif

typedef struct
{
    byte            padBit;     /* PAD_* in hw.h */
    byte            idB;        /* second PAD_* to press at the same time
                                  * as padBit, for the diagonal combo rows
                                  * below - 0 for the ordinary single-
                                  * button rows (no PAD_* bit is ever 0,
                                  * see hw.h, so 0 doubles as "none",
                                  * matching VkToLabel's vk==0 "unbound"
                                  * convention below). */
    int             ctrlId;     /* IDC_IC_BTN_* in the config dialog */
    const char     *cfgKey;     /* ce_config.c key name */
    int             defaultVk;
    int             vk;         /* current mapping - live, mutated by the config dialog */
    int             suppressed; /* see CeInputSuppressUntilReleased() below */
    int             lastRawDown;/* previous GetAsyncKeyState() edge, for debug logging only */
} CeInputMapEntry;

static CeInputMapEntry s_map[] = {
    { PAD_UP,     0, IDC_IC_BTN_UP,     "InputUp",     VK_UP,     VK_UP,     0, 0 },
    { PAD_DOWN,   0, IDC_IC_BTN_DOWN,   "InputDown",   VK_DOWN,   VK_DOWN,   0, 0 },
    { PAD_LEFT,   0, IDC_IC_BTN_LEFT,   "InputLeft",   VK_LEFT,   VK_LEFT,   0, 0 },
    { PAD_RIGHT,  0, IDC_IC_BTN_RIGHT,  "InputRight",  VK_RIGHT,  VK_RIGHT,  0, 0 },
    { PAD_A,      0, IDC_IC_BTN_A,      "InputA",      VK_RETURN, VK_RETURN, 0, 0 },
    { PAD_B,      0, IDC_IC_BTN_B,      "InputB",      VK_BACK,   VK_BACK,   0, 0 },
    { PAD_SELECT, 0, IDC_IC_BTN_SELECT, "InputSelect", VK_TAB,    VK_TAB,    0, 0 },
    { PAD_START,  0, IDC_IC_BTN_START,  "InputStart",  'M',       'M',       0, 0 },
    /* Diagonal combos (ported from the sister PopNES / PopSG
     * projects' own Input Config, user request): the one
     * physical key bound here drives both directions at once. No natural
     * default physical key for these, so they start unbound (defaultVk/
     * vk = 0) until the user remaps one - see VkToLabel's vk==0 case and
     * CeInputPoll's vk!=0 guard below. */
    { PAD_UP,    PAD_RIGHT, IDC_IC_BTN_UPRIGHT,   "InputUpRight",   0, 0, 0, 0 },
    { PAD_RIGHT, PAD_DOWN,  IDC_IC_BTN_RIGHTDOWN, "InputRightDown", 0, 0, 0, 0 },
    { PAD_DOWN,  PAD_LEFT,  IDC_IC_BTN_DOWNLEFT,  "InputDownLeft",  0, 0, 0, 0 },
    { PAD_LEFT,  PAD_UP,    IDC_IC_BTN_LEFTUP,    "InputLeftUp",    0, 0, 0, 0 },
};
#define CE_INPUT_COUNT (sizeof(s_map) / sizeof(s_map[0]))

/* ------------------------------------------------------------------ */
/* Poll hook - reflects straight into hw.pad via pad_set()             */
/* ------------------------------------------------------------------ */

void CeInputInit(void)
{
    unsigned i;

    for (i = 0; i < CE_INPUT_COUNT; i++)
        s_map[i].vk = CeConfigGetInt(s_map[i].cfgKey, s_map[i].defaultVk);

    CeLog("CeInputInit: loaded key mapping from config file");
}

void CeInputPoll(void)
{
    unsigned i;
    byte down = 0; /* accumulated PAD_* bits for this poll, flushed below */

    for (i = 0; i < CE_INPUT_COUNT; i++)
    {
        /* vk==0 is a diagonal combo row not yet remapped to a physical
         * key (see s_map's comment) - skip GetAsyncKeyState(0) rather
         * than ask the driver about a meaningless VK code. */
        int rawDown = (s_map[i].vk != 0 && (GetAsyncKeyState(s_map[i].vk) & 0x8000)) ? 1 : 0;
        int pressed = rawDown;

        /* Edge-triggered only (never every frame) - this is the raw
         * GetAsyncKeyState() reading, before the suppression override
         * below, so a real-hardware log shows exactly when the driver
         * itself reports each key going down/up regardless of what
         * gameplay ends up seeing. Meant to confirm or rule out the
         * "stuck key after a dialog closes" hypothesis from the port's dev notes
         * session 7/7-continued. */
        if (rawDown != s_map[i].lastRawDown)
        {
            CeLog("CeInputPoll: vk=0x%02X pad=%u rawDown %d->%d suppressed=%d",
                  (unsigned)s_map[i].vk, (unsigned)s_map[i].padBit,
                  s_map[i].lastRawDown, rawDown, s_map[i].suppressed);
            s_map[i].lastRawDown = rawDown;
        }

        if (s_map[i].suppressed)
        {
            /* Only lift the suppression once GetAsyncKeyState actually
             * reports this key up - see CeInputSuppressUntilReleased(). */
            if (!rawDown)
            {
                s_map[i].suppressed = 0;
                CeLog("CeInputPoll: vk=0x%02X pad=%u suppression lifted (key observed released)",
                      (unsigned)s_map[i].vk, (unsigned)s_map[i].padBit);
            }
            pressed = 0;
        }

        /* OR'd into a single accumulator rather than calling pad_set()
         * per entry directly (the old, single-button-per-row design):
         * a diagonal combo row and one of the four plain direction rows
         * can both claim the same PAD_* bit in the same poll (e.g. "Up"
         * and "Up R" both set PAD_UP), and calling pad_set() once per
         * entry would let whichever row happens to be processed last in
         * s_map win, silently dropping the other's contribution. */
        if (pressed)
        {
            down |= s_map[i].padBit;
            down |= s_map[i].idB;
        }
    }

    pad_set(PAD_UP,     (down & PAD_UP)     != 0);
    pad_set(PAD_DOWN,   (down & PAD_DOWN)   != 0);
    pad_set(PAD_LEFT,   (down & PAD_LEFT)   != 0);
    pad_set(PAD_RIGHT,  (down & PAD_RIGHT)  != 0);
    pad_set(PAD_A,      (down & PAD_A)      != 0);
    pad_set(PAD_B,      (down & PAD_B)      != 0);
    pad_set(PAD_SELECT, (down & PAD_SELECT) != 0);
    pad_set(PAD_START,  (down & PAD_START)  != 0);
}

/* The Config dialogs' own navigation reuses Enter/arrow keys - the same
 * VK codes PAD_A/the D-pad default to - to confirm/move focus
 * (SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc all EndDialog() straight
 * out of their WM_KEYDOWN handler). On this device's keyboard driver
 * that can leave GetAsyncKeyState() still reporting the key held after
 * the dialog window that would have received its WM_KEYUP is already
 * destroyed, so gameplay would otherwise resume with e.g. A stuck down -
 * read back by the user as a rapid repeating confirm-sound "beep" plus
 * the slowdown from whatever the game does every frame while A is held.
 * Called once, right when a menu/dialog closes back to gameplay (see
 * ce_main.c). */
void CeInputSuppressUntilReleased(void)
{
    unsigned i;

    CeLog("CeInputSuppressUntilReleased: called");
    for (i = 0; i < CE_INPUT_COUNT; i++)
    {
        int rawDown = (s_map[i].vk != 0 && (GetAsyncKeyState(s_map[i].vk) & 0x8000)) ? 1 : 0;

        /* Logged unconditionally for every key that reads as down right
         * now - this is the exact moment the port's dev notes' session 7/
         * 7-continued suspected a dialog-dismissing key (typically
         * VK_RETURN/PAD_A) could still read as held. If real hardware
         * never logs this line, the "stuck key" theory is ruled out and
         * the remaining suspect is the focus handling logged below in
         * ShowMainMenu(). */
        if (rawDown)
            CeLog("CeInputSuppressUntilReleased: vk=0x%02X pad=%u reads DOWN right now - will suppress until released",
                  (unsigned)s_map[i].vk, (unsigned)s_map[i].padBit);

        s_map[i].suppressed = 1;
        s_map[i].lastRawDown = rawDown;
    }
}

/* ------------------------------------------------------------------ */
/* Input Config dialog (remap-by-press)                                */
/* ------------------------------------------------------------------ */

typedef struct { int vk; const wchar_t *label; } VkName;

static const VkName kVkNames[] = {
    { VK_UP,     L"Up" },    { VK_DOWN,   L"Down" },
    { VK_LEFT,   L"Left" },  { VK_RIGHT,  L"Right" },
    { VK_RETURN, L"Enter" }, { VK_BACK,   L"Backspace" },
    { VK_TAB,    L"Tab" },   { VK_SPACE,  L"Space" },
    { VK_ESCAPE, L"Esc" },   { VK_OEM_MINUS, L"-" },
    /* This device's own dedicated hardware buttons - see Sample
     * Emulater's ce_input.c for how these VK codes were identified. */
    { 0xDC, L"Voice" },      { 0x14, L"Function" },
    { 0x21, L"Forward" },    { 0x22, L"Previous" },
    { '0',L"0" },{ '1',L"1" },{ '2',L"2" },{ '3',L"3" },{ '4',L"4" },
    { '5',L"5" },{ '6',L"6" },{ '7',L"7" },{ '8',L"8" },{ '9',L"9" },
    { 'A',L"A" },{ 'B',L"B" },{ 'C',L"C" },{ 'D',L"D" },{ 'E',L"E" },
    { 'F',L"F" },{ 'G',L"G" },{ 'H',L"H" },{ 'I',L"I" },{ 'J',L"J" },
    { 'K',L"K" },{ 'L',L"L" },{ 'M',L"M" },{ 'N',L"N" },{ 'O',L"O" },
    { 'P',L"P" },{ 'Q',L"Q" },{ 'R',L"R" },{ 'S',L"S" },{ 'T',L"T" },
    { 'U',L"U" },{ 'V',L"V" },{ 'W',L"W" },{ 'X',L"X" },{ 'Y',L"Y" },
    { 'Z',L"Z" },
};
#define CE_VKNAME_COUNT (sizeof(kVkNames) / sizeof(kVkNames[0]))

static const wchar_t *VkToLabel(int vk)
{
    static wchar_t fallback[16];
    unsigned i;
    if (vk == 0)
        return L"None"; /* diagonal combo row, not yet remapped */
    for (i = 0; i < CE_VKNAME_COUNT; i++)
        if (kVkNames[i].vk == vk)
            return kVkNames[i].label;
    _snwprintf(fallback, 16, L"VK_%02X", vk);
    return fallback;
}

static int CtrlIdToIndex(int ctrlId)
{
    unsigned i;
    for (i = 0; i < CE_INPUT_COUNT; i++)
        if (s_map[i].ctrlId == ctrlId)
            return (int)i;
    return -1;
}

/* -1 when no remap is pending, otherwise the s_map[] index waiting for
 * a new key. */
static int s_waitingIndex = -1;

static void BeginWaitForKey(HWND hDlg, int index)
{
    s_waitingIndex = index;
    /* Shortened from "Press a key..." (13 chars, then "Press a key" -
     * 11 chars) - real-hardware confirmed both still got clipped
     * ("ress a key.", then a further-reported clip) inside the 52px-
     * wide remap button once BS_OWNERDRAW center-aligned drawing
     * (ce_bmpfont.c) replaced GDI's own auto-shrinking TrueType label. */
    SetWindowTextW(GetDlgItem(hDlg, s_map[index].ctrlId), L"Press key");
}

static void EndWaitForKey(HWND hDlg)
{
    (void)hDlg;
    s_waitingIndex = -1;
}

static void CancelWaitForKey(HWND hDlg)
{
    if (s_waitingIndex < 0)
        return;
    SetWindowTextW(GetDlgItem(hDlg, s_map[s_waitingIndex].ctrlId), VkToLabel(s_map[s_waitingIndex].vk));
    EndWaitForKey(hDlg);
}

static void CaptureKeyAsBinding(HWND hDlg, int index, int vk)
{
    s_map[index].vk = vk;
    SetWindowTextW(GetDlgItem(hDlg, s_map[index].ctrlId), VkToLabel(vk));
    EndWaitForKey(hDlg);
}

static void CeInputSaveConfig(void)
{
    unsigned i;

    for (i = 0; i < CE_INPUT_COUNT; i++)
        CeConfigSetInt(s_map[i].cfgKey, s_map[i].vk);

    CeConfigSave();
    CeLog("CeInputSaveConfig: saved key mapping");
}

static int IsRoutineDialogChatter(UINT msg)
{
    switch (msg)
    {
    case WM_PAINT:          case WM_NCPAINT:
    case WM_ERASEBKGND:     case WM_SETCURSOR:
    case WM_NCHITTEST:      case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
        return 1;
    default:
        return 0;
    }
}

/* All twelve row/OK labels (translated + fixed) that WM_PAINT repaints
 * with the Shinonome bitmap font - see CeBmpFontPaintLabel() in
 * ce_bmpfont.c. The remap PUSHBUTTONs (IDC_IC_BTN_*, IDOK) are not in
 * this list: they're BS_OWNERDRAW (ce_res.rc) and redraw themselves via
 * WM_DRAWITEM instead. */
static const int kInputLabelIds[] = {
    IDC_IC_LBL_UP,     IDC_IC_LBL_DOWN,   IDC_IC_LBL_LEFT,  IDC_IC_LBL_RIGHT,
    IDC_IC_LBL_SELECT, IDC_IC_LBL_START,  IDC_IC_LBL_A,     IDC_IC_LBL_B,
    IDC_IC_LBL_UPRIGHT, IDC_IC_LBL_RIGHTDOWN, IDC_IC_LBL_DOWNLEFT, IDC_IC_LBL_LEFTUP,
};
#define CE_INPUT_LABEL_COUNT (sizeof(kInputLabelIds) / sizeof(kInputLabelIds[0]))

/* Translates the row captions (Up/Down/Left/Right/Select/Start), A/B, the
 * four diagonal combo labels, and OK. The Japanese branch uses fullwidth
 * Ａ/Ｂ (U+FF21/FF22) and 右上/右下/左下/左上 for the diagonals (user
 * request); English keeps the RC defaults ("A"/"B"/"Up R"/... ). One-shot
 * at WM_INITDIALOG. Every LTEXT label (STATIC -
 * no ownerdraw style exists for that control class) is hidden here and
 * repainted by WM_PAINT instead; GetWindowTextW() still sees the text
 * set above even while the control is hidden. */
static void ApplyInputConfigLanguage(HWND hDlg)
{
    unsigned i;

    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_IC_LBL_UP,     L"\x4e0a");                   /* 上 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_DOWN,   L"\x4e0b");                   /* 下 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_LEFT,   L"\x5de6");                   /* 左 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_RIGHT,  L"\x53f3");                   /* 右 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_SELECT, L"\x30bb\x30ec\x30af\x30c8"); /* セレクト */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_START,  L"\x30b9\x30bf\x30fc\x30c8"); /* スタート */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_A,      L"\xff21");                   /* Ａ */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_B,      L"\xff22");                   /* Ｂ */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_UPRIGHT,   L"\x53f3\x4e0a");          /* 右上 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_RIGHTDOWN, L"\x53f3\x4e0b");          /* 右下 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_DOWNLEFT,  L"\x5de6\x4e0b");          /* 左下 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_LEFTUP,    L"\x5de6\x4e0a");          /* 左上 */
        SetDlgItemTextW(hDlg, IDOK,     L"\x6c7a\x5b9a");                      /* 決定 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_IC_LBL_UP,     L"Up");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_DOWN,   L"Down");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_LEFT,   L"Left");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_RIGHT,  L"Right");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_SELECT, L"Select");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_START,  L"Start");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_A,      L"A");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_B,      L"B");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_UPRIGHT,   L"Up R");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_RIGHTDOWN, L"R Down");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_DOWNLEFT,  L"Down L");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_LEFTUP,    L"L Up");
        SetDlgItemTextW(hDlg, IDOK,     L"OK");
    }

    for (i = 0; i < CE_INPUT_LABEL_COUNT; i++)
        ShowWindow(GetDlgItem(hDlg, kInputLabelIds[i]), SW_HIDE);
}

#define WM_SETINPUTFOCUS (WM_APP + 204)

/* Physical-key focus chain: reading order top-to-bottom, left column
 * then right column then OK (Up Down Left Right Up-R R-Down Down-L
 * L-Up, A B Select Start, OK - matching ce_res.rc's IDD_INPUTCONFIG
 * eight-row-left/four-row-right layout: the four diagonal D-pad combo
 * rows stack at the bottom of the left column below Left/Right, per
 * user feedback, rather than spreading across both columns) - same
 * explicit-subclass-every-control technique as the sister ports' shared
 * kInputOrder (ce_input.c) and the sister PopNES / PopSG
 * projects' own kInputOrder, a single flat loop wrapped at the left
 * column's height rather than two independent up/down axes. */
static const int kInputOrder[13] = {
    IDC_IC_BTN_UP,      IDC_IC_BTN_DOWN,      IDC_IC_BTN_LEFT,    IDC_IC_BTN_RIGHT,
    IDC_IC_BTN_UPRIGHT, IDC_IC_BTN_RIGHTDOWN, IDC_IC_BTN_DOWNLEFT,IDC_IC_BTN_LEFTUP,
    IDC_IC_BTN_A,       IDC_IC_BTN_B,         IDC_IC_BTN_SELECT,  IDC_IC_BTN_START,
    IDOK,
};
#define CE_INPUT_ORDER_COUNT (sizeof(kInputOrder) / sizeof(kInputOrder[0]))

static int InputOrderIndex(int ctrlId)
{
    unsigned i;
    for (i = 0; i < CE_INPUT_ORDER_COUNT; i++)
        if (kInputOrder[i] == ctrlId)
            return (int)i;
    return -1;
}

static int InputNeighborUp(int ctrlId)
{
    int i = InputOrderIndex(ctrlId);
    if (i < 0)
        return ctrlId;
    return kInputOrder[(i + CE_INPUT_ORDER_COUNT - 1) % CE_INPUT_ORDER_COUNT];
}

static int InputNeighborDown(int ctrlId)
{
    int i = InputOrderIndex(ctrlId);
    if (i < 0)
        return ctrlId;
    return kInputOrder[(i + 1) % CE_INPUT_ORDER_COUNT];
}

static WNDPROC s_pInputOrigProc = NULL;

static LRESULT CALLBACK InputBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    HWND hDlg = GetParent(hWnd);
    int  id   = GetDlgCtrlID(hWnd);
    int  idx  = CtrlIdToIndex(id);

    switch (message)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    case WM_KEYDOWN:
        if (idx >= 0 && idx == s_waitingIndex)
        {
            CaptureKeyAsBinding(hDlg, idx, (int)wParam);
            return 0;
        }
        switch (wParam)
        {
        case VK_UP:
        case VK_LEFT:  SetFocus(GetDlgItem(hDlg, InputNeighborUp(id)));   return 0;
        case VK_DOWN:
        case VK_RIGHT: SetFocus(GetDlgItem(hDlg, InputNeighborDown(id))); return 0;

        case VK_RETURN:
        case VK_SPACE:
            if (id == IDOK)
            {
                SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            }
            else if (idx >= 0)
            {
                BeginWaitForKey(hDlg, idx);
            }
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
        break;
    }

    return CallWindowProc(s_pInputOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK InputConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        unsigned i;
        for (i = 0; i < CE_INPUT_COUNT; i++)
            SetWindowTextW(GetDlgItem(hDlg, s_map[i].ctrlId), VkToLabel(s_map[i].vk));
        ApplyInputConfigLanguage(hDlg);

        s_pInputOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        for (i = 0; i < CE_INPUT_COUNT; i++)
            SetWindowLongPtrW(GetDlgItem(hDlg, s_map[i].ctrlId), GWLP_WNDPROC, (LONG_PTR)InputBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC, (LONG_PTR)InputBtnCtrlProc);

        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_IC_BTN_UP));
        PostMessage(hDlg, WM_SETINPUTFOCUS, 0, 0);
        return FALSE;
    }

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        unsigned i;
        for (i = 0; i < CE_INPUT_LABEL_COUNT; i++)
            CeBmpFontPaintLabel(hdc, hDlg, kInputLabelIds[i]);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            SetFocus(GetDlgItem(hDlg, IDC_IC_BTN_UP));
            PostMessage(hDlg, WM_SETINPUTFOCUS, 0, 0);
        }
        break;

    case WM_SETINPUTFOCUS:
        SetFocus(GetDlgItem(hDlg, IDC_IC_BTN_UP));
        break;

    case WM_COMMAND:
    {
        int ctrlId = LOWORD(wParam);
        int idx = CtrlIdToIndex(ctrlId);

        if (idx >= 0 && HIWORD(wParam) == BN_CLICKED)
        {
            if (idx == s_waitingIndex)
            {
                CancelWaitForKey(hDlg);
                return TRUE;
            }
            BeginWaitForKey(hDlg, idx);
            return TRUE;
        }

        switch (ctrlId)
        {
        case IDOK:
        case IDCANCEL:
            EndWaitForKey(hDlg);
            CeInputSaveConfig();
            EndDialog(hDlg, ctrlId);
            return TRUE;
        }
        return FALSE;
    }

    default:
        if (s_waitingIndex >= 0 && !IsRoutineDialogChatter(msg))
            CeLog("InputConfigDlgProc: msg=0x%04X wParam=0x%08X while waiting for a key press",
                  (unsigned)msg, (unsigned)wParam);
        return FALSE;
    }
    return FALSE;
}

void CeShowInputConfigDialog(HWND owner)
{
    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), MAKEINTRESOURCEW(IDD_INPUTCONFIG),
               owner, InputConfigDlgProc);
}
