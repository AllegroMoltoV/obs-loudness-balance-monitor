# Loudness Balance Monitor

**OBS Studio 向けリアルタイム音量バランス監視プラグイン**

配信中に「声が BGM に埋もれている」「全体の音量が小さい」といった問題を視覚的にフィードバックし、適切な音量バランスを維持するのを支援します。

---

## Features

* **Voice Activity Detection (VAD)** - 声の有無を自動検出（しきい値調整可能）
* **LUFS Measurement** - libebur128 による業界標準のラウドネス計測
* **Balance Monitoring** - 声と BGM のバランスを OK/WARN/BAD で表示
* **Mix Loudness** - 全体の音量レベル監視
* **Peak/Clip Detection** - クリッピング（音割れ）検出
* **Qt Dock UI** - OBS に統合されたドックウィジェット
* **Localization** - 日本語 / English 対応

## Status Indicators

| Status       | Balance (声-BGM) | Mix (全体音量)     | Clip (ピーク)  |
| ------------ | --------------- | -------------- | ----------- |
| **OK** (緑)   | +6 LU 以上        | -18 LUFS 以上    | -1 dBFS 未満  |
| **WARN** (黄) | +3 〜 +6 LU      | -22 〜 -18 LUFS | -1 〜 0 dBFS |
| **BAD** (赤)  | +3 LU 未満        | -22 LUFS 未満    | 0 dBFS 以上   |

## Requirements

* OBS Studio 31.0.0+
* Windows 10/11 (x64)

> 注: **現在、ビルド済みバイナリ（Releases で配布しているもの）は Windows のみ**です。
> macOS / Linux で利用する場合は、本ページ下部の **Building from Source** を参照して自分でビルドしてください。

---

## Installation

### Windows（配布バイナリあり）

1. [Releases](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/releases) から `loudness-balance-monitor-0.1.2-windows-x64.zip` をダウンロードします。
2. OBS Studio を終了し、ZIP を展開します。
3. ZIP 内の `loudness-balance-monitor` フォルダーを `%PROGRAMDATA%\obs-studio\plugins` にコピーします。ZIP ルートの `include` と `lib` はインストールに使いません。

   ```text
   C:\ProgramData\obs-studio\plugins\
   └─ loudness-balance-monitor\
      ├─ bin\64bit\loudness-balance-monitor.dll
      └─ data\locale\
         ├─ en-US.ini
         └─ ja-JP.ini
   ```

   `ProgramData` は隠しフォルダーです。エクスプローラーで「隠し項目」を表示するか、アドレス欄に `%PROGRAMDATA%\obs-studio\plugins` を入力してください。
4. OBS Studio を起動し、メニューの **ドック** → **音量バランスモニター** からドックを表示します。現行版では OBS を起動するたびに、この操作が必要です ([issue #3](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/issues/3))。

#### ポータブル版 OBS 32.1.2 で確認した配置

隔離した Windows ポータブル版 OBS 32.1.2 では、次の配置でプラグインの読み込み、音声表示、設定の復元を確認しました。OBS の展開先を基準に、ZIP 内の DLL と言語ファイルをそれぞれコピーします。

```text
<OBS の展開先>\
├─ obs-plugins\64bit\loudness-balance-monitor.dll
└─ data\obs-plugins\loudness-balance-monitor\locale\
   ├─ en-US.ini
   └─ ja-JP.ini
```

[OBS のプラグインガイド](https://obsproject.com/kb/plugins-guide)は、この配置を将来廃止予定の旧方式としています。同ガイドにある `data/plugins` への配置だけでは、今回の OBS 32.1.2 と配布済み v0.1.1 DLL の組み合わせは読み込まれませんでした。上記の動作確認は OBS 32.1.2 に限られます。

### macOS（要ビルド）

現在、Releases では macOS 向けのビルド済みバイナリは配布していません。
**Building from Source** を参照してビルドし、生成物を OBS のプラグインフォルダーへ配置してください。
macOS 26 の CI ではビルドに成功しましたが、macOS 実機の OBS への読み込みと音声動作は未確認です。

### Linux（要ビルド）

現在、Releases では Linux 向けのビルド済みバイナリは配布していません。
**Building from Source** を参照してビルドし、生成物を OBS のプラグインフォルダーへ配置してください。

---

## Usage

1. ドックで **声** ソース（マイク）を選択
2. モニターしたい **BGM** ソースにチェック
3. 配信中はステータスインジケーターを確認:

   * **緑** = 良好
   * **黄** = 注意
   * **赤** = 問題あり

### Settings

| 設定                 | 説明                         |
| ------------------ | -------------------------- |
| **VAD Threshold**  | 音声検出のしきい値 (-60 〜 -20 dB)   |
| **Balance Target** | 目標バランス値 (0 〜 20 LU)        |
| **Mix Preset**     | YouTube 標準 / 小さめ安全 / 大きめ攻め |

## Known Limitations

* **Mix は推定値**: Voice + 選択 BGM ソースの合算です。OBS のマスター出力とは異なる場合があります
* **True Peak**: 未実装（Sample Peak のみ）
* **自動調整**: 音量の自動調整機能はありません（監視のみ）

---

## Building from Source

> macOS / Linux は現状こちらの手順でビルドしてください（Windows もソースビルド可能です）。

### Requirements

| Platform | Tools                                                 |
| -------- | ----------------------------------------------------- |
| Windows  | Visual Studio 2022, CMake 3.28+                       |
| macOS    | Xcode 16.0+, CMake 3.28+                              |
| Ubuntu   | CMake 3.28+, ninja-build, pkg-config, build-essential |

### Build

```bash
# Windows
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo

# macOS
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo

# Ubuntu
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 --config RelWithDebInfo
```

### Technical Details

**Thread Model:**

```
Audio Thread (OBS) → Lock-free Queue → Worker Thread (LUFS) → Atomic Results → UI (10Hz)
```

**VAD Parameters:**

* Attack: 150 ms
* Release: 600 ms
* Default threshold: -40 dBFS

**Dependencies:**

* [libebur128](https://github.com/jiixyj/libebur128) v1.2.6 (MIT License, statically linked)

---

## License

GPL-2.0 (same as OBS Studio)

## Author

**AllegroMoltoV** - [https://www.allegromoltov.jp](https://www.allegromoltov.jp)
