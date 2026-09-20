# issue #1・#2 の追加検証結果

検証日: 2026-09-19

## #1: macOS 26 のリンク失敗

`issue/1-2-validation` のコミット `a3dbdde676ccd07f1f865f63ba04ff008917795e` で、`macos-ci` に限り `CMAKE_AUTOMOC_COMPILER_PREDEFINES=false` を指定した。[GitHub Actions #35437053048](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/actions/runs/35437053048)は Windows と Ubuntu のジョブに成功し、macOS ジョブの `Build Plugin` に失敗した。

macOS では AutoMoc とプラグイン本体のコンパイルを通過し、`loudness-balance-monitor` のリンク時に `ld: framework 'AGL' not found` が出た。issue #1 と同じエラー文字列を再現できた。同じ実行をデバッグログ付きで再実行した結果、`clang++` のリンクコマンドに `-framework AGL` が含まれていた。

現行の[OBS依存パッケージ 2025-07-11](https://github.com/obsproject/obs-deps/releases/tag/2025-07-11)から、macOS Universal版Qtの圧縮ファイルを取得し、公開SHA-256 `d3f5f04b6ea486e032530bdf0187cbda9a54e0a49621a4c8ba984c5023998867` と照合した。内部の `lib/cmake/Qt6/FindWrapOpenGL.cmake` 40～49行は、AGLが見つからなければ `-framework AGL` を `WrapOpenGL::WrapOpenGL` に追加する。`Qt6GuiDependencies.cmake` は `WrapOpenGL` を依存に含め、`Qt6WidgetsDependencies.cmake` は `Qt6Gui` を依存に含める。プラグインの `CMakeLists.txt` は `Qt6::Widgets` をリンクしている。この依存経路が実際のリンク引数と一致する。

[OBS依存パッケージ 2025-08-23](https://github.com/obsproject/obs-deps/releases/tag/2025-08-23)の同名Qtファイルを公開SHA-256 `990f11638b80a4509e14e8c315f6e4caa0861e37fcd3113a256fbff835ffca29` と照合して比較した。Qtのバージョンは両方とも6.8.3で、新しいファイルはAGLのリンク設定を持たない。[Qtの修正コミット](https://github.com/qt/qtbase/commit/cdb33c3d562)と一致する。新パッケージでのビルド成功はまだ未検証である。検証環境のOBSソースは31.1.1で、報告者のOBS 32.0.4と同一条件ではない。AutoMocの事前定義生成を省く指定は切り分け用であり、元の配置ターゲット競合の原因も未特定である。

完全なジョブログはローカルの `.logs/issue-1-automoc-predefs-2026-09-19.log`、デバッグ再実行のログは `.logs/issue-1-agl-debug-2026-09-19.log` に保存した。Qtパッケージ内の比較対象も `.logs/issue-1-qt-*-FindWrapOpenGL.cmake` に保存した。次の検証ではQt依存パッケージを更新してビルドとリンクを確認する。

## #2: Windows ポータブル版のプラグイン探索

[OBS 32.1.2 の公式 ZIP](https://github.com/obsproject/obs-studio/releases/tag/32.1.2)とプラグイン v0.1.1 の配布 ZIP を SHA-256 で照合して、一時領域へ展開した。[OBS のプラグインガイド](https://obsproject.com/kb/plugins-guide)に記された `data/plugins` の下へ `loudness-balance-monitor/bin/64bit/loudness-balance-monitor.dll` と `data/locale/*.ini` を配置した。OBS ルートに `portable_mode` を作り、ZIP 内の `bin/64bit/obs64.exe` をそのディレクトリを作業ディレクトリとして起動した。

OBS ログは `Portable mode: true`、`OBS 32.1.2`、起動完了を記録した。一方、読み込みモジュール一覧にはプラグインがなく、実行中プロセスのモジュール一覧にも該当 DLL がなかった。ドック登録も記録されていない。この配置での読み込みは確認できなかった。実行ファイルは一時領域内であり、テストした PID のパスを照合して終了した。OBS ログはローカルの `.logs/issue-2-portable-obs-2026-09-19.log` に保存した。

OBS 32.1.2 の公式ソースでは、[追加モジュールパスの設定](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/widgets/OBSBasic.cpp#L110-L177)はポータブルモードで `OBS_PLUGINS_PATH` と `OBS_PLUGINS_DATA_PATH` の処理後に戻り、通常の `ProgramData` の探索先を追加しない。[Windows の標準パス](https://github.com/obsproject/obs-studio/blob/32.1.2/libobs/obs-windows.c#L33-L42)は `../../obs-plugins/64bit` と `../../data/obs-plugins/%module%` である。これらのソースは、今回の `data/plugins/<プラグイン名>` が自動探索されなかった結果と整合する。ただし、ドキュメントとの食い違いの理由と、標準パスへ置いた場合の実際の読み込みは未検証である。[OBS のプラグインガイド](https://obsproject.com/kb/plugins-guide)は標準パスのバイナリ配置を旧方式としている。

次の検証では、隔離した同じ OBS に標準パス形式でプラグインを配置し、DLL の絶対パス、ドック登録、設定保存の順に調べる。OBS 32.1 以降の音声ミキサーとの連携は、今回の起動ログからは判定できない。

## 公開前に残る確認

issue #1 の修正版ビルド、issue #2 のポータブル版での読み込みと設定保持、OBS 32 系の画面・音声動作を確認してから release と OBS Forum の更新を検討する。GitHub issue への文章は利用者向けに文案を作り、Codex は投稿しない。
