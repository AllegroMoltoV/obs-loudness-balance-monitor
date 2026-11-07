# OBS 33 の Windows 配置移行

## 対象と結果

Loudness Balance Monitor 1.0.2 を OBS 33 の新構成へ変更した。音声処理、評価ロジック、設定形式は変更していない。計画の正本は Beads の obs-loudness-balance-monitor-142。

DLL と PDB を製品フォルダー直下へ配置し、data/locale を維持した。README にフォルダー未作成時、新規導入、旧版更新、ポータブル版の手順を記載した。

## 検証

[GitHub Actions 42370546368](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/actions/runs/42370546368) はコミット 90c1c4c で Windows・macOS・Ubuntu の全ジョブに成功した。

Windows の通常版検証を GitHub-hosted の新規環境で実施した。

| 段階 | 確認内容 | 結果 |
| --- | --- | --- |
| fresh | 新構成 ZIP の配置、DLL ロードパス、声・BGM のメーター | 成功 |
| old-setup | 公式 1.0.1 ZIP のハッシュ、旧配置のロード、設定保存 | 成功 |
| upgrade | 旧 DLL を残した新版上書き、新 DLL のロード、設定復元 | 成功 |
| restore-hidden | ドック非表示状態の復元、設定とメーター | 成功 |

更新後の実ロード先は C:/ProgramData/obs-studio/plugins/loudness-balance-monitor/loudness-balance-monitor.dll。旧版 ZIP の SHA256 は 755c85def433f7807534c22d2b15d46c4e76ca16fe7b26a302d0c689e9c99ad5。

ローカルの公式 OBS 33.0.1 の隔離ポータブル環境でも、新配置の読み込み、設定復元、非表示状態復元に成功した。実ロード先は runtime/plugins/loudness-balance-monitor/loudness-balance-monitor.dll。ADAT へ試験音を出していない。

macOS と Ubuntu は既存のビルド・音声検証を維持した。今回の Windows 配置変更を理由に、未検証の OS や実機への保証を拡張していない。

## 検証処理の修正

OBS が返すモジュールパスには相対パスが含まれる。検証処理を OBS の作業ディレクトリ基準で絶対化する方式へ修正した。

新規導入試験と旧版準備で設定を混在させると、存在しないソースの選択が残った。両試験の初期状態を分離した。新規導入の設定を証拠として退避し、旧版の準備を独立して行う。旧版の保存から新版への更新では設定とシーンを保持する。

両修正は依頼者 AllegroMoltoV の承認後に実施した。製品の音声処理は変更していない。

## 公開結果

依頼者 AllegroMoltoV が通常環境で先行 2 製品を目視確認し、公開を承認した。PR #10 をマージし、ruleset の全設定を復元・照合した。

[タグ CI 42372805503](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/actions/runs/42372805503) の 3 OS ビルドが成功した。最終 ZIP を隔離 OBS 33.0.1 の新配置から読み込み、設定保存と声・BGM のメーターを確認した。Release 用ビルドの確認では、完了済みの旧版更新試験を繰り返していない。

[1.0.2 Release](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/releases/tag/v1.0.2) を公開し、再取得した ZIP と検証品の SHA256 が一致した。

- ZIP: D1A01A05656942AB114C8FB2BE01CB9F06333B3E589A575B45E351AEC16D5552
- DLL: 377211352B2E871AC00717FEDDCF01FCF8A5E2F75300C50BF9297DD75EAE52AB
- 証拠: .deps/install-142-final-load/results

Forum の本文と外部ダウンロード URL を更新した。Go to download が 1.0.2 Release を開くことを Brave で確認した。AI 利用開示と GIF を保持した。

## 保留事項

Forum リソースに審査待ちと表示され、新版の追加操作が表示されていない。既存の更新投稿 8817・8819 を保持した。1.0.2 の更新履歴登録は、依頼者の指定により審査通過後の再依頼まで保留する。本文の保存と一般公開の反映を同一視しない。

通常環境には目視確認済みの CI 候補を保持している。公開 ZIP 自体のロード検証は隔離環境で完了した。Simple Pitch Shift は別工程として再開計画を提示する。
