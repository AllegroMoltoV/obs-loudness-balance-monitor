# OBS 起動時の音量バランスモニタードックの表示状態

調査日: 2026-09-20

## 結論

現行のプラグインは OBS の起動完了通知を受けるたびにドックを作成・登録する。OBS 32.1.2 の登録 API は、登録したドックを明示的に非表示にする。前回ドックを開いたまま OBS を終了しても、次回起動時はドックを手動で開く必要がある。利用者の観察と実装が一致する。

これはドックの表示状態の問題であり、プラグイン自体が起動していないわけではない。ドックの生成時に音声解析器と画面更新タイマーは開始される。表示されていなくても音声ソースの選択設定は読み込まれる。

## 根拠

- `src/plugin-main.cpp` は `OBS_FRONTEND_EVENT_FINISHED_LOADING` で `LoudnessDock` を作成し、`obs_frontend_add_dock_by_id()` を呼ぶ。起動時にドックを表示する処理も、前回の表示状態を読み込む処理もない。
- `src/loudness-dock.cpp` のコンストラクターは設定を読み込み、解析器と 100 ms 間隔のタイマーを開始する。
- [OBS 32.1.2 の起動処理](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/widgets/OBSBasic.cpp#L1222-L1230)は、保存済みの `DockState` を先に復元する。[起動完了通知](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/widgets/OBSBasic.cpp#L1367)はその後に送られる。
- [OBS 32.1.2 の `obs_frontend_add_dock_by_id()`](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/OBSStudioAPI.cpp#L309-L329)は、ドックを追加した直後に `setVisible(false)` と `setFloating(true)` を呼ぶ。
- 隔離版の `.tmp/issue-2-portable/obs-32.1.2/config/obs-studio/user.ini` には `DockState` があり、そのバイナリを確認すると `loudness-balance-monitor-dock` の識別子が含まれる。OBS はドックの配置データを保存しているが、プラグインが登録する時点でドックは再び非表示になる。

## 復元方法の調査

OBS のユーザー設定には `DockState` が保存される。[フロントエンド API](https://docs.obsproject.com/reference-frontend-api) には、この設定を取得する `obs_frontend_get_user_config()` がある。[Qt の `QMainWindow::restoreState()`](https://doc.qt.io/qt-6/qmainwindow.html) はドックの識別子を使って保存済み配置を復元する。OBS 自身も、[Twitch 用ドックの追加後に `restoreState()` を呼ぶ](https://github.com/obsproject/obs-studio/blob/master/frontend/oauth/TwitchAuth.cpp)。このため、プラグインのドック登録後に保存済み `DockState` を再適用する方法を候補とする。ただし、この方法は OBS 全体のドック配置を再適用する。起動時の最初の復元後に別のプラグインが変更したドック状態を巻き戻す可能性がある。このプラグインでの成否と他ドックへの影響は未検証である。

Qt の `restoreDockWidget()` は、`restoreState()` より後に作成したドックの復元に使える。しかし、[Qt の `addDockWidget()` は同じ識別子の復元用プレースホルダーを削除する](https://github.com/qt/qtbase/blob/dev/src/widgets/widgets/qdockarealayout.cpp)。現行の OBS API は先にドックを追加するため、その後で `restoreDockWidget()` だけを呼ぶ方法では復元できないとソースから判断した。この判断も OBS 実機での測定ではない。

単に登録後にドックを表示する方法では、前回閉じていた状態を保持できない。今回の調査では実装変更を行っていない。
