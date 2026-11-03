# macOS/Linux有限PCM再生の隔離試作

## 目的と範囲

固定5秒のfloat32 little-endianステレオPCMを、明示した出力デバイスへ一度だけ再生する独立CLIである。OBS内部APIへのリンク、録音、製品ドックへの組込み、自動再試行を含まない。入力を固定した後の再生終端と取消しを検証するための準備物であり、製品の動作保証ではない。

## 内容物

- `main.cpp`: 44,100 Hzまたは48,000 Hz、正確に5秒、有限値かつ絶対値1以下のPCMを検査する。`--play`と明示デバイスが必須である。
- `playback.h`: 共通の呼出し境界と終了コードを定義する。
- `playback-macos.cpp`: AudioQueueへ最大4,096 framesの3バッファを供給する。入力末尾の短いバッファも投入する。
- `playback-pulse.cpp`: libpulseで明示sinkへ接続し、最大4,096 framesずつ供給する。
- `CMakeLists.txt`: 3OS共通録音試験とmacOS/Linux出力試作の独立ビルド定義である。Windowsでは共通試験だけを実行し、WASAPI出力試験は別の実機記録で管理する。

## ビルドと実行

macOSにはC++17コンパイラー、CMake、Apple SDKが必要である。LinuxにはC++17コンパイラー、CMake、pkg-config、libpulseの開発ヘッダーとライブラリー、接続可能なPulseAudio互換サーバーが必要である。環境導入を自動実行しない。

以下は対象OSでの準備用コマンドである。本作業では実行していない。プロジェクトルートから構成する。

```sh
cmake -S tests/preview -B tests/preview/build
cmake --build tests/preview/build
```

再生前に通常配信の停止と対象出力を確定する。別の物理出力は要求しない。デバイス引数にはmacOSの出力UID、Linuxの正式なsink名を渡す。`default`と`@`で始まる別名を拒否する。次のパスと識別子は実環境の値へ置換する。

```sh
tests/preview/build/lbm-preview --play /absolute/path/five-seconds.raw 48000 explicit-device-uid-or-sink
```

末尾へ整数の`cancel-ms`を追加すると、再生処理開始から指定時間後に取消しへ進む。例えば`1000`が1秒、`0`が開始前取消しとなる。接続準備時間を含まない。SIGINT/SIGTERMも取消しを要求する。

終了コード0はAPI上の自然終端、2は失敗、3は取消しである。環境不在、無効デバイス、期限超過、清掃処理失敗を成功に変換しない。各実行で新しいプロセスを起動するため、同一インスタンスの再利用を試験していない。

## 終端と停止

macOSでは指定UIDの出力能力と生存状態を確認してAudioQueueへ設定する。コールバックを呼出し元run loopで実行し、固定PCMを順に投入する。全PCM投入後に`AudioQueueStop(false)`を要求し、`kAudioQueueProperty_IsRunning`の停止を確認した後に破棄する。取消しと失敗時には供給を停止し、`AudioQueueStop(true)`と同期Disposeへ進む。再生中に指定デバイスのUID・生存状態・出力能力を再確認する。

Linuxでは`PA_STREAM_DONT_MOVE`と正式sink名で接続し、実際の接続名を照合する。`prebuf=0`で短い末尾を待機させず、全PCM提出後の`pa_stream_drain`コールバック成功を自然終端の条件にする。取消しでは未完了operationを取消し、cork、flush、disconnectへ進む。サーバーやstreamの失敗、sink名の変更を失敗として扱う。休止中sinkの自動復帰を妨げないため、休止表示だけで即時失敗にしない。

Linuxの接続待機期限は10秒、再生処理の監視期限は20秒、個別operation待機は3秒である。取消し後のcorkとflushはそれぞれ最大3秒を待つ。macOSの再生処理にも20秒の監視期限を設ける。これらは試作が待機し続ける状況を検出する監視値であり、製品性能要件ではない。OSの同期API内部の停止やスケジューリング遅延を強制中断する期限ではない。

## 検証状況と残る制約

ソースの読戻しと有限終端・取消し・デバイス消失・待機期限の机上レビューを実施した。macOS/Linuxでのコンパイル、実再生、再収録、端数一致、デバイス切断、取消し、停止直後の再試行は未実施である。利用可能なMac実機を確認できていない。WSL Ubuntuは存在するがC++コンパイラーとlibpulse開発環境を確認できていない。サーバーとデバイスの存在を未確認のまま成功扱いしない。

実機検証では44,100 Hzと48,000 Hzの自然終端、取消し、無効デバイス、再生途中の切断、同一入力の反復を測定する。各OSの戻りPCMを別経路で捕捉し、末尾欠落・重複・停止後残音・次回録音への混入を確認する。API上の停止完了だけでは機器の残音消失や、次回録音への混入防止を保証しない。macOSのデバイス監視はポーリングであり、切断直後の経路変化を即時検知する保証もない。出力先固定と消失時動作の実測が必要である。

PCMの最大振幅を1以下としているが、聴取音量の安全性を保証する制限ではない。検証用PCMの振幅と機器側音量を別途決定する。試作はPCMを減衰・加工しない。

## APIとソースの参照

API呼出しの構成を参照して独立実装した。外部の関数本文やサンプルコードを転載していない。

- ローカルOBS 33.0.1: `.deps/obs-studio-33.0.1/libobs/audio-monitoring/osx/coreaudio-output.c`、`coreaudio-enum-devices.c`、`.deps/obs-studio-33.0.1/libobs/audio-monitoring/pulse/pulseaudio-output.c`。
- [Apple Audio Queue Services Programming Guide: Playing Audio](https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/AudioQueueProgrammingGuide/AQPlayback/PlayingAudio.html): バッファ供給、非同期停止、破棄の手順。
- [PulseAudio stream.h公式API](https://www.freedesktop.org/software/pulseaudio/doxygen/stream_8h.html): 明示sinkへの接続、write、drain、cork、flush、disconnect。
- `PA_STREAM_DONT_MOVE`の使用例: `.deps/obs-studio-33.0.1/plugins/linux-pulseaudio/pulse-input.c:367`。

