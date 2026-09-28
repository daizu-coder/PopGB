#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 daizu-coder
r"""
shinonome-font (16dot) の .bit 疑似BDFソースを、PopGB 向けの
Cヘッダ(ビットマップフォントテーブル)へ変換するビルド前処理スクリプト。

入力: shinonome-font-main/16/{kanjic,latin1,hankaku}/font_src*.bit
出力: sys/ce/ce_shinonome16.h (Cヘッダ、gitにコミットして使う。CE側では
      このスクリプトもPythonも一切実行しない)

.bit ファイルの実体は東雲プロジェクト独自の中間ソース形式であり、
BDFの BITMAP セクションのような16進ダンプではなく、各行が '.'(消灯)/
'@'(点灯) のテキストアートになっている。東雲純正ツール tools/bit2bdf.in
の tr 変換 (`tr/pPOo@:;,\/./#####       /`) に倣い、
  点灯: p P O o @
  消灯: : ; , \ / .
として解釈する(このリポジトリの実データでは '.' と '@' のみ使用)。

kanjic の ENCODING は Unicode でも Shift_JIS でもない「JISコード」
(区点コードの各バイトに0x20を加算した値 = ISO-2022-JPのGL領域そのもの)
なので、`ESC $ B <hi><lo> ESC ( B` を組み立てて iso2022_jp でデコードし
Unicodeへ変換する。
"""
import re
import sys
from pathlib import Path

ON_CHARS = set("pPOo@")
OFF_CHARS = set(":;,\\/.")

GLYPH_RE = re.compile(
    r"^STARTCHAR\s+\S+\s*\n"
    r"ENCODING\s+(\d+)\s*\n"
    r"SWIDTH[^\n]*\n"
    r"DWIDTH[^\n]*\n"
    r"BBX\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s*\n"
    r"BITMAP\s*\n"
    r"(.*?)"
    r"ENDCHAR",
    re.MULTILINE | re.DOTALL,
)


def line_to_bits(line, width):
    """1グリフ行('.'/'@'のテキストアート)をMSBファーストの整数に変換する。"""
    bits = 0
    for i, ch in enumerate(line[:width]):
        on = ch in ON_CHARS or (ch not in OFF_CHARS and (ord(ch) & 1))
        if on:
            bits |= 1 << (width - 1 - i)
    return bits


def parse_bit(path):
    """
    .bit ファイルをパースし、{encoding(int): (width, height, [row_bits...])}
    の辞書を返す。
    """
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    glyphs = {}
    for m in GLYPH_RE.finditer(text):
        encoding = int(m.group(1))
        w, h = int(m.group(2)), int(m.group(3))
        body = m.group(6)
        lines = [ln for ln in body.splitlines() if ln.strip() != ""]
        if len(lines) != h:
            print(
                f"warning: {path} encoding={encoding}: expected {h} rows, got {len(lines)}",
                file=sys.stderr,
            )
        rows = [line_to_bits(ln, w) for ln in lines[:h]]
        while len(rows) < h:
            rows.append(0)
        glyphs[encoding] = (w, h, rows)
    return glyphs


def jis_code_to_unicode(jis_code):
    """区点由来のJISコード(2バイト、各0x21-0x7E)をUnicode1文字に変換する。"""
    hi = (jis_code >> 8) & 0xFF
    lo = jis_code & 0xFF
    raw = bytes([0x1B, 0x24, 0x42, hi, lo, 0x1B, 0x28, 0x42])
    try:
        s = raw.decode("iso2022_jp")
    except UnicodeDecodeError:
        return None
    # ESC(Bの終端シーケンスは decode 後に消えているはずだが、念のため
    # 制御文字を除去して1文字だけ取り出す
    s = s.replace("\x1b", "")
    if len(s) != 1:
        return None
    return s


def main():
    # デフォルトは「このスクリプトの3つ上(sys/ce/tools -> sys/ce -> sys ->
    # gnuboy-master のさらに親)に shinonome-font-main/16 が置かれている」
    # という、このワークツリーでのレイアウトを前提にする。他の場所に置く
    # 場合は引数で明示すること。
    script_dir = Path(__file__).resolve().parent
    default_root = script_dir.parent.parent.parent.parent / "shinonome-font-main" / "16"
    default_out = script_dir.parent / "ce_shinonome16.h"

    root = Path(sys.argv[1]) if len(sys.argv) > 1 else default_root
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else default_out

    kanjic = parse_bit(root / "kanjic" / "font_src.bit")
    latin1 = parse_bit(root / "latin1" / "font_src.bit")
    hankaku_diff = parse_bit(root / "hankaku" / "font_src_diff.bit")

    # --- 全角(漢字・かな・記号) : JISコード -> Unicode ---
    kanji_table = {}  # codepoint -> rows(16 x 16bit)
    unmapped = []
    for jis_code, (w, h, rows) in kanjic.items():
        if w != 16 or h != 16:
            continue
        ch = jis_code_to_unicode(jis_code)
        if ch is None:
            unmapped.append(jis_code)
            continue
        cp = ord(ch)
        if cp in kanji_table:
            # 東雲側で同一Unicode文字に複数のJIS異体字が当たるケースは
            # 最初に見つかったものを優先する(順序はENCODING昇順)
            continue
        kanji_table[cp] = (rows, ch)

    if unmapped:
        print(
            f"note: kanjic中 {len(unmapped)} 字はiso2022_jpで変換できず除外 "
            f"(例: {unmapped[:5]})",
            file=sys.stderr,
        )

    # --- 半角ASCII可視域 (latin1由来、0x20-0x7E) ---
    ascii_lo, ascii_hi = 0x20, 0x7E
    ascii_rows = {}
    for cp in range(ascii_lo, ascii_hi + 1):
        if cp in latin1:
            w, h, rows = latin1[cp]
            ascii_rows[cp] = rows
        else:
            print(f"warning: latin1にASCII 0x{cp:02X}が無い", file=sys.stderr)
            ascii_rows[cp] = [0] * 16

    # --- 半角カナ (hankaku diff由来、JIS X0201 0xA1-0xDF -> U+FF61-U+FF9F) ---
    kana_lo_jis, kana_hi_jis = 0xA1, 0xDF
    kana_rows = {}
    for jis in range(kana_lo_jis, kana_hi_jis + 1):
        cp = 0xFF61 + (jis - kana_lo_jis)
        if jis in hankaku_diff:
            _, _, rows = hankaku_diff[jis]
            kana_rows[cp] = rows
        else:
            print(f"warning: hankaku diffに半角カナ 0x{jis:02X}が無い", file=sys.stderr)
            kana_rows[cp] = [0] * 16

    write_header(out_path, kanji_table, ascii_lo, ascii_hi, ascii_rows, kana_rows)
    print(
        f"generated {out_path}: kanji={len(kanji_table)} ascii={len(ascii_rows)} "
        f"halfkana={len(kana_rows)}"
    )


def write_header(out_path, kanji_table, ascii_lo, ascii_hi, ascii_rows, kana_rows):
    lines = []
    lines.append("/* AUTO-GENERATED by sys/ce/tools/shinonome2c.py - DO NOT EDIT BY HAND. */")
    lines.append("/*")
    lines.append(" * Glyph data derived from the Shinonome bitmap font project")
    lines.append(" * (16dot: kanjic/latin1/hankaku). See sys/ce/THIRDPARTY_LICENSES.txt")
    lines.append(" * for the license text and author credits.")
    lines.append(" */")
    lines.append("#ifndef CE_SHINONOME16_H")
    lines.append("#define CE_SHINONOME16_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("typedef struct { uint16_t codepoint; uint16_t rows[16]; } CeKanjiGlyph16;")
    lines.append("")

    lines.append(f"#define CE_ASCII16_FIRST 0x{ascii_lo:02X}")
    lines.append(f"#define CE_ASCII16_LAST  0x{ascii_hi:02X}")
    lines.append("static const uint8_t s_ceAsciiGlyph16[CE_ASCII16_LAST - CE_ASCII16_FIRST + 1][16] = {")
    for cp in range(ascii_lo, ascii_hi + 1):
        rows = ascii_rows[cp]
        row_str = ", ".join(f"0x{r:02X}" for r in rows)
        printable = chr(cp) if 0x21 <= cp <= 0x7E else " "
        lines.append(f"    {{ {row_str} }}, /* 0x{cp:02X} '{printable}' */")
    lines.append("};")
    lines.append("")

    lines.append("#define CE_HALFKANA16_FIRST 0xFF61")
    lines.append("#define CE_HALFKANA16_LAST  0xFF9F")
    lines.append(
        "static const uint8_t s_ceHalfKanaGlyph16[CE_HALFKANA16_LAST - CE_HALFKANA16_FIRST + 1][16] = {"
    )
    for cp in range(0xFF61, 0xFF9F + 1):
        rows = kana_rows[cp]
        row_str = ", ".join(f"0x{r:02X}" for r in rows)
        try:
            printable = chr(cp)
        except ValueError:
            printable = " "
        lines.append(f"    {{ {row_str} }}, /* U+{cp:04X} '{printable}' */")
    lines.append("};")
    lines.append("")

    sorted_kanji = sorted(kanji_table.items())
    lines.append(f"#define CE_KANJI16_COUNT {len(sorted_kanji)}")
    lines.append("static const CeKanjiGlyph16 s_ceKanjiGlyph16[CE_KANJI16_COUNT] = {")
    for cp, (rows, ch) in sorted_kanji:
        row_str = ", ".join(f"0x{r:04X}" for r in rows)
        safe_ch = ch if ch.isprintable() else " "
        lines.append(f"    {{ 0x{cp:04X}, {{ {row_str} }} }}, /* {safe_ch} */")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* CE_SHINONOME16_H */")

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
