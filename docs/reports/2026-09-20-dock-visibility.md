# OBS 起動時の音量バランスモニタードックの表示状態

調査日: 2026-09-20

## 結論

OBS 32.1.2 では、保存済みのドック配置を復元した後にプラグインのドックが登録される。登録 API がドックを非表示にするため、修正前は前回の表示状態が引き継がれなかった。

`src/plugin-main.cpp` で、登録成功後に OBS が保存した `DockState` を再適用するようにした。隔離版 OBS 32.1.2 では、表示して正常終了した場合も、非表示で正常終了した場合も、次回起動時にその状態が復元された。対応 OS の CI ビルドは未実施である。

## 根拠

- `src/plugin-main.cpp` は `OBS_FRONTEND_EVENT_FINISHED_LOADING` で `LoudnessDock` を作成し、`obs_frontend_add_dock_by_id()` を呼ぶ。
- `src/loudness-dock.cpp` のコンストラクターは設定を読み込み、解析器と 100 ms 間隔のタイマーを開始する。
- [OBS 32.1.2 の起動処理](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/widgets/OBSBasic.cpp#L1222-L1230)は、保存済みの `DockState` を先に復元する。[起動完了通知](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/widgets/OBSBasic.cpp#L1367)はその後に送られる。
- [OBS 32.1.2 の `obs_frontend_add_dock_by_id()`](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/OBSStudioAPI.cpp#L309-L329)は、ドックを追加した直後に `setVisible(false)` と `setFloating(true)` を呼ぶ。
- 隔離版の `.tmp/issue-2-portable/obs-32.1.2/config/obs-studio/user.ini` には `DockState` があり、そのバイナリを確認すると `loudness-balance-monitor-dock` の識別子が含まれる。OBS はドックの配置データを保存しているが、プラグインが登録する時点でドックは再び非表示になる。

## 復元方法

OBS のユーザー設定には `DockState` が保存される。[フロントエンド API](https://docs.obsproject.com/reference-frontend-api) には、この設定を取得する `obs_frontend_get_user_config()` がある。[Qt の `QMainWindow::restoreState()`](https://doc.qt.io/qt-6/qmainwindow.html) はドックの識別子を使って保存済み配置を復元する。OBS 自身も、[Twitch 用ドックの追加後に `restoreState()` を呼ぶ](https://github.com/obsproject/obs-studio/blob/master/frontend/oauth/TwitchAuth.cpp)。プラグインではドック登録後に `BasicWindow/DockState` を読み、値がある場合だけ Base64 を復号して再適用する。復元の成否を OBS ログへ記録する。保存値がない場合は登録後の状態を変更しない。

Qt の `restoreDockWidget()` は、`restoreState()` より後に作成したドックの復元に使える。しかし、[Qt の `addDockWidget()` は同じ識別子の復元用プレースホルダーを削除する](https://github.com/qt/qtbase/blob/dev/src/widgets/widgets/qdockarealayout.cpp)。現行の OBS API は先にドックを追加するため、その後で `restoreDockWidget()` だけを呼ぶ方法では復元できないとソースから判断した。この判断も OBS 実機での測定ではない。

## 検証結果

- Windows の `cmake --preset windows-ci-x64 -DENABLE_POST_BUILD_INSTALL=OFF` と `cmake --build --preset windows-ci-x64` が成功した。既定のビルド設定では、コンパイル後の `C:\Program Files` へのインストールが権限不足で失敗したため、ローカル検証ではインストール処理を無効にした。整形検査と `git diff --check` も成功した。
- 修正版 DLL を `.tmp/issue-2-portable/obs-32.1.2` にのみ配置した。OBS ログ `2026-09-20 11-20-05.txt`、`11-22-07.txt`、`11-24-07.txt` には、ポータブルモードとドック登録・復元の成功が記録されている。いずれもメニューから正常終了し、メモリーリーク数は 0 だった。
- 表示した状態で終了すると、再起動時に音量バランスモニターが表示された。声・BGM・ミックスの値はそれぞれ約 `-19.6`、`-26.6`、`-18.8 LUFS`、声と BGM の差は `+7.0 LU` と表示された。検証用音声ソースと OBS ミキサーも動作した。
- 「ドック」メニューで非表示にして終了すると、再起動時も非表示だった。同メニューのチェックは外れており、再表示すると測定値が再び表示された。
- 隔離版で利用できる標準ドック (シーン、ソース、音声ミキサー、シーントランジション、コントロール) は、表示状態と配置に目視で異常がなかった。他のサードパーティ製ドックとの組合せは未検証である。

起動時に OBS 全体の `DockState` を再適用するため、先に起動した別プラグインが同じ起動中に変更したドック配置を巻き戻す可能性は残る。隔離版にはその組合せを検証できる別プラグインがなかった。
