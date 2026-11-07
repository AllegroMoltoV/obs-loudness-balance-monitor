# OBS 33 の Windows 配置移行

## 対象と結果

Loudness Balance Monitor 1.0.2 の候補を OBS 33 の新構成へ変更した。音声処理、評価ロジック、設定形式は変更していない。計画の正本は Beads の obs-loudness-balance-monitor-142。

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

## 残る提供工程

通常環境での依頼者の目視確認、最終タグの配布 ZIP 確認、Release・Forum 更新は未実施。Simple Drop Shadow の候補がそろってから、先行 2 製品の目視確認へ進む。Simple Pitch Shift を待たない。

ローカル証拠は .deps/install-142-ci-success と .deps/install-142-portable-fixed に保存した。
