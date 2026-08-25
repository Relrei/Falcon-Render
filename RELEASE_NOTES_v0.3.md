**Falcon Render v0.3 beta は無料のデモ版です。機能の制限はありません。**
以降の版は有料で配布する予定です。Linux x86_64 / NVIDIA GPU 向け。

*English below / 英語は下にあります*

---

## ★ v0.2 で GPU レンダーが使えなかった不具合を直しました

v0.2 の配布物には、GPU カーネルの組み立てに必要なヘッダが 2 つ入っていませんでした。
GPU のカーネルは**実行時に**同梱ソースを読んでコンパイルするため、**ビルドも CPU レンダーも
通り、GPU レンダーだけが失敗します**。v0.2 を GPU で使えなかった方は、こちらが原因です。

再発を止めるため、配布したソースだけでカーネルが閉じているかを確かめる検査を
パッケージ作成の手順に入れ、**通らなければパッケージを作らない**ようにしました。

## 他人の `.blend` を開いたときに踏める 3 件を塞ぎました

| | v0.2 | v0.3 |
|---|---|---|
| 任意のファイルが 1 GiB で上書きされる | 踏める | 塞いだ |
| 確保より大きく読む | 踏める | 撥ねて完走 |
| 異常な半径で戻ってこなくなる | 素通り | 切り詰め |

**正常系は変えていません。**同じシーンのコースティクス出力がバイト単位で一致し、
速度は 0.963〜1.021 倍でした。

## ライトトレースの不具合を 2 件直しました

- **コンポジターを組んだシーンで画面全体が明るくなる** — 層をファイル経由で読み戻していて、
  読んでいたのがコンポジターを通した後の絵でした。修正後は空 1.0000 / 壁 1.0002 / 屋根 1.0000
- **光源に大きさを与えると画面全体に下駄が乗る** — 光子がランプ自身に当たった放射を
  フィルムへ書いていました。修正後は全画素の下駄 +0.078 → −0.0009

## リメッシュに「目標の面数」

面数を直接指定できるようにしました。実測の誤差は **0.23% / −0.08% / 0.01%**。
adaptivity との併用、スカルプトのマスクの尊重、**UV と頂点カラーが落ちていた不具合の修正**を含みます。

## スカルプト表示の高速化

| | v0.2 | v0.3 |
|---|---|---|
| 位置バッファの作り直し(400 万面) | 6.90 ms | **1.59 ms** |
| 1 回に送る量 | 30.4 MB | **8.0 MB** |
| 表示全体(200 万面) | 51.2 ms | **42.6 ms** |

## 導入

~~~bash
tar xf falcon-render-v0.3-beta-linux-x86_64.tar.xz
cd falcon-render-v0.3-beta-linux-x86_64
./falcon-render.sh
~~~

`falcon-render.sh` から起動してください。OptiX の初回コンパイル待ちと、
DLSS の更新チェックによるネットワーク待機を回避します。

## 弱点について

**2026-08-25 に参照を撮り直し、数字を全部計算し直しました。**
測定条件と限界は [WEAKNESSES.md](WEAKNESSES.md)、未解決の不具合は [KNOWN_ISSUES.md](KNOWN_ISSUES.md) に
書いてあります。**買ってから気づくより先に読めるようにしてあります。**

## ソースコード

GPL のため対応ソースを同梱しています(`falcon-render-v0.3-beta-src.tar.xz`・コミット `5eb3ab668e4`)。

## DLSS について

DLSS-RR はオプション機能で、既定では無効です。**v0.2 と同じく、署名済みライブラリは同梱していません。**
正規に入手した `libnvidia-ngx-dlssd.so` を展開先の直下に置いた場合にだけ、デノイザーの選択肢に出ます。

---
---

# English

**Falcon Render v0.3 beta is a free demo release. There are no feature limits.**
Later versions are planned to be paid. For Linux x86_64 / NVIDIA GPU.

## ★ Fixed: GPU rendering did not work in v0.2

The v0.2 package was missing two headers needed to assemble the GPU kernels.
GPU kernels are compiled **at run time** from the bundled sources, so **the build passed,
CPU rendering passed, and only GPU rendering failed.** If you could not use v0.2 on the GPU,
this was why.

To stop it happening again, a check that the bundled sources are self-contained is now part of
the packaging procedure, and **the package is not built if that check fails**.

## Closed three issues reachable by opening someone else's `.blend`

| | v0.2 | v0.3 |
|---|---|---|
| An arbitrary file is overwritten with 1 GiB | reachable | closed |
| Reads past the allocation | reachable | rejected, render completes |
| An out-of-range radius never returns | passed straight through | clamped |

**Normal operation is unchanged.** Caustics output for the same scene matches byte for byte,
and speed was 0.963–1.021x.

## Fixed two light-tracing defects

- **Whole frame brightened in scenes with a compositor node tree** — the layer was read back
  through a file, and what was read was the image *after* the scene compositor.
  After the fix: sky 1.0000 / wall 1.0002 / roof 1.0000
- **A pedestal across the whole frame when the light is given a size** — the emission from a
  photon hitting the lamp itself was written to the film.
  After the fix: whole-frame pedestal +0.078 → −0.0009

## Remesh target face count

You can now specify the face count directly. Measured error: **0.23% / −0.08% / 0.01%**.
Includes use together with adaptivity, respecting the sculpt mask, and **a fix for UVs and
vertex colors being dropped**.

## Faster sculpt display

| | v0.2 | v0.3 |
|---|---|---|
| Rebuilding the position buffer (4 M faces) | 6.90 ms | **1.59 ms** |
| Data uploaded per stroke step | 30.4 MB | **8.0 MB** |
| Whole display (2 M faces) | 51.2 ms | **42.6 ms** |

## Installing

~~~bash
tar xf falcon-render-v0.3-beta-linux-x86_64.tar.xz
cd falcon-render-v0.3-beta-linux-x86_64
./falcon-render.sh
~~~

Start it through `falcon-render.sh`. It avoids the first-run OptiX compile stall and the network
wait from the DLSS update check.

## About the weaknesses

**References were re-shot on 2026-08-25 and every number recomputed.**
Measurement conditions and limits are in [WEAKNESSES.en.md](WEAKNESSES.en.md); unresolved defects
are in [KNOWN_ISSUES.en.md](KNOWN_ISSUES.en.md). **They are there to be read before you download,
not discovered afterwards.**

## Source code

The corresponding source is included, as GPL requires
(`falcon-render-v0.3-beta-src.tar.xz`, commit `5eb3ab668e4`).

## About DLSS

DLSS-RR is optional and disabled by default. **As in v0.2, the signed library is not bundled.**
It appears in the denoiser list only if you place a legitimately obtained
`libnvidia-ngx-dlssd.so` directly in the extracted folder.
