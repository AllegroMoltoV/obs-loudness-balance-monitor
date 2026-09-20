# OBS 起動時の音量バランスモニタードックの表示状態

調査日: 2026-09-20

## 結論

現行のプラグインは OBS の起動完了通知を受けるたびにドックを作成・登録する。OBS 32.1.2 の登録 API は、登録したドックを明示的に非表示にする。前回ドックを開いたまま OBS を終了しても、次回起動時はドックを手動で開く必要がある。利用者の観察と実装が一致する。

これはドックの表示状態の問題であり、プラグイン自体が起動していないわけではない。ドックの生成時に音声解析器と画面更新タイマーは開始される。表示されていなくても音声ソースの選択設定は読み込まれる。

## 根拠

- `src/plugin-main.cpp` は `OBS_FRONTEND_EVENT_FINISHED_LOADING` で `LoudnessDock` を作成し、`obs_frontend_add_dock_by_id()` を呼ぶ。起動時にドックを表示する処理も、前回の表示状態を読み込む処理もない。
- `src/loudness-dock.cpp` のコンストラクターは設定を読み込み、解析器と 100 ms 間隔のタイマーを開始する。
- [OBS 32.1.2 の起動処理](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/widgets/OBSBasic.cpp#L1111-L1119)は、保存済みの `DockState` を先に復元する。[起動完了通知](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/widgets/OBSBasic.cpp#L1243-L1246)はその後に送られる。
- [OBS 32.1.2 の `obs_frontend_add_dock_by_id()`](https://github.com/obsproject/obs-studio/blob/32.1.2/frontend/OBSStudioAPI.cpp#L309-L329)は、ドックを追加した直後に `setVisible(false)` と `setFloating(true)` を呼ぶ。
- 隔離版の `.tmp/issue-2-portable/obs-32.1.2/config/obs-studio/user.ini` には `DockState` があり、そのバイナリを確認すると `loudness-balance-monitor-dock` の識別子が含まれる。OBS はドックの配置データを保存しているが、プラグインが登録する時点でドックは再び非表示になる。

## 対応を検討する際の区別

毎回表示するだけなら、登録後に表示する方法が考えられる。前回閉じていた場合は閉じたままにするには、表示状態を保存・復元する仕様と検証が必要になる。今回の調査では実装変更を行っていない。
