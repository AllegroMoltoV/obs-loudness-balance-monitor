# Qt 依存更新と Windows ポータブル版の追加検証

検証日: 2026-09-19

## 結果

- issue #1: 公式 OBS の Qt 依存パッケージを 2025-08-23 版へ更新した。以前 `AGL` のリンクエラーが出た macOS 26 の GitHub Actions で、Universal バイナリのリンクと配布用アーカイブの生成に成功した。CI で再現したリンク失敗は解消した。
- issue #2: 隔離した OBS 32.1.2 ポータブル版は、`obs-plugins/64bit` へ置いたプラグイン v0.1.1 の DLL を読み込んだ。Computer Use でドック本体の日本語表示を確認した。正常終了時には設定ファイルの書き込みエラーが再現した。

## issue #1: Qt 依存更新後の CI

`buildspec.json` の Qt 依存パッケージを 2025-07-11 版から [2025-08-23 版](https://github.com/obsproject/obs-deps/releases/tag/2025-08-23)へ更新した。macOS Universal 版と Windows x64 版の SHA-256、および Windows x64 版デバッグシンボルの SHA-256 も同じ公開版に合わせた。変更はコミット `f71d4a985c46d1e4db273ef9c11b15c007339f3b` に含まれる。前回再現した `AGL` の混入経路は[先の検証報告](2026-09-19-validation-gates.md)に記録した。

[GitHub Actions #35440650908](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/actions/runs/35440650908)は、このコミットを対象に実行され、macOS、Windows、Ubuntu の全ジョブが成功した。macOS のログには `macos-deps-qt6-2025-08-23-universal.tar.xz` の取得、両アーキテクチャ向けソースのコンパイル、プラグインのリンク、Universal バイナリの作成、アーカイブのアップロードが記録された。取得した生成物は `loudness-balance-monitor-0.1.1-macos-universal.tar.xz` であり、内部にプラグイン本体と `ja-JP.ini`、`en-US.ini` がある。Windows のログにも `windows-deps-qt6-2025-08-23-x64.zip` の取得、ビルド、ZIP 生成物のアップロードが記録された。

この実行は `macos-ci` の `CMAKE_AUTOMOC_COMPILER_PREDEFINES=false` を使う。AutoMoc の事前定義生成で以前起きた配置ターゲット競合の原因は未特定である。CI が参照する OBS ソースは 31.1.1 で、issue #1 の報告環境である OBS 32.0.4 とは異なる。CI は macOS 上の OBS にプラグインを読み込ませておらず、音声解析、署名、正式なインストーラー配布の成否も示さない。ビルド工程で一時的な `.pkg` は作成されたが、配布用アーティファクトは `.tar.xz` である。

完全なジョブログはローカルの `.logs/issue-1-qt-update-macos-2026-09-19.log` と `.logs/issue-1-qt-update-windows-2026-09-19.log` に保存した。

## issue #2: 組込探索先での読み込み

公式 OBS 32.1.2 の ZIP とプラグイン v0.1.1 の ZIP を公開 SHA-256 と照合した。OBS の一時展開先の `obs-plugins/64bit/loudness-balance-monitor.dll` と `data/obs-plugins/loudness-balance-monitor/locale/*.ini` に配置し、コピー元とコピー先のハッシュを照合した。この配置は [OBS 32.1.2 の Windows 標準探索先](https://github.com/obsproject/obs-studio/blob/32.1.2/libobs/obs-windows.c#L33-L42)に対応する。前回読み込まれなかった `data/plugins/<プラグイン名>` の配置も残したが、実行中プロセスのモジュール絶対パスから今回読み込まれた DLL を識別した。

OBS を `bin/64bit` を作業ディレクトリとして起動した。ログには `Portable mode: true`、`OBS 32.1.2 (64-bit, windows)`、`plugin loaded successfully (version 0.1.1)`、`Dock registered successfully` が記録された。PID 17512 のモジュール一覧でも、一時展開先の `obs-plugins/64bit/loudness-balance-monitor.dll` を確認した。当該プラグインの言語ファイル読み込みエラーはログに見つからなかった。

Computer Use の `sky.list_apps()` と `sky.list_windows()` には、この PID の OBS ウィンドウが表示されなかった。プロセス側では一時展開先の実行ファイルパスとウィンドウハンドルを確認できたが、Computer Use 側の対象ウィンドウと照合できなかったため、GUI 操作は行っていない。パスを再照合して PID 17512 だけを停止した。強制停止のため、終了時の設定保存も判定しない。OBS の完全なログはローカルの `.logs/issue-2-portable-legacy-2026-09-19.log` に保存した。

利用者の操作直前承認を受け、Computer Use の `sky.launch_app` に隔離版の実行ファイル絶対パスを渡して再試行した。しかし、起動後に列挙されたのは `C:\Program Files\obs-studio\bin\64bit\obs64.exe` の OBS 32.2.2 だった。隔離版のウィンドウではないため、ドックの操作はしていない。インストール済み OBS の画面取得は安全審査が拒否した。別の手段でその画面を取得せず、利用者へ開いた OBS の終了を依頼した。

利用者がインストール済み OBS を終了した後、起動先と作業ディレクトリを隔離版に固定したショートカットを一時領域へ作成した。利用者がこれを開いた際の PID 1988 と Computer Use の `window.app` は、共に隔離版の `bin/64bit/obs64.exe` を指した。「ドック」メニューに「音量バランスモニター」があり、有効化すると同名のドック本体にソース選択、ステータス、メーター、設定が表示された。これにより OBS 32.1.2 ポータブル版での読み込み、ドック登録、日本語表示を確認した。PID 1988 のモジュール絶対パスも隔離版ルートの `obs-plugins/64bit/loudness-balance-monitor.dll` だった。

OBS の「ファイル」→「終了」から正常終了し、PID 1988 の消滅を確認した。終了ログは `os_quick_write_utf8_file_safe: failed to write to ../../config/obs-studio/plugin_config/loudness-balance-monitor/settings.json.tmp` を記録した。隔離版の `plugin_config` は存在したが、`loudness-balance-monitor` ディレクトリと `settings.json` は存在しなかった。前回のインストール済み OBS 32.2.2 でも同じ書き込みエラーが出ていた。`src/loudness-dock.cpp` の `save_settings()` は取得したパスへ保存する前にディレクトリを作成していない。この欠落が今回の書き込み失敗と整合する。完全なログはローカルの `.logs/issue-2-portable-gui-2026-09-19.log` に保存した。

[OBS のプラグインガイド](https://obsproject.com/kb/plugins-guide)は `obs-plugins/64bit` へのバイナリ配置を旧方式とし、将来使えなくなる予定としている。この一例の成功だけで恒久的な推奨配置先とはしない。OBS 32.1 以降の音声ミキサーとの連携と音声解析は未確認である。設定は保存エラーが再現したため、修正後の保持確認が必要である。

## 公開前に残る確認

macOS 実機の OBS での読み込み、Windows ポータブル版の設定保存の修正と保持確認、OBS 32 系の音声動作を確認する。確認結果に合わせて README とリリース内容を確定する。GitHub issue への返信は利用者に文案を渡し、Codex は投稿しない。
