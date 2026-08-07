# 既知の不具合

現在調査中、または未解決の不具合をここにまとめます。
再現条件・切り分け状況は分かっている範囲で更新していきます。

## DLSS レンダリングでサンプル数が指定値の何倍も消費される (#2, #3)

- **症状**: DLSS(DLSS-RR)を有効にしてレンダリングすると、設定でどのようなサンプル数を
  指定しても、実際に消費されるサンプル数が指定値を大幅に超過する。`preview_samples` と
  `samples` を揃えても再現し、他のパラメータの組み合わせを変えても消費量の倍率傾向は改善しない
- **報告日**: 2026-08-07
- **状態**: 原因を実機検証で確認済み。恒久対応(コード修正)は未着手
- **重要な再現条件**: **静止画では発生しない。連番(アニメーション)レンダーに切り替えた
  途端に確実に発生する。** ビューポートの「ナビ履歴持ち越し」をON/OFFして事前に試した
  際は改善しなかった(後述の通り別プロパティだったため無関係)

### 実機検証結果(2026-08-07、classroomシーンで確認)

2フレームのみの連番レンダー(16spp、DLSS-RR)で `use_persistent_data` の有無を比較。

| 条件 | 所要時間 |
|---|---:|
| Persistent Data OFF(既定・バグ再現条件) | 20.8 秒 |
| Persistent Data ON | 12.4 秒 |

比率 1.68倍。理論値(1枚目は必ずコールドスタートで5倍、2枚目以降がPersistent Data
の有無で温存されるかどうか: OFF=10B, ON=6B → 比率1.67)とほぼ完全に一致し、下記の
仮説が実測で裏付けられた。145フレームの本番シーンではフレーム数に比例してこの差が
開き、理論上ON/OFF比は最大約4.9倍まで拡大する計算になる。

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

**当面の回避策(実測で効果確認済み)**: レンダープロパティ → パフォーマンス →
**Persistent DataをON**にしてから連番レンダーする。Sessionがフレーム間で維持され、
`dlss_history_cold_`が2フレーム目以降`false`のままになり、毎フレームの5倍消費が
止まる。ただし回避策であり、根本原因(Persistent Data OFF時の想定外のSession再構築)
自体の修正ではない。

**注意**: 事前に試した「ナビ履歴持ち越し」(`preview_denoising_carry_history`)は
**ビューポート専用のプロパティ**で、この連番レンダーの問題とは別物
(こちらは`denoising_carry_history`、UI表記「履歴持ち越し」、デフォルトON)。
別プロパティを触っていた可能性が高く、Persistent Dataの方はまだ試されていない。

### 別に確認していた仮説(ビューポートRENDERED表示、優先度は連番問題より低い)

`RenderScheduler::get_num_samples()`は、ビューポートでDLSS使用時かつ
`preview_denoising_carry_history`(デフォルトOFF)の場合、指定サンプル数を無視して
`Integrator::MAX_SAMPLES`を返す。これは今回の連番レンダー問題とは別の経路。

### 設計上の透明性の問題(別課題、実装は保留)

根本原因の修正以前に、そもそも `FALCON_DLSS_PREROLL` はUIのどこにも露出しておらず、
純粋に環境変数のみで制御されている。`denoising_carry_history`のようなCyclesプロパティ
にすらなっていないため、ユーザーはサンプル数が5倍消費される理由を知る手段がない。

- 指定サンプル数を無視して5倍消費すること自体より、**それがUIから一切分からないこと**
  が実用上の問題として大きい
- 改善案: `FALCON_DLSS_PREROLL` をCyclesのRNAプロパティとして露出する(既存の
  `denoising_carry_history` と同じ扱い)。進捗表示の作り込みまではせず、まずは
  「この値が存在し、変更できる」ことがUIから分かる状態にするだけでも十分
- ビルド・検証コストが重いため、実装は現時点で保留。着手する場合はここに記録して進める

### 未実施の切り分け項目

- [x] Persistent DataをONにして連番レンダーを実機検証 — classroomシーンで確認済み(上記表)
- [ ] 根本原因の恒久修正(Persistent Data OFF時でもフレーム間でDLSS-RR履歴を保持する
      か、`dlss_history_cold_`の判定をSession再構築に依存しない形にする)
- [ ] `FALCON_DLSS_PREROLL=0` で連番レンダーの倍数消費が消えるか確認(Persistent Data
      を使えない場合の代替策として)
- [ ] ビューポートで「ナビ履歴持ち越し」ONの実機再検証(連番問題とは別件として)
- [ ] adaptive sampling を OFF にした場合の挙動
- [ ] 他のデノイザー(OIDN 等)との比較

### 注記

このリポジトリはビルド配布・ドキュメント用であり、実際にレンダリングを行う
Cycles / DLSS-RR 統合部分のソースコードは含まれていません(詳細は [SOURCE.md](SOURCE.md) を
参照)。そのため、本問題の原因調査・実装修正は、`falcon-render-v0.1-src.tar.xz` を展開した
別リポジトリ(ローカル)側で行う必要があります。このリポジトリ側では、症状の記録と
切り分け状況の追跡のみを行います。
