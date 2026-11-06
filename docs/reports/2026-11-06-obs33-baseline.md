# OBS 33.0.1での既存版の基準検証

## 対象と結果

録音・試聴機能を統合する前に、既存メーター製品の通常導入、起動、設定復元を確認する。計画・進捗の正本はBeadsの`obs-loudness-balance-monitor-udy`。既存製品の基準は`2839304`、配布環境修正は`f8370f0`、共通復元試験の修正は`357c6dc`。

| 対象 | 確認結果 |
| --- | --- |
| Windows x64 | 通常ProgramData導入、3起動で設定・表示復元、メーター応答、正常終了に成功 |
| Ubuntu 26.04 x86_64 | 通常deb導入、3起動で設定・表示復元、メーター応答、正常終了に成功 |
| macOS Universal | Universal生成、arm64で通常pkg導入、3起動で設定・表示復元、メーター応答、正常終了に成功 |

WindowsとUbuntuの根拠は[Actions 42162754291](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/actions/runs/42162754291)。macOSの根拠は修正`d225421`の[Actions 42164856388](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/actions/runs/42164856388)。製品バージョン0.1.3の維持は新版公開を意味しない。

## 通常導入と復元の証拠

Windowsでは新規GitHub-hostedランナーのProgramDataへ配布ZIPの製品フォルダーを配置し、公式OBS33.0.1を非portable起動した。Ubuntuでは公式OBSと製品debをapt導入し、製品用の特別な探索パスなしでロードした。検証モジュールを製品へ混入させていない。

初回に声・BGM選択、VAD=-35、balance=8、preset=1を設定して終了する。2回目には操作前の復元状態を検査し、ドックを隠して終了する。3回目には非表示状態の復元を検査する。各起動でメーター3本の応答と正常終了を確認した。試験音源を隔離シーンで保持し、出力mixers=0で通常音声デバイスへ送らない。

- [Windows初回](../logs/2026-11-06-obs33-windows-setup.json)
- [Windows再起動](../logs/2026-11-06-obs33-windows-restore.json)
- [Windows非表示復元](../logs/2026-11-06-obs33-windows-restore-hidden.json)
- [Linux初回](../logs/2026-11-06-obs33-linux-setup.json)
- [Linux再起動](../logs/2026-11-06-obs33-linux-restore.json)
- [Linux非表示復元](../logs/2026-11-06-obs33-linux-hidden.json)

通常利用中のOBS設定と音声デバイス設定は変更していない。Windowsの旧portable配置の初期検証結果は別の証拠として保存している。

## Ubuntuの配置修正の根拠

公式deb内のOBS実行ファイルと製品deb内容を読み取り、探索先と配置先の不一致を確定した。旧製品debの`/usr/lib/x86_64-linux-gnu/obs-plugins`は、公式OBSの探索対象外だった。

修正後の配置:

```text
/usr/local/lib/x86_64-linux-gnu/obs-modules/plugins/loudness-balance-monitor.so
/usr/local/share/obs/obs-modules/plugins/loudness-balance-monitor/locale/
```

OBS33.0.1の`frontend/cmake/feature-plugin-manager.cmake:3–9`がprefixから探索先を生成し、`frontend/plugin-manager/PluginModuleLoader_Linux.cpp:116–123`が新形式を読み込む。公式実行ファイルの埋込み文字列でも上記パスを確認した。`libobs/obs-module.c:803–820`の列挙方法に合わせ、ライブラリをディレクトリ直下に配置する。

## macOSの結果と未確認範囲

通常build/packageのpkgを、非rootのrunnerユーザーへinstallerで導入した。導入先はユーザーのLibrary/Application Support/obs-studio/plugins。導入済みbundleのファイルとハッシュをパッケージ生成元と照合し、公式OBSの通常探索でロードした。公式OBS同梱のQt・libobs・frontend APIを使用し、開発用依存先からのロードがないことを確認した。

- [初回](../logs/2026-11-06-obs33-macos-setup.json)
- [再起動](../logs/2026-11-06-obs33-macos-restore.json)
- [非表示復元](../logs/2026-11-06-obs33-macos-restore-hidden.json)
- [導入・署名の診断](../logs/2026-11-06-obs33-macos-installation.json)

両CPUスライスの生成とarm64での実行を確認した。バイナリの最低OS指定は13.0。Intelと最低OSでの実行は未検証であり、公開対応範囲の確定に別途反映する。

pkgは未署名、bundleはadhoc署名、公証は未実施。Gatekeeperの評価はno usable signatureで拒否された。CLI導入とOBSロードの成功を、ブラウザー経由の通常配布やGatekeeperの通過へ読み替えない。署名・公証と公開条件は製品の最終配布物検証で確定する。bundle/package識別子がテンプレートのcom.example.plugintemplate-for-obsのままである点も公開前に解消する。receipt診断では記録を取得できず、今回の導入証拠は正常終了と導入済みファイル照合を用いる。
## 検証環境

| OS | ビルド環境 | OBS |
| --- | --- | --- |
| Windows x64 | VS2026、SDK26100、OBS deps/Qt2026-10-23 | 公式33.0.1 ZIP |
| macOS Universal | Xcode/SDK26.5、OBS deps/Qt2026-10-23 | 公式33.0.1 Apple DMG、arm64ランナー |
| Ubuntu x86_64 | Ubuntu26.04、公式deb同梱ヘッダー・ライブラリ | 公式33.0.1 deb |

旧版から候補版への更新と、録音・試聴の製品統合後の検証は別工程であり、本報告の基準確認に含めない。
