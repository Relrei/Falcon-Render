# Falcon CyclesF — Blender 5.1 fork (record)

Blender v5.1.2 の Cycles に、コースティクス/分散まわりの実験実装を足したフォークの記録。
ベースは shallow クローンのため、初コミットは v5.1.2 のスナップショット(履歴なし)。
Upstream: https://projects.blender.org/blender/blender.git

## 主な追加

- **GPU フォトンマップ(点ベース・Round 9)** — 光子ベイク→点バッファ+近傍グリッド gather。
  レンダー時ノブ(半径/ゲイン/法線角)で再ベイク不要の調整。SUN/AREA/SPOT 対応。
- **波長分散** — Cauchy B ノブでガラス屈折とコースティクスの虹が自動(`FALCON_DISPERSION_B`)。
- **ライトトレーシング(LT)** — キャッシュレスの収束するコースティクス。
  絶対較正済(PT 参照比 1.0007)・1/√N 収束・スプラットぼかし・可視性レイ・
  パネル1ボタンのライト毎 LT パス+ビューティ加算合成。
- **SHARC 放射輝度キャッシュ移植** ほか。

## 場所

- カーネル: `intern/cycles/kernel/integrator/falcon_*.h`
- アドオン/パネル: `intern/cycles/blender/addon/`(falcon_photon.py, properties/operators/ui)
- 設計・進捗の正本: リポジトリ直下の `FALCON_LIGHTTRACE.md` / `FALCON_PHOTON.md` /
  `FALCON_DISPERSION.md` / `FALCON_EXPERIMENT.md`

## ライセンス

Blender 本体と同じ GPL-2.0-or-later(追加コードも同ライセンス)。
