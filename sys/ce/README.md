# PopGB — SHARP Brain (Windows CE) 移植版

## 概要

- ゲームボーイ / ゲームボーイカラーのエミュレータ **gnuboy** を、SHARP の電子辞書 **Brain PW-G5300**(Windows CE / ARM)向けに移植したものです。libretro コアではなく、独立したエミュレータ本体 gnuboy を土台に、Win32 のフロントエンド(`sys/ce/` 以下)を新しく書いて繋いでいます
- コア本体(`lcd.c` / `cpu.c` / `sound.c` など)は変えていません。上流のファイルで変えたところは [`LICENSING.md`](LICENSING.md) の2節にまとめています
- 市販・自作を問わず GB / GBC の ROM を実行できます。gzip / DEFLATE / `.xz` で圧縮された ROM もそのまま読み込める作りです(zlib は使っていません。**圧縮 ROM の読み込みは実機未確認**)
- ゲームの ROM は同梱していません。利用者が合法的に用意したものを使ってください
- SHARP・任天堂とは関係のない、非公式のファンプロジェクトです

### 主な機能

- 日本語 / 英語の UI(Galmuri14 と東雲 16 ドットのビットマップフォントを `AppMain.exe` に内蔵)
- 画面の表示倍率(x1 / x2 / Wide / Full)、GDI で直接描画
- カラーフィルター
- フレームスキップ
- サウンド出力(レート・ビット深度・品質・バッファサイズ。オーディオのリングバッファを基準にしたフレームの速さの調整、音が途切れたときのプチッという音を抑えるフェードつき — **音まわりの一部は実機未確認**)
- 入力キーのリマップ(斜め入力を含む)
- ステートセーブ / ロード(セーブ前に確認)、電池バックアップ(SRAM)の持ち越し
- 画面の保存(スクリーンショット。`AppMain.exe` と同じフォルダの `Screenshots` に BMP で保存)
- 日本語のファイル名・フォルダ名に対応した ROM 選択画面(前回開いたフォルダから始まります)
- デバッグログ出力のトグル(既定は OFF)

## ダウンロード

ビルド済みの実行ファイルを [Release](../../../../releases) に置いています。

## ビルド方法

- SDK / ツールチェーン
  * WSL(Windows 上の Linux)に入れた cegcc(`arm-mingw32ce-*` クロスコンパイラ)
- ビルド手順(リポジトリ直下で実行)

```sh
make -f Makefile.ce clean && make -f Makefile.ce && make -f Makefile.ce strip
```

- 生成物はリポジトリ直下の `AppMain.exe`(依存 DLL は `COREDLL.dll` のみ)。UI が使うビットマップフォントはバイナリに埋め込み済み(`sys/ce/ce_galmuri14.h`、`sys/ce/ce_shinonome16.h`)なので、外部のフォントファイルは不要です
- `make -f Makefile.ce CE_FONT=shinonome` で東雲 16 ドット、`make -f Makefile.ce CE_FONT=galmuri11` で GalmuriMono11 のメニュー文字の版(`AppMain_shinonome.exe` / `AppMain_galmuri11.exe`)も作れます

## 使用方法

対象は SHARP Brain(PW-G5300)。PC にリムーバブルディスクとして接続し、ドライブ直下に次の構成を作ります(メニュー項目名は機種により異なる場合があります):

```
<ドライブ直下>/
  アプリ/
    <任意のアプリ名>/
      AppMain.exe    ← ビルド生成物をそのまま
      index.din      ← 中身は空でよいダミーファイル
```

- ROM ファイルは SD カード上に置いてください。アプリ内の「Open ROM…」から選べます
- `index.din` をこの名前で置くと、そのフォルダが [追加アプリ・動画] に一覧表示されます
- 設定ファイル `PopGB.cfg` は、`AppMain.exe` と同じフォルダに作られます。設定ファイルが無いときの既定値は、UI 言語=日本語、デバッグログ=OFF、画面の表示倍率=Full です
- `PopGB_debug.log` は Video Config で「デバッグログを有効にする」を ON にしたときのみ生成されます(既定は OFF)
- セーブデータ(`.srm`)、時計付きのカートリッジの時計(`.rtc`)、ステートセーブ(`.state`)のファイルは、ROM と同じフォルダに、ROM のファイル名に拡張子を足した名前で作られます(例: `Zelda.DX.gbc` なら `Zelda.DX.gbc.srm`)。セーブデータは、メニューを開いたとき、ROM を切り替えたとき、終了したときに書き込みます。セーブしないゲームでは作られません。時計付きのカートリッジでの動作は確かめていません
- v1.0.3 までのファイル(ROM のファイル名を最初の `.` で切った名前の `.sav`、`.rtc`、`.000`)は、新しい名前のファイルが無いときだけ読み込みます。書き換えも削除もしません。`.rtc` を読み込むことは確かめていません

### 日本語フォルダ名について

ROM・セーブファイルとも、フォルダ名・ファイル名に日本語を含んでいても開けます(**実機確認済み**)。`fopen()` の呼び出しを UTF-8 対応のラッパー(`ce_fopen_utf8()`)へ差し替えることで対応しており、コア本体の `loader.c` はそのままです。

## 動作確認環境

- SHARP Brain PW-G5300

PopGBA を CeOpener や CERestorer から起動すると、PW-G5300 で、終了後に画面が真っ暗になる、または操作を受け付けなくなることがありました。USB ケーブルと電池を抜いてから入れ直すと戻りました。PopGB での動作は確かめていません

## クレジット

- **gnuboy** GB / GBC エミュレーションコア — 復活版の gnuboy 1.0.3 / 1.0.4 系列。原作者は **Laguna** 氏・**Gilgamesh** 氏ほか(`docs/CREDITS`)。このリポジトリは **rofl0r** 氏が保守している版(<https://github.com/rofl0r/gnuboy>)を元にしています
- **inflate.c**(gzip 展開)— **David Madore** 氏(1999)。パブリックドメイン
- **miniz / tinfl**(DEFLATE 展開)— RAD Game Tools / Valve Software、**Rich Geldreich** 氏 / Tenacious Software LLC。MIT
- **XZ Embedded**(`.xz` 圧縮 ROM 対応)— **Lasse Collin** 氏、**Igor Pavlov** 氏。パブリックドメイン
- **Galmuri** ビットマップフォント(メニューの既定の文字)— **Lee Minseo**(quiple)氏。SIL Open Font License 1.1
  <https://github.com/quiple/galmuri>
- **東雲(しののめ)16 ドットビットマップフォント** — メインデザイン **古川 泰之** 氏ほか、**The Electronic Font Open Laboratory(/efont/)**。実質パブリックドメイン
  <https://github.com/code4fukui/shinonome-font>
- **CeGCC** — Windows CE / ARM 向けクロスコンパイラ(`arm-mingw32ce-*`)。プロジェクト創設・主要開発の **Danny Backx** 氏、および gcc / binutils をモダンな版へ引き上げた cegcc-build / cegcc-mk の **Max Kellermann** 氏。本プロジェクトのビルドは後者を使用しています
- **SHARP Brain homebrew コミュニティ** — 端末固有の情報を Wiki やフォーラムに残してくださった皆さん
- Windows CE フロントエンド(`sys/ce/`)は本プロジェクトで作成

コンポーネントごとの出所とライセンスの詳細は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) と [`LICENSING.md`](LICENSING.md) をご覧ください。

## 制作について

コードとマスコットの絵はAI(Claude)で作りました。製作者はプログラムを読めません。

製作者がしたのは、AI への指示と、実機(PW-G5300)での確認です。アプリが途中で異常終了したときは、例外の内容を記録する [Brain Exception Reporter(BER)](https://github.com/Sgch/ber) を使いました。電源が落ちたときなどは、AI にデバッグ用の記録(ログ)を残す仕組みを作ってもらいました。そのうえで同じ操作をもう一度試し、記録を AI に読んでもらって原因を調べ、直してもらうことを繰り返して作りました。

マスコットの絵とアイコン(`sys/ce/icon/`)は CC0 1.0(パブリックドメイン)です。アイコンは、Pop シリーズのマスコットをもとに AI(Claude)で作りました。各ライセンスについては、[`LICENSING.md`](LICENSING.md) をご覧ください。

## ライセンス

PopGB 全体は、上流の gnuboy と同じ条件の **GNU General Public License バージョン2(GPL-2.0)** で配布します。本文はリポジトリ直下の [`COPYING`](../../COPYING) にあります。

- ソース・バイナリとも再配布できます(GPL の範囲で商用利用もできます)。ライセンスの文と各ファイルの著作権表示は残してください
- バイナリを配るときは、対応するソース一式(このリポジトリ)も入手できるようにしてください
- 改変したものも、全体を GPL で配布してください
- 保証はありません

PopGB の自作部分(`SPDX-License-Identifier: MIT` と書いてあるファイル)は MIT です([`LICENSE`](LICENSE) の2節)。バイナリを配るときは、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) も一緒に配ってください。

## 商標・免責

PopGB は非公式のファンプロジェクトです。シャープ株式会社、任天堂株式会社とは関係がなく、許諾・後援も受けていません。

- 「SHARP」「Brain」はシャープ株式会社の商標です
- 「ゲームボーイ」「Game Boy」「Game Boy Color」「Nintendo」「任天堂」は任天堂の商標です

ゲームの ROM は含みません。利用者が合法的に入手したものを用意してください。
