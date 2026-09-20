# 初期調査: issue 対応と公開

調査日: 2026-09-19

## 対象と完了条件

対象リポジトリは `AllegroMoltoV/obs-loudness-balance-monitor` です。利用者が指定した完了条件は、GitHub issue #1 と #2 の両方への対応、OBS 実機での人間による動作確認、GitHub release の作成、OBS Forum への最新 release URL とリリースノートの掲載です。

## 確認できた状態

- ローカルの `main` は `origin/main` と一致し、HEAD は `1e92e91`、タグは `v0.1.1` です。調査開始時の作業ツリーに変更はありませんでした。
- `gh issue list --state all` で未解決 issue を 2 件確認しました。[#2](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/issues/2) はポータブル版 OBS への配置、手動インストール時のディレクトリ構造、OBS 32.1 以降との互換性に関する質問です。[#1](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/issues/1) は macOS Tahoe 26.2 と OBS 32.0.4 でのビルド失敗に関する報告です。いずれもコメントはありませんでした。報告された不具合や互換性の有無は未検証です。
- [最新 release `v0.1.1`](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/releases/tag/v0.1.1) は 2026-01-05 に公開され、`loudness-balance-monitor-0.1.1-windows-x64.zip` が付いています。`buildspec.json` のバージョンも `0.1.1` です。
- [OBS Forum の掲載ページ](https://obsproject.com/forum/resources/loudness-balance-monitor.2327/) は `0.1.1` を表示しています。ChatGPT 拡張機能を通したブラウザーで `AllegroMoltoV` のログイン済み表示を確認しました。掲載ページには、リソースの投稿・更新には 2 要素認証が必要と表示されています。更新操作の可否は未検証です。
- Windows 開発機には Visual Studio 2022 Community と CMake 4.2.1 が存在します。ローカルに `build_x64` はありません。ビルドの成否と OBS 実機での挙動は未確認です。
- `CMakePresets.json` の `windows-x64` は Visual Studio 17 2022 を指定します。`cmake/windows/defaults.cmake` は既定のインストール先を `%ALLUSERSPROFILE%/obs-studio/plugins` に設定し、`cmake/windows/helpers.cmake` はプラグインの `bin/64bit` と `data` を配置します。ポータブル版 OBS に適した配置先は、この調査だけでは確定していません。
- `.github/workflows/push.yaml` はタグの push を受けて draft release を作る構成です。現行のタグ判定は `1.2.3` 形式で、既存タグの `v0.1.1` 形式とは一致しません。次の release 手順は、対象 issue と変更内容が確定した後に検証する必要があります。

## 次の作業

両 issue の技術調査と検証経路は [issue #1・#2 の技術調査](2026-09-19-issues.md) に記録します。Forum 更新前には、2 要素認証による更新条件を満たす必要があります。
