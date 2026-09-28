/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

#include "ce_lang.h"
#include "ce_config.h"

static int g_japanese = 0;

void CeLangInit(void)
{
    /* Default Japanese (1) - user request. Only matters for a fresh
     * config file (no UILanguageJapanese key yet, e.g. first launch or
     * a wiped PopGB.cfg); anyone who has already picked a language
     * via Video Config keeps that persisted choice either way. */
    g_japanese = CeConfigGetInt("UILanguageJapanese", 1);
}

int CeLangIsJapanese(void)
{
    return g_japanese;
}

void CeLangSetJapanese(int japanese)
{
    g_japanese = japanese ? 1 : 0;
    CeConfigSetInt("UILanguageJapanese", g_japanese);
}
