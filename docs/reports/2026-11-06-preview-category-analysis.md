# 録音試聴のカテゴリー解析の成立性

## 対象と承認

2026-11-06、利用者がBeads親課題obs-loudness-balance-monitor-udyの第3.1節と第3.2節を承認した。表示対象は声とBGMのリアルタイムLUFSと平均LUFS、および各行の既存3段階評価である。本書は取得経路の調査結果を記録する。計画の正本はBeadsに置く。

## ソース取得

OBS 33.0.1のlibobs/obs-source.c:1609-1696では、capture callbackへ元のdataを渡す。内部コピーに適用するtiming_adjust、sync_offset、resample_offsetをcallbackのtimestampから直接取得できない。このcallbackと最終mixのtimestampを同一視しない。

同ファイル:5531-5547の公開APIは処理済みPCMとtimestampを返す。ただしraw callback時点ではdiscard_audioがtimestampを変更済みである。raw callbackからこの2値を読むだけでは同じ区間の対にならない。

候補は非rootのprivate音声入力ソースにaudio_renderを設定し、処理済みソースを観測してfalseを返す方式である。OBS_SOURCE_AUDIOのみを指定する。OBS_SOURCE_COMPOSITEとの併用は禁止される。

libobs/obs-audio.c:574-618でviewのactive treeが残余音声ソースより先にrender_orderへ入る。観測用ソースをsceneやviewへ追加せず、子ソースも列挙しなければ、既存active treeの後に観測できる。false返却によりaudio_ts=0、audio_pending=trueとなり、min_ts算出とroot mixへ寄与しない。

成立範囲は現在のviewのactive treeに含まれる選択ソースである。obs_source_activeだけをtree所属の保証にしない。観測後のbuffering判定で不採用になる区間があるため、最終raw callbackを受けた区間だけを確定する。scene itemの演出音量はソースPCMの取得後に適用されるため、個々のソースPCMと最終寄与量を区別する。

## 再生位置

送信済み量は再生位置ではない。共通表現を開始からの秒とし、元録音のレートに対応付ける。

- Windows: IAudioClock::GetPositionとGetFrequencyを使う。位置を周波数で割る。レート変換後のフレーム数を直接使わない。
- macOS: AudioQueueGetCurrentTimeの有効なsample timeをキューのレートで割る。timeline discontinuityを検出する。
- PulseAudio: PA_STREAM_AUTO_TIMING_UPDATEとpa_stream_get_timeを使う。初回の時刻情報未取得を位置0への巻戻しにしない。

取得失敗をGUI経過時間や送信量へ置換しない。開始・停止・正常終了と位置の有効性を別々に扱う。各APIの実行時の精度と末尾挙動は未検証。

一次資料:

- [Microsoft IAudioClock::GetPosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)
- [Apple AudioQueueGetCurrentTime](https://developer.apple.com/documentation/audiotoolbox/audioqueuegetcurrenttime(_:_:_:_:))
- [PulseAudio Audio Streams](https://freedesktop.org/software/pulseaudio/doxygen/streams.html)

## 隔離試作

検証用ヘッダーは.github/baseline/preview-category-probe.h。製品コードを変更せず、既存の隔離OBSと合成入力を使う。通常OBS設定とTotalMixを変更せず、試聴出力を開始しない。最終mixと観測したカテゴリー音声を時刻で対応付ける。48kHzの初回試作は不合格となり、後段を停止した。モジュールのビルド成功後、隔離OBS33.0.1でBGMの同期オフセット37msを設定して9秒間捕捉した。比較対象の内部6秒間で286,976フレームの合成値が最終mixと完全一致したが、1,024フレームの声の観測が未確定だった。閾値を緩めず、44.1kHzの実行と製品実装を停止した。

保存済み全データを読み取ると、raw packet 91、timestamp 87,803,619,165,000ns、raw開始から約1.941333秒の区間が該当する。render packet 93では声のtimestampが0、BGMが同区間のtimestampだった。当該rawの全2,048個のfloat値がBGMのPCMと完全一致した。全体無音ではなく、声の寄与が0でBGMが鳴っている区間である。

初回試作でaudio_pendingとtimestamp0を記録し分けていなかったため、正当な非出力と観測漏れを区別できなかった。利用者の追加承認を受け、同じ条件で観測時とraw確定時のpending、render周期番号を記録した。OBSが出力に採用しなかったソースを欠落扱いしていたことを確認した。無条件のゼロ埋めではなく、同じ周期の最終pendingに基づく非出力と未観測を判定する処理へ修正した。

証拠は基準worktree .deps/obs33-baseline内の次の場所にある。

- .logs/2026110682-category-build.log
- .logs/2026110683-category-48000.log
- .deps/preview-category-48000/result/category-result.json
- 同result内category-render-blocks.bin、category-raw-blocks.bin、CSV、比較PCM
- .deps/preview-category-48000/config/obs-studio/logs/2026-11-06 20-02-42.txt

初回試作のOBSは正常終了した。この段階で通常環境への導入、ADAT出力、製品コード変更は行っていない。

## 修正後の検証と製品接続

| 検証 | 結果 | 証拠 |
|---|---|---|
| 状態記録を追加した同条件測定 | 最終pendingを使う独立再解析で不明0、最大差0 | logs86〜87、preview-category-states-48000 |
| 48kHzの修正後試作 | 288,000フレーム、欠落0、最大差0 | logs88〜89、preview-category-cycle-48000 |
| 44.1kHzの修正後試作 | 264,600フレーム、欠落0、最大差0 | log90、preview-category-cycle-44100 |
| 声1本・BGM2本、37ms/53ms同期オフセット | 288,000フレーム、欠落0、最大差0 | logs91〜92、preview-category-multiple-48000 |
| Windows製品ビルド | 成功 | log94 |
| 製品UIから6秒録音・自動試聴 | リアルタイムと平均の声−16.7 LUFS、BGM−22.7 LUFS、目標8 LUに対し黄 | log96、preview-analysis-48000-analysis/result/result.json |
| ADAT5/6出力照合 | 承認済み先頭10msの立上りを除き一致、末尾欠落なし | log97、同result/audio-audit.json |
| 解析の境界試験 | 成功。短時間・無音、既知振幅比、3色、入力分割非依存、resetを確認 | logs98〜100 |

証拠ディレクトリは基準worktreeの.deps、ログは.logs/20261106を接頭辞とする。
製品候補のWindows DLL SHA256はA7A2FD2D92C79A79B8621D61EFE3B95DD163724E3E5ED3BA1FCE58A978CEAD3E。作業ツリーの未コミット候補である。

製品では既存のソース選択・VAD・libebur128・録音状態管理を再利用した。カテゴリー合成用の観測はprivate AUDIO sourceから行い、最終PCMへ書き込まない。音声callbackから固定キューへ渡し、workerで3秒Short-termと100ms間隔の系列を計算する。VADは10msのサンプル境界で更新する。平均は両値有効かつ発声中の同一時点の算術平均である。再生位置はOS音声APIから取得し、GUIの経過時間へ置換しない。

Linuxの製品・検証モジュール・解析単体テストのビルドと解析試験はlog105で成功した。ただし実行スクリプト末尾のdate引数にCRが残り、コンテナ全体の終了コードは1だった。ビルド・試験の成功とスクリプト全体の終了を区別する。

44.1kHzの製品UIによる6秒録音・自動試聴も成功し、声−16.7 LUFS、BGM−22.7 LUFS、両評価が黄となった。ADAT5/6照合も成功した (logs106〜107)。プロファイル切替後のトラック4自動選択、無効設定・複数トラック時の案内と手動復帰を確認した。操作試験と独立PCM照合が成功した (logs109〜110、preview-feedback-profile-track-reviewed/result)。

状態切替から1秒後の英語待機画面では、狭いドックの説明と解析行が正常に折り返された。同試験のpreview-waiting-settled-01.pngを目視確認した。切替直後の画像だけで恒常的な見切れと断定しない。日本語待機画面の同条件確認は未完了。

## 解析失敗と録音失敗の分離

追加実装で、声・BGMの未選択や解析失敗からfail/cancelへ進み、有効な録音まで破棄する問題を確認した。利用者が2026-11-06に分離案を承認したため修正した。カテゴリー解析の不成立では解析結果を公開せず、未評価と理由を表示する。最終ミックスの録音が正常なら自動試聴・再試聴を維持する。音声取得自体の失敗は従来どおり停止する。

解析準備の失敗と録音途中の解析失敗をそれぞれ確認した。既存の隔離OBSと製品UIによる録音・自動試聴を使い、最終mixの独立PCMとADAT5/6出力を照合した。

| 検証 | 結果 | 証拠 |
|---|---|---|
| 声を未選択にして録音 | 自動試聴・自然終了・モニター復元、未評価と未選択理由を確認。音声照合成功 | logs112/114 |
| 録音中に選択BGMをシーンで非表示 | 解析不成立後も録音・自動試聴を維持。未評価と解析不成立の理由を確認。音声照合成功 | logs115/116 |
| 正常な選択で6秒録音 | リアルタイム・平均と黄評価を維持。録音・自動試聴・音声照合成功 | logs117/118 |

Windowsビルドはlogs111/113で成功した。最初の未選択試験で理由が重複表示され、重複追加コードを削除した。後続の途中失敗ケースで理由が1回だけ表示された。変更後のDLL SHA256はDA3AF44BC5D9D091328B5EBAE3CB3E0C38160AEAF3B4AEBCF75DEF1FC4F08937。ビルド後のソース変更は解析worker部分の字下げだけである。git diff --checkも成功した。今回の修正はWindowsの隔離OBSで検証済みであり、他OSでの同修正の実行確認は未完了。

## 連続操作とLinux再生位置の検証

Windowsの既存連続操作試験へ解析結果の観測を追加した。初回の検証器は自然終了後の未評価表示を再生中の不具合として判定した。再生中だけを検査し、必要な観測の存在を終了時に確認する条件へ修正した。失敗証拠はlog120、修正後の操作とPCM照合はlogs122/123である。

再試聴中に声の選択を解除しても、録音時の解析結果を保持した。選択を戻し、BGM音量を0.5倍にして録り直すと、声の平均−16.7 LUFSを維持し、BGM平均が−22.7から−28.7 LUFS、平均評価が黄から緑へ変わった。再試聴と録り直しの全出力を独立PCMと照合して成功した。これは選択固定と結果置換の証拠であり、全系列の独立数値照合を代替しない。

Linuxでは修正版の製品・検証モジュールのビルドと解析単体試験が成功した (log124)。既存コンテナにPulseAudio実行環境がなかったため、隔離コンテナ内へpulseaudio/pulseaudio-utilsを導入した。ホストOSの音声設定を変更していない。

製品のPulseAudio出力実装を直接使うpreview-position.cppで、仮想sinkへの4秒再生と途中取消を44.1/48kHzで確認した (log125)。位置の取得・単調増加・自然終了時の4秒・取消時の未完了位置は全条件で成功した。ただし位置の更新粒度に問題がある。48kHz通常再生の報告位置は1.27811→2.55873→3.85013秒と飛び、次の通知まで古い値を保持する。検証器の基本合格を表示同期の合格に読み替えない。

同コンテナの公式ヘッダーstream.h:679-707では、pa_stream_get_timeが新値を返すためにtiming infoの更新が必要とされる。現実装はPA_STREAM_AUTO_TIMING_UPDATEのみを使い、明示更新要求も補間もない。従ってLinuxの試聴中LUFS表示が古い位置を参照する問題であり、音声データの欠落を示すものではない。平均は録音全体から確定済みで、この位置更新問題の影響を受けない。

この結果で一度後段を停止した。その後、利用者が用途に応じた小さな表示ずれを許容し、PulseAudioの標準補間を使う案を承認した。厳密な同期保証は追加しない。現実装はPA_STREAM_AUTO_TIMING_UPDATEとPA_STREAM_INTERPOLATE_TIMINGを併用する。独自の補間処理や固定遅延の加算、100msごとの更新要求管理は追加しない。

独立調査では残る数値照合に、隔離OBSの別トラックへ声とBGMカテゴリーを分離し、OBS自身のraw出力を基準PCMにする案を確認した。声の有音・休止・再開と、同位相BGM2本の重なりを1本のシナリオに集約できる。確定した解析だけを返す読取り専用recordedAnalysisプロパティを実装した。この調査は同目的の計画反証の再実施ではなく、残存検証の準備に限る。

## 標準補間の実行結果

基準worktreeの.logs/20261106126-position-linux-interpolated.logで、44.1/48kHzそれぞれ通常再生→途中取消→再度の通常再生を確認した。各呼出しは独立した検証プロセスであり、製品UIの同一セッション再試聴の証拠とは区別する。全6ケースが成功し、通常終了位置4秒、取消位置約1.008秒を確認した。位置は各再生で先頭付近から進み、単調増加した。約10ms間隔の観測で、報告位置の最大変化幅は44.1kHz通常再生の初回で27.374ms、それ以外で11.124〜11.428msだった。以前の約1.28秒分の飛びは観測されなかった。この値は位置更新の滑らかさを表し、音声に対する同期誤差の上限ではない。

修正後のLinux製品ビルドとgit diff --checkが成功した。コンテナの終了コードは0。音声出力方式は維持し、タイミングの標準補間フラグだけを追加した。今回の停止理由は解消済みである。

## 独立数値照合と日本語画面

声の有音・休止・再開と、BGM2本の部分的重なりを含む48kHzの12秒録音を製品UIから実行した。OBSが別トラックへ合成した声/BGMを独立PCMとして採取した。参照計算器は製品の解析・VAD関数を呼ばず、libebur128の原語、独立した発声判定・平均・評価計算を使う。全120点、平均対象56点でLUFS最大差0.0、発声判定・各点評価・平均値・平均評価も一致した。同じ操作で録音した最終mixとADAT5/6出力の照合も成功した。証拠はlogs127〜130と.deps/preview-analysis-numeric-48000-analysis-numeric/result。

日本語待機のsettled-00〜03.pngを目視した。幅232px、viewport220px、96DPI、倍率1で案内末尾・解析行・ボタン・出力・ソース・下部設定を確認し、文字の欠けを認めなかった。既存の英語確認と5セクションのキーボードヘルプ検証を再利用する。

このWindows候補のDLL SHA256はC38396EDA2E2D68550575646330B4FA557096110DEA7C6AE9B64C9BC56592595。macOS実行検証を既存3OS CIへ追加した。現runnerのCoreAudio出力在庫を調べ、Apple Virtual Sound DeviceのUIDを一意選択し、現製品のAudioQueue処理で44.1/48kHzの通常再生・取消と位置更新を確認する。物理デバイスの証拠とは区別する。診断用実行器を配布pkgへ含めない。

## 候補CIと通常環境への導入

候補d00bcc42e3d16c445a6b1069fafdec69193615d8の[CI42224723250](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/actions/runs/42224723250)はWindows・macOS・Ubuntuで全成功した。macOSは現runnerのApple Virtual Sound Device (UID AVIODevice 2)で44.1/48kHzの通常再生・取消・位置更新が全成功した。これは仮想デバイスによるOS音声API実行の証拠で、物理出力波形との一致保証ではない。

Windows CIの通常配布ZIPからDLLとlocaleを通常環境へ導入し、DLL SHA256 7EE597B9208721112E08ADDB1BE33394F2C25E28D832C421F024F0F9B1A7500Aを照合した。ローカルE2E候補と同じソースコミットのCIビルドである。CIではOBS33.0.1への導入・設定とドック表示復元・メーター動作も成功している。既存設定を保持した。

更新直前のプラグインと設定は.appendix/20261106-preview-analysis-trialへ退避し、manifest.json・trial-guide.md・restore.mdを用意した。通常OBSの起動はWindowsの許可画面で待機中。通常環境での新DLLロード・画面確認と利用者再試用は未完了であり、過去の起動ログを今回の成功証拠として使わない。公開は未実施。


