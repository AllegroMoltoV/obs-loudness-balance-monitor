# Loudness Balance Monitor

**OBS Studio 向けリアルタイム音量バランス監視プラグイン**

配信中に「声がBGMに埋もれている」「全体の音量が小さい」といった問題を視覚的にフィードバックし、適切な音量バランスを維持するのを支援します。

---

## Features

- **Voice Activity Detection (VAD)** - 声の有無を自動検出（しきい値調整可能）
- **LUFS Measurement** - libebur128 による業界標準のラウドネス計測
- **Balance Monitoring** - 声とBGMのバランスを OK/WARN/BAD で表示
- **Mix Loudness** - 全体の音量レベル監視
- **Peak/Clip Detection** - クリッピング（音割れ）検出
- **Qt Dock UI** - OBS に統合されたドックウィジェット
- **Localization** - 日本語 / English 対応

## Status Indicators

| Status | Balance (声-BGM) | Mix (全体音量) | Clip (ピーク) |
|--------|------------------|----------------|---------------|
| **OK** (緑) | +6 LU 以上 | -18 LUFS 以上 | -1 dBFS 未満 |
| **WARN** (黄) | +3 〜 +6 LU | -22 〜 -18 LUFS | -1 〜 0 dBFS |
| **BAD** (赤) | +3 LU 未満 | -22 LUFS 未満 | 0 dBFS 以上 |

## Requirements

- OBS Studio 31.0.0+
- Windows 10/11 (x64)
- macOS 12.0+ (Universal: Intel + Apple Silicon)
- Ubuntu 22.04+ / 24.04+ (x86_64)

## Installation

### Windows

1. [Releases](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/releases) から ZIP をダウンロード
2. OBS Studio を終了
3. ZIP を展開し、中身を OBS のインストールフォルダにコピー
4. OBS Studio を起動
5. メニュー **ドック** → **音量バランスモニター** を有効化

### macOS

1. [Releases](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/releases) から PKG をダウンロード
2. インストーラーを実行
3. OBS Studio を起動
4. メニュー **Docks** → **Loudness Balance Monitor** を有効化

### Linux

```bash
sudo dpkg -i loudness-balance-monitor-*.deb
```

OBS Studio を起動し、**Docks** → **Loudness Balance Monitor** を有効化

## Usage

1. ドックで **声** ソース（マイク）を選択
2. モニターしたい **BGM** ソースにチェック
3. 配信中はステータスインジケーターを確認:
   - **緑** = 良好
   - **黄** = 注意
   - **赤** = 問題あり

### Settings

| 設定 | 説明 |
|------|------|
| **VAD Threshold** | 音声検出のしきい値 (-60 〜 -20 dB) |
| **Balance Target** | 目標バランス値 (0 〜 20 LU) |
| **Mix Preset** | YouTube標準 / 小さめ安全 / 大きめ攻め |

## Known Limitations

- **Mix は推定値**: Voice + 選択BGMソースの合算です。OBS のマスター出力とは異なる場合があります
- **フェーダー位置**: ソースのフェーダー位置は反映されない可能性があります
- **True Peak**: 未実装（Sample Peak のみ）
- **自動調整**: 音量の自動調整機能はありません（監視のみ）

---

## Building from Source

### Requirements

| Platform | Tools |
|----------|-------|
| Windows | Visual Studio 2022, CMake 3.28+ |
| macOS | Xcode 16.0+, CMake 3.28+ |
| Ubuntu | CMake 3.28+, ninja-build, pkg-config, build-essential |

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
- Attack: 150 ms
- Release: 600 ms
- Default threshold: -40 dBFS

**Dependencies:**
- [libebur128](https://github.com/jiixyj/libebur128) v1.2.6 (MIT License, statically linked)

---

## License

GPL-2.0 (same as OBS Studio)

## Author

**AllegroMoltoV** - https://www.allegromoltov.jp
