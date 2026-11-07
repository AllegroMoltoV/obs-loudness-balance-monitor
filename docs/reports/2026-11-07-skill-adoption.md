# 開発スキルの導入と適用範囲

2026-11-07、利用者指定の[shokai/agent-skills](https://github.com/shokai/agent-skills/tree/2cf4d099c66afe9aa2811f0348ff1c544b0459e2)を確認し、次の3件をCodexのスキル領域へ導入した。製品コード、OBS設定、既存テストは変更していない。現在の計画はBeadsのobs-loudness-balance-monitor-udyを正本とする。

| スキル | 採用した内容 | 適用上の制約 |
|---|---|---|
| software-test | 守る境界を先に定め、実装の写しを避ける。失敗を通すためだけの変更をしない | 実利用E2Eと音声・数値の独立照合を維持する。正常系という分類だけで削除しない |
| orchestrator | 代表1系列の成立後に独立作業を並行化し、成果物で報告を確認する | Beadsを進捗の正本とし、再開時に実物を照合する。共有OBS・ADAT・同一ファイルの競合を避ける |
| user-decision | 判断材料を先に示し、仕様選択を実行承認へ埋め込まない | 既存承認を繰り返し求めない。手動操作は必要な工程へ到達してから依頼する |

導入先は `C:/Users/scher/.codex/skills/`。公式skill-installerでrevisionを固定して導入した。各SKILL.mdのSHA-256は次のとおり。

- software-test: `7235B1E25A6FB265A5A7BD65E61A23ED6C375E60A27F1E1A5BD9603B52C14B03`
- orchestrator: `5704AD7481ECD505167CDE5FF428FC74621D1A0B3A61924E4FCA2F95BDC31985`
- user-decision: `5A6C1203287A8F853A6A618FCB6692AE0F4BF2EB563FEA49733540F11865927A`

Software Factoryモード全体は採用していない。Claude専用の前提、相談の反復、別の対話記録、追加の外部投稿を既存の開発工程へ持ち込む必要がないためである。コードコメントとGitHub文書のスキルも確認したが、既存ルールと重なるため今回の導入対象を広げなかった。

## 既存検証との照合

読取り専用調査では、T1〜T5に重大な過剰検証を示す根拠は見つからなかった。T1の再録音、T2の画面、T5の音声を同じ代表録音で観測する構成へ計画を改めた。既存試験の一括削除は行っていない。

- `.github/tests/preview-analysis.cpp:82`の内部系列の再平均は内部整合の検査である。独立OBSトラックを取得する`.github/baseline/preview-numeric-probe.h:36`と、数値・VAD・評価を照合する`analyze-preview-numeric.py:43`とは保証が異なる。
- 独立参照`preview-analysis-reference.cpp:24`は48kHz・固定設定で同じlibebur128を使用する。取得経路と集計の照合に有用だが、ライブラリ自体の正しさや44.1kHzの全系列を証明しない。
- `preview-preferences-probe.h:32`の復元前観測を再利用できる。新しいdefaultの保存・復元と、新規手動OFFの確認を既存の更新試験へまとめる。
- `preview-position.cpp:42`の位置・終了検査では、OS既定デバイスの解決先を確認できない。追加機能に必要な接続先観測を残す。
- `analyze-preview.py:134`以降の末尾・余分な音声・不連続検査は音声出力の保証である。API成功や画面表示で代用しない。

この調査は既存計画の反証の再実施ではなく、スキル適用に伴う検証範囲の照合である。製品テストや音声再生は実行していない。

## 公開前提の確認

`buildspec.json:36`にテンプレートのbundleIdが残っている。`docs/reports/2026-11-06-obs33-baseline.md:54`には未署名・未公証とGatekeeper拒否の記録がある。過去の資産の記録を最新候補の結果へ読み替えず、配布前提の調査を再試用改善と独立して先行する構成へ計画を改めた。

親課題と.9/.6/.7/.8を更新した。修正実装の承認待ちを維持し、最終成果物検証後のRelease、Release確認後のForum更新という依存順を保持した。
