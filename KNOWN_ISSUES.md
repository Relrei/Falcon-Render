# 既知の不具合

現在調査中、または未解決の不具合をここにまとめます。
再現条件・切り分け状況は分かっている範囲で更新していきます。

## DLSS レンダリングでサンプル数が指定値の何倍も消費される (#2, #3)

- **症状**: DLSS(DLSS-RR)を有効にしてレンダリングすると、設定でどのようなサンプル数を
  指定しても、実際に消費されるサンプル数が指定値を大幅に超過する。`preview_samples` と
  `samples` を揃えても再現し、他のパラメータの組み合わせを変えても消費量の倍率傾向は改善しない
- **報告日**: 2026-08-07
- **状態**: 有力な原因仮説あり(ソースコード調査ベース)。実機再検証は未実施
- **重要な再現条件(2026-08-07 追加報告)**: **静止画では発生しない。連番(アニメーション)
  レンダーに切り替えた途端に確実に発生する。** ビューポートの「ナビ履歴持ち越し」を
  ON/OFFして事前に試した際は改善しなかった(ただしこれは後述の通り別プロパティの可能性が高い)

### 最有力仮説: 連番レンダーで毎フレーム発生する原因(ソース: `intern/cycles/blender/session.cpp`)

`BlenderSession::bake` 相当のフレーム同期処理に以下がある。

```cpp
if ((this->b_render->mode & blender::R_PERSISTENT_DATA) == 0)
{
  if (!is_new_session) {
    free_session();
    create_session();
  }
  return;
}
```

**「Persistent Data」(レンダープロパティ→パフォーマンス、Blenderのデフォルトは OFF)が
OFFの場合、アニメーションの各フレームでCycles Sessionが丸ごと破棄・再生成される。**
`RenderScheduler`はSessionのメンバであり、破棄されればフレームごとに再構築される。

`render_scheduler.h` の `dlss_history_cold_`(初回フレーム判定フラグ、初期値`true`)は
`reset()`内で一度`false`にされる以外に戻す処理が無い。Sessionがフレームごとに
再構築されると、このフラグも毎回`true`に戻る。

`get_dlss_preroll_passes()` は「アニメーションの最初のフレームだけ、DLSS-RRの時間
履歴を温めるために同じフレームを5回(既定`FALCON_DLSS_PREROLL=4`+本番1回)レンダー
する」設計だが、**Persistent DataがOFFだと「最初のフレーム」判定が全フレームで
成立してしまい、連番の全フレームで5倍のサンプルを消費する。**

- 静止画(1フレームのみ)で問題が出ない: `is_animation_`がfalseのため
  `get_dlss_preroll_passes()`が早期リターンで0を返す(そもそも対象外)
- 連番だと確実に発生する: 上記の通りフレームごとに条件が成立し直すため
- サンプル数設定を変えても改善しない: 原因がサンプル数と無関係(何倍になるかは
  `FALCON_DLSS_PREROLL`の値で決まる)なため

**想定される対処(未検証)**: レンダープロパティ → パフォーマンス → **Persistent Data
をON**にしてから連番レンダーする。Sessionがフレーム間で維持されれば
`dlss_history_cold_`は2フレーム目以降`false`のままになり、毎フレームの5倍消費が
止まるはず。

**注意**: 事前に試した「ナビ履歴持ち越し」(`preview_denoising_carry_history`)は
**ビューポート専用のプロパティ**で、この連番レンダーの問題とは別物
(こちらは`denoising_carry_history`、UI表記「履歴持ち越し」、デフォルトON)。
別プロパティを触っていた可能性が高く、Persistent Dataの方はまだ試されていない。

### 別に確認していた仮説(ビューポートRENDERED表示、優先度は連番問題より低い)

`RenderScheduler::get_num_samples()`は、ビューポートでDLSS使用時かつ
`preview_denoising_carry_history`(デフォルトOFF)の場合、指定サンプル数を無視して
`Integrator::MAX_SAMPLES`を返す。これは今回の連番レンダー問題とは別の経路。

### 未実施の切り分け項目

- [ ] **Persistent DataをONにして連番レンダーを実機検証** — 最優先。毎フレームの
      5倍消費が止まるか確認する
- [ ] `FALCON_DLSS_PREROLL=0` で連番レンダーの倍数消費が消えるか確認(Persistent Data
      が効かなかった場合の切り分け)
- [ ] ビューポートで「ナビ履歴持ち越し」ONの実機再検証(連番問題とは別件として)
- [ ] adaptive sampling を OFF にした場合の挙動
- [ ] 他のデノイザー(OIDN 等)との比較

### 注記

このリポジトリはビルド配布・ドキュメント用であり、実際にレンダリングを行う
Cycles / DLSS-RR 統合部分のソースコードは含まれていません(詳細は [SOURCE.md](SOURCE.md) を
参照)。そのため、本問題の原因調査・実装修正は、`falcon-render-v0.1-src.tar.xz` を展開した
別リポジトリ(ローカル)側で行う必要があります。このリポジトリ側では、症状の記録と
切り分け状況の追跡のみを行います。
