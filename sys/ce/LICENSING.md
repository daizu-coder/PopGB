# PopGB のライセンス

PopGB は、ゲームボーイ / ゲームボーイカラーのエミュレータ [gnuboy](https://github.com/rofl0r/gnuboy)(Laguna 氏、Gilgamesh 氏ほか)を、SHARP Brain PW-G5300(Windows CE)向けに移植した、**非公式**の改変版です。gnuboy の作者やメンテナーはこの移植に関わっていません。

ライセンスは2段になっています。

## 1. アプリ全体: GNU GPL バージョン2(上流と同じ条件)

アプリ全体(`AppMain.exe`、およびこのリポジトリで公開しているソース全体)は、上流の gnuboy と同じ条件の **GNU General Public License バージョン2(GPL-2.0)** で配布します。GPL バージョン2の本文はリポジトリ直下の [`COPYING`](../../COPYING) に、gnuboy の著作権表示は [`LICENSE`](LICENSE) の3節にあります。

「それ以降のバージョンの GPL」も選べるかどうかは、ここでは断定しません。上流のファイルの書き方がそろっていないためです。`main.c` と `sys/sdl/`・`sys/sdl2/` の一部は「バージョン2またはそれ以降」と書いていますが、ほかは「GNU GPL」とだけ書いてあるもの、「LGPL バージョン2」と書いてあるもの(ビルドに使わない `sys/thinlib/`)、ライセンスの表記が無いもの(`AppMain.exe` に入るコアのファイルの多く)があります。

条件は次のとおりです。

- ソース・バイナリとも再配布できます。GPL の範囲で商用利用もできます
- 配るときは、ライセンスの文と各ファイルの著作権表示を残すこと
- `AppMain.exe` を配布するときは、対応するソース(このリポジトリ)も入手できるようにすること
- 改変したものを配るときは、全体を GPL で配布すること
- 保証はありません

## 2. gnuboy 本体(上流のファイル)

リポジトリ直下のファイル(`lcd.c`、`cpu.c`、`sound.c`、`loader.c` など)は、上流の [rofl0r/gnuboy](https://github.com/rofl0r/gnuboy) のコミット `c367bb4` を元にしています。

`AppMain.exe` に入る gnuboy のファイルは、`Makefile.ce` の `OBJS` にあるものです(`lcd.c` `refresh.c` `lcdc.c` `palette.c` `cpu.c` `mem.c` `rtc.c` `hw.c` `sound.c` `fastmem.c` `loader.c` `save.c` `emu.c` `rcvars.c` と、そのヘッダ)。

PopGB が変えた上流のファイルは次のものだけです。どれも Windows CE 用のフロントエンドから使うためのもので、ライセンスは変わりません。

- `loader.c`、`loader.h`: `state_save()` / `state_load()` が成功したかどうかを返すようにした。試したパスを返す `loader_get_last_state_path()` を足した。`ALT_PATH_SEP` があるときは、`base()` が `\` でもパスを区切るようにした(Windows CE のパスでセーブの場所がおかしくなっていたため)。セーブデータ・時計・ステートセーブを、ROM のファイル名に `.srm` / `.rtc` / `.state` を足した名前で書くようにした。中身が変わっていないときは書かない `sram_save_if_changed()` / `sram_save_checkpoint()` を足した。前の名前の `.sav` / `.rtc` / `.000` は、新しい名前のファイルが無いときだけ読むようにした。セーブのファイルがあるのに読めなかったときは、その ROM では保存しないようにした
- `rtc.c`、`rtc.h`: `rtc_load_internal()` が読めた値の数を返すようにした(`.rtc` を読み切れなかったことを見分けるため)
- `rc.h`: `rcvars.c` にある `rc_getmem()` / `rc_getmem_n()` の宣言を足した(宣言が抜けていたため)
- `emu.c`、`sys.h`: 処理時間を測るための呼び出しを足した。`-DCE_PERF_DIAG` を付けたときだけコンパイルされ、ふだんのビルドでは何も変わらない
- `.gitignore`: PopGB のビルドの生成物などを除外するように書き足した

このほか、`loader.c` の `fopen()` は、`sys/ce/compat/stdio.h`(`-I./sys/ce/compat` で先に読まれるヘッダ)で UTF-8 のパスに対応したラッパーへ置き換えています。`loader.c` のファイル自体は変えていません。

上流のソースの著作権表示は、変えずに残しています。

## 3. PopGB の自作部分: MIT

PopGB の自作部分は、MIT ライセンスです。本文は [`LICENSE`](LICENSE) の2節にあります。対象は、先頭に `SPDX-License-Identifier: MIT` と書いてある次のファイルです。

- `sys/ce/` のフロントエンド(`ce_*.c`、`ce_*.h`、`ce_res.rc`、`compat/`)。ただし次のものは除きます
  - フォントのデータ `ce_shinonome16.h`、`ce_galmuri14.h`、`ce_galmuri11.h`(下の「第三者のもの」を参照)
  - マスコットの画像とアプリのアイコン(下の「マスコットの絵とアイコン」を参照)
- `sys/ce/tools/` のフォント変換スクリプト(`shinonome2c.py`、`galmuri2c.py`)
- `Makefile.ce`

自作部分だけを取り出して、ほかのプロジェクトで MIT として使うことができます。gnuboy と組み合わせて配布する場合は、1 の条件も守る必要があります。

## 4. マスコットの絵とアイコン: CC0 1.0

メニューのマスコットの絵(`sys/ce/icon/popgb_mascot.bmp`)、アプリのアイコン(`sys/ce/icon/popgb.ico`、`AppMain.exe` に入っているもの)、README の先頭の絵(`.github/images/popgb_mascot_C_kyoudai_4x.png`)は、[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)(パブリックドメイン)です。アイコンは、Pop シリーズのマスコットをもとに AI(Claude)で作りました。

## 5. 第三者のもの

`AppMain.exe` に入っている第三者のものは次のとおりです。著作権表示と許諾文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。

| もの | 作者 | ライセンス |
|---|---|---|
| gnuboy 本体(`Makefile.ce` の `OBJS`) | Laguna 氏、Gilgamesh 氏ほか(`docs/CREDITS`) | GPL-2.0(上流と同じ条件) |
| `inflate.c`(gzip 展開) | David Madore 氏 | パブリックドメイン |
| miniz / tinfl(`miniz_tinfl.c`、`miniz.h`) | RAD Game Tools、Valve Software、Rich Geldreich 氏、Tenacious Software LLC | MIT |
| XZ Embedded(`xz/`) | Lasse Collin 氏、Igor Pavlov 氏 | パブリックドメイン |
| Galmuri フォント(メニューの既定の文字) | Lee Minseo 氏 | SIL Open Font License 1.1 |
| 東雲 16 ドットフォント | 古川泰之 氏ほか、/efont/ | 実質パブリックドメイン |

どれも GPL と組み合わせて配布できます。非商用に限る条件のものは入っていません。

## 6. 同梱していないもの

- ゲームの ROM。任天堂の著作物などは含みません。使う人が自分で用意してください

## 7. 上流のファイルについての注意

- 次のものは上流のファイルで、PopGB のビルドには使っていません(`AppMain.exe` には入っていません)。ビルドに使うファイルは `Makefile.ce` で決まります
  - ほかの機種向けのフロントエンド(`sys/` の `sys/ce/` 以外)
  - gnuboy のコマンドライン版の入口・メニュー・デバッガなど(`main.c`、`menu.c`、`events.c`、`keytable.c`、`rccmds.c`、`rckeys.c`、`rcfile.c`、`exports.c`、`debug.c`、`split.c`、`path.c`、`newsound.c`)
  - x86 用のアセンブリ(`asm/`)
  - ほかのビルド設定(`Makefile.in`、`Makefile.nix`、`Makefile.dos`、`Makefile.win`、`configure`、`configure.ac`、`install-sh`、`Rules`)
- `docs/` は上流の gnuboy の説明です
- リポジトリ直下の `README` は上流の gnuboy の説明で、PopGB の説明ではありません
