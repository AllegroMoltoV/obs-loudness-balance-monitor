# OBS Forum の AI 利用開示要件

確認日: 2026-09-22

## 結論

[Forum Resource and IP Policy](https://obsproject.com/forum/threads/forum-resource-and-ip-policy.178569/) は、AI または LLM をコード、画像、その他の制作に使用したリソースに対し、使用したツールと用途をリソース説明へ記載するよう求めている。同ポリシーの `disclaimer` は、損害賠償責任を制限する免責条項ではなく、AI 利用の開示文を指す。

公式の定型文は掲載されていない。リソース編集画面にも AI 専用欄はなく、通常の `Description` 欄へ開示文を追加する。公開中の承認例では、見出しと文章が統一されていない。この差異により、固定文言より、実際の利用状況を具体的に説明する点が審査対象と判断できる。

現行ポリシーには、AI コーディングツールで全体または大部分を制作したリソースを許可しない旨が残っている。同じ節には、AI 利用を全面禁止せず、開示内容を踏まえて承認を判断する旨もある。2026-07-29 の最終編集後に公開されたリソースの中には、AI がコードの大部分を記述した事実を開示した掲載例もある。今回の管理者メッセージも、AI への依存度を指摘したうえで、開示追加後の再審査を案内している。

以上から、禁止文言を無効と扱う根拠はない一方、AI の関与率だけで自動的に却下する運用とも断定できない。現行の実務は、利用実態、作者の関与、検証内容を開示させたうえで個別審査する運用と判断できる。開示文の追加だけでは承認を保証せず、最終判断は OBS Forum のモデレーションチームに属する。

## 適用時期

AI 利用開示の義務は 2026-01-20 にポリシーへ追加され、2026-07-12 に説明と参照先が補強された。Loudness Balance Monitor の初回公開日は 2026-01-05 であり、当初の公開は義務化より前だった。今回の更新は義務化後に提出されている。

[Resource Security Incident](https://obsproject.com/forum/threads/resource-security-incident.194555/) によると、2026-03-09 以降は既存リソースの更新も手動審査の対象である。現在の再審査依頼は、この時系列と一致する。

## 開示文に必要な内容

最低限、次の事実を英語で記載する。

- 使用した AI ツール名と提供元
- AI を使用した対象: ソースコード、テスト、ビルド設定、文書、リソース説明、画像など
- 使用方法と関与の程度: 補助、個別修正、広範な実装支援など
- 人間が担当した要件決定、レビュー、動作確認の範囲

ポリシーには、利用率、プロンプト履歴、モデルの版、コード行数の記載要件がない。確認できない人間レビューを記載せず、実施済みの試験と手動確認を具体的に示す方が正確である。日本語と英語の説明を併記する現在の構成では、開示文も両言語で併記すると読者へ同じ情報を提供できる。

## Loudness Balance Monitor への適用

2026-09-19 以降の作業記録では、OpenAI Codex が次の作業を支援した。

- macOS の依存関係と CI ビルドの調査・修正
- 設定保存先の作成、シーンコレクション切替時のソース参照解放、ドック表示状態復元の実装
- 自動検査、隔離版 OBS の検証手順、ログ解析、公開手順の作成
- README、GitHub Release、OBS Forum 説明、issue 返信文案の作成と改稿

利用者は Windows の隔離版 OBS 32.1.2 で、設定保存、音声表示、シーンコレクション切替、ドックの表示・非表示復元を手動確認した。Windows、macOS、Ubuntu の CI ビルドも成功している。詳細は [OBS 32.1.2 の音声動作](2026-09-20-obs-32-audio.md)、[ドック表示状態](2026-09-20-dock-visibility.md)、[v0.1.3 の配布結果](2026-09-20-v013-publish.md)にある。

利用者への確認により、v0.1.0 までの制作におけるほぼ全過程で Claude Code を使用し、その後の修正におけるほぼ全過程で OpenAI Codex を使用した事実が確定した。利用者本人による確認はユーザーテストであり、コード読解による厳密な人間レビューは実施していない。リソースアイコンは Claude Code に SVG を出力させて制作した。概要説明欄の GIF は利用者本人が ScreenToGif で画面収録し、AI は使用していない。

## 対応結果

2026-09-22 に、3 件の README と OBS Forum 説明へ日英の AI 利用開示を追加した。README は [Loudness Balance Monitor PR #8](https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/pull/8)、[Simple Drop Shadow PR #2](https://github.com/AllegroMoltoV/obs-simple-drop-shadow/pull/2)、[Simple Pitch Shift PR #2](https://github.com/AllegroMoltoV/obs-simple-pitchshift/pull/2)で `main` へ反映した。3 件の `main` ruleset は、承認要件を含む規則を保持した `active` 状態である。

OBS Forum の保存結果と一般公開状態は次のとおりである。

- Loudness Balance Monitor: 所有者画面で開示文と `Awaiting approval before being displayed publicly.` を確認した。ログイン情報を付けない取得ではリソースページが 404 となり、一般公開前である。管理者 DM へ開示追加と再審査依頼を送信済みで、管理者からの追加返信はない。
- Simple Drop Shadow: 開示文が一般公開ページに表示されている。ログイン情報を付けない取得でも日本語と英語の開示文を確認した。
- Simple Pitch Shift: 所有者画面で開示文と `Awaiting approval before being displayed publicly.` を確認した。ログイン情報を付けない取得ではリソースページが 404 となり、一般公開前である。

Loudness Balance Monitor の v0.1.2 更新文もモデレーター承認待ちであり、v0.1.3 の版履歴と更新文は未登録である。Loudness Balance Monitor と Simple Pitch Shift の公開結果、および Loudness Balance Monitor の版履歴更新は、モデレーター判断後に確認する。

## 文章構造

次の英文は公式テンプレートではなく、必要事項を欠落させないための構造例である。角括弧内は確認済みの事実へ置換する。

```text
AI disclosure

OpenAI Codex [and other tools, if applicable] was used [extent] during development. It assisted with [specific code areas], automated tests, build and release tooling, and documentation drafts, including revisions to this resource description. The maintainer defined the requirements, directed the work, [state the exact review scope], and validated the released build through [specific automated and manual tests]. [State any AI use in images or other assets.]
```

## 公開例

- [SceneAnchor](https://obsproject.com/forum/resources/sceneanchor.2666/) は Claude Code の利用、説明文への利用、作者によるレビュー・ビルド・Windows 実機確認を記載している。
- [Lumetric Corrector 3.0.0](https://obsproject.com/forum/resources/lumetric-corrector.2137/updates) は、Anthropic Claude と OpenAI Codex、対象となったコード・シェーダー・テスト・文書、保守者による試験を列挙している。
- [Halsu plugins for streaming](https://obsproject.com/forum/resources/halsu-plugins-for-streaming.2374/) は、人間が作成した中核部分と AI を使用した追加機能・定型コード・ビルド基盤を区別している。
- [IKANDY Music Visualizer](https://obsproject.com/forum/resources/ikandy-music-visualizer-for-obs-window-capture-free-spout-with-pro.2713/) は、Claude Code と Codex がコードとテストの大部分を担当した事実を開示している。ポリシーの最終編集後に公開された掲載例であり、AI の関与率だけで公開可否が自動決定されない実務を示す。ただし、個別審査の判断基準までは公開されていない。

これらは公開済みリソースの実例であり、OBS Forum が配布する公式テンプレートではない。

## 関連する公式方針

[OBS Studio Contribution Guidelines](https://github.com/obsproject/obs-studio/blob/master/CONTRIBUTING.md#aimachine-learning-policy) は、OBS 本体への提出物に対して、コード、説明、issue、コメントを人間が記述する方針を示している。Forum ポリシーはこの文書を背景説明として参照する一方、Forum リソースには AI 利用の開示を要求している。両文書の対象を混同せず、Forum 掲載では Forum Resource and IP Policy を直接の要件として扱う。
