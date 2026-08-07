# 既知の不具合

現在調査中、または未解決の不具合をここにまとめます。
再現条件・切り分け状況は分かっている範囲で更新していきます。

## DLSS 連番レンダーでサンプル数が指定値の何倍も消費される (#2, #3) — 修正済み

- **症状**: DLSS-RR有効時、連番(アニメーション)レンダーで指定サンプル数を大幅に超過して
  消費し続ける。静止画では発生しない
- **原因**: `intern/cycles/blender/session.cpp`。Persistent Data(既定OFF)がOFFの場合、
  アニメーションの毎フレームで`BlenderSession`(と内部の`Session`/`RenderScheduler`)が
  丸ごと破棄・再構築される。DLSS-RRの「最初のフレームだけ5倍サンプルで時間履歴を温める」
  仕組み(`get_dlss_preroll_passes()`)の初回判定フラグ`dlss_history_cold_`が、この
  再構築のたびに初期値`true`へ戻るため、**全フレームが「初回」扱いされ毎回5倍消費**する
- **修正**: `BlenderSession::render()`完了時に「このジョブでDLSS-RR履歴は温まった」と
  記録し、次にSessionが再構築された際その記録を渡して`dlss_history_cold_`を`false`に
  戻す。記録場所は`BlenderSession`のインスタンスメンバではなく`static`メンバ
  (`dlss_history_warmed_this_job`)にする必要がある。`BlenderSession`自体が毎フレーム
  作り直されるため、インスタンスメンバでは値が生き残らない(最初にこれで一度失敗した)
- **実機検証(classroomシーン、2フレーム、Persistent Data OFF)**:
  修正前 20.8〜24.4秒 → 修正後 14.5秒(8spp)。デバッグログでフレーム2の
  `warmed_flag`が`0`→`1`に変わったことも確認済み
- **対象コミット**: `intern/cycles/integrator/render_scheduler.{h,cpp}`,
  `intern/cycles/session/session.{h,cpp}`, `intern/cycles/blender/session.{h,cpp}`
  (Blender-5.2.C2-dlss、ローカルソース側)

## メモ

- `FALCON_DLSS_PREROLL` はUI未露出(環境変数のみ)。着手する時に検討
- ビューポート(RENDERED表示)側で`preview_denoising_carry_history`(既定OFF)が
  `get_num_samples()`を`MAX_SAMPLES`に差し替える件は、今回の連番問題とは別経路。
  未対応
- デバッグ用ログ(`FALCON_DEBUG_LIFECYCLE`環境変数で有効化、BlenderSession/Sessionの
  生成・破棄・warmed_flagの値を出力)はソースに残置。次回同系統の調査で再利用可能

## 注記

このリポジトリはビルド配布・ドキュメント用であり、実際にレンダリングを行う
Cycles / DLSS-RR 統合部分のソースコードは含まれていません(詳細は [SOURCE.md](SOURCE.md) を
参照)。そのため、本問題の原因調査・実装修正は、`falcon-render-v0.1-src.tar.xz` を展開した
別リポジトリ(ローカル)側で行う必要があります。このリポジトリ側では、症状の記録と
切り分け状況の追跡のみを行います。
