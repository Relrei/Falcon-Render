# Falcon Render v0.4

Blender 5.2.1 LTS ベース。**この版の主題は動画編集(VSE)です。**
v0.3 はレンダリング側の修正が中心でしたが、v0.4 では Blender 内蔵の
ビデオシーケンサ(VSE)を実用速度まで持ち上げました。

起動時バージョン表示: `Falcon Render v0.4`

---

## ★ 動画編集(VSE)が速くなりました

### 1. 切っただけの書き出しを、復号も符号化もせずに通す(fast path)

素材を切って並べただけのタイムラインを書き出すとき、従来は
**全コマを復号してから全コマを符号化し直して**いました。
つなぎ目以外は元の映像そのままなので、そこは**パケットをそのまま流す**ようにしました。
外部 `ffmpeg` の `-c copy` と同じことを、Blender の書き出しの中でやります。

240 コマ・同じ台で3回ずつ・最小値(2026-08-31 実測):

| 解像度 | fast path | 従来 | |
|---|---|---|---|
| 720p | 1.42 秒 | 7.01 秒 | 4.9 倍 |
| **FHD** | **2.27 秒** | **15.47 秒** | **6.8 倍** |
| **WQHD** | **2.86 秒** | **27.24 秒** | **9.5 倍** |
| 4K | 5.09 秒 | 59.68 秒 | 11.7 倍 |

- **倍率は解像度で決まります。**従来の書き出しは画素数にほぼ比例しますが、
  コピーは画素数と無関係だからです。数字を見るときは解像度を必ず添えてください。
- コピーした区間は**元の素材とビット単位で一致**します(240 コマ中 240 コマで確認)。
  つなぎ目だけが焼き直され、そこは通常の書き出しと同じ較正で符号化されます。
- 音は素材から運ばず、**既存のミックスダウンに作らせます**(音ズレの経路を増やさない)。
- **条件が合わないときは黙って通常の書き出しへ降ります。**降りる条件:
  出力 codec が素材と違う / 可変フレームレート / インターレース /
  回転メタデータ付き(スマートフォン撮影) / HDR / エフェクトやモディファイアが付いている。
  H.264・HEVC・VP9・AV1 に対応。29.97fps は通ります。
- 戻す口: `FALCON_VSE_FASTPATH=0`

### 2. 再生が 5.6 倍

先読みデコードを入れました。再生時のコマあたり **52.2ms → 9.40ms(106fps 相当)**。
H.264 の GPU デコードでも 2.40 倍です。
先読みの本数は `BL_VSE_PREFETCH_N`(既定 8)で変えられます。

> PNG 連番だけは変わりません。そちらは保存側(1 コマ 230ms)が律速で、
> 先読みしても待ち時間が移動するだけだからです。

### 3. 色補正が 3.8 倍

明度・コントラスト等のストリップモディファイアを自動ベクトル化が効く形に書き直しました。
BRIGHT_CONTRAST 単体で **15.82ms → 4.14ms/コマ**、5 個積んだ状態で 1.26 倍。
**出力画素は全 10 組で従来と完全一致**(maxdiff = 0)です。速いだけで、絵は変わりません。

### 4. プレビュー解像度がモディファイアに効くようになりました

従来、ストリップモディファイアはプレビュー解像度を下げても**費用が変わりません**でした
(480x270 表示でも 4K のまま計算していた)。縮小してから掛けるように直したので、
プレビュー 25% で **13 倍**軽くなります。

### 5. GPU 動画エンコード(NVENC)

動画の書き出しで NVIDIA GPU の符号化器を使えるようにし、**既定で有効**にしました。
**使う前に一度開いてみて、開けなければ自動で CPU 側へ降ります。**
GPU で符号化できない組み合わせ(10bit の一部・極端に小さい解像度・
品質 BEST / REALTIME など)でも、**書き出しが失敗することはありません。**

### 6. メモリを持っていかなくなりました

VSE のキャッシュは設定値そのものは守っていましたが、
**機械の空きメモリを誰も見ていません**でした。空きが尽きかけたら手放すようにしました。
常時メモリ圧力をかけた状態で、ピーク RSS **2450MB → 1443MB**。
**出力画素は maxdiff 0** で変わりません。
下限は既定 1024MB、戻す口は `FALCON_VSE_MEM_FLOOR_MB=0`。

### 7. 画像の保存をレンダーに重ねる

連番書き出しで、1 コマの保存が終わるのを待たずに次のコマのレンダーへ進みます。
EXR(マルチレイヤー含む)の保存は **230ms → 42ms** 相当まで隠れます。

### 8. タイムラインの上下を反転する口

チャンネル 1 を上に置く並びに切り替えられます。既定は従来どおり(オフ)。
`FALCON_VSE_FLIP_CHANNELS=1` で有効。

### 9. レンダーした物を、そのまま VSE へ渡す(同梱アドオン・既定で有効)

**レンダーの出力先に出来た物を、同じシーンの Sequencer へ自動で足します。**
連番画像なら Image ストリップ、動画なら Movie ストリップとして 1 本入ります。
アドオン `Falcon VSE Bridge` は**同梱されていて、最初から有効**です
(アドオンの一覧に出さない「中核」扱いなので、有効化の操作は要りません)。

- **出力プロパティ > 出力** に「**出力名**」の欄が増えます。
  ここに `shot01` と入れると、出力先のフォルダに `shot010001.png` … と書きます。
  連番の桁と拡張子は Blender の規則のままです。**書き出しの間だけ差し替え、
  終わったら必ず元へ戻す**ので、`.blend` の出力先は書き換わりません。
- 同じ欄の「**出力先を VSE に共有**」(既定 ON)で、レンダーが終わった時に
  自動で Sequencer へ入ります。同じファイルを撮り直すと**増えずに差し替わり**、
  チャンネル(段)はそのまま引き継ぎます。
- **「VSE で編集」ボタン**(出力プロパティ / レンダー結果を映している画像エディタの
  ヘッダ)で、その場で足して Video Editing へ移ります。
- **Sequencer の「追加」メニューに「レンダー出力」**が増えます。
  Video Editing のワークスペース側からも呼べます。

> ⚠ Blender 5.x の Sequencer は、ワークスペースごとに「どのシーンを編集するか」
> (ヘッダのシーン選択)を持っています。**ここが空だと、ストリップを足しても
> Video Editing は空っぽに見えます。**このアドオンは、まだ何も選んでいない
> ワークスペースをレンダー後にそのシーンへ向けます(既に選んである所は触りません)。

---

## レンダリング側の変更

### 土台を Blender 5.2.1 LTS へ

v0.3 は 5.2.0 LTS でした。上流の 5.2.1(160 コミット・323 ファイル)を取り込んでいます。

### レンダー領域でカメラカリングを絞る

「レンダー領域」を有効にしているとき、従来のカメラカリングは
**常に画面全体**と比べていたため、領域の外のオブジェクトも読み込まれていました。
領域に従うようにし、パネル(簡略化 > カリング)から ON/OFF できるようにしました。

> ⚠ **領域の外に光源があると、その照明ごと消えます。**
> 既定の余白(margin = 1.0)では何も変わらないので、既存のファイルは黙って変わりません。

### Vulkan: GPU メモリ不足で落ちない

ビューポートで GPU メモリの確保に失敗したときに異常終了していたのを直しました。

### 最終レンダー中はビューポート側の GPU メモリを退避(既定 ON)

---

## 標準版との差分(v0.3 からの追加ぶんのみ)

| 機能 | 内容 |
|---|---|
| VSE fast path | 切っただけの書き出しを再符号化せずに通す。FHD 6.8 倍 / WQHD 9.5 倍 |
| VSE 先読みデコード | 再生 5.6 倍(52.2ms → 9.40ms/コマ) |
| VSE 色補正の高速化 | 3.8 倍。出力画素は従来と完全一致 |
| VSE プレビュー縮小 | モディファイアをプレビュー解像度で掛ける。25% で 13 倍 |
| GPU 動画エンコード | NVENC。開けない組み合わせは自動で CPU へ降りる |
| VSE メモリ圧力の解放 | 空きメモリが尽きかけたらキャッシュを手放す。ピーク 2450 → 1443MB |
| 保存とレンダーの重ね | 連番書き出しで保存待ちを隠す。EXR 230 → 42ms |
| **VSE ブリッジ**(同梱アドオン) | レンダーした連番/動画をそのまま Sequencer へ。「出力名」の欄つき。既定で有効 |
| レンダー領域カリング | カメラカリングをレンダー領域に従わせる |

---

## 導入

```bash
tar xf falcon-render-v0.4-linux-x86_64.tar.xz
cd falcon-render-v0.4-linux-x86_64
./falcon-render.sh
```

`falcon-render.sh` から起動してください(直接 `./blender` でも動きますが、
OptiX キャッシュの設定が入りません。詳細は README を参照)。

---

## 既知の問題

`KNOWN_ISSUES.md` を参照してください。v0.4 で新しく分かっているもの:

- **fast path はコピー区間だけがビット一致**です。つなぎ目の焼き直し区間は
  再符号化されるので、そこだけ画質が元素材と異なります(crf 16 相当)。
- **fast path の出力は従来よりわずかに容量が大きくなります**(コピー区間が素材そのままのため)。
- DNxHD での書き出しが失敗することがあります。これは **素の Blender 5.2 でも同じ**で、
  上流側の既存の不具合です。

---

# English

# Falcon Render v0.4

Based on Blender 5.2.1 LTS. **This release is about video editing (VSE).**
v0.3 was mostly render-side fixes; v0.4 brings Blender's built-in Video Sequence
Editor up to a usable speed.

Startup version string: `Falcon Render v0.4`

---

## ★ Video editing (VSE) is faster

### 1. Cut-only exports pass through without decode or encode (fast path)

When you export a timeline that only cuts and arranges source clips, Blender used to
**decode every frame and re-encode every frame**. Everything except the joins is the
original video, so those parts now **stream the original packets through unchanged** —
the same thing external `ffmpeg -c copy` does, but inside Blender's own export.

240 frames, same machine, best of 3 (measured 2026-08-31):

| Resolution | fast path | before | |
|---|---|---|---|
| 720p | 1.42 s | 7.01 s | 4.9x |
| **FHD** | **2.27 s** | **15.47 s** | **6.8x** |
| **WQHD** | **2.86 s** | **27.24 s** | **9.5x** |
| 4K | 5.09 s | 59.68 s | 11.7x |

- **The speedup depends on the resolution.** A normal export scales roughly with pixel
  count; copying does not. Always quote the resolution along with the number.
- Copied ranges are **bit-identical to the source** (verified on 240 of 240 frames).
  Only the joins are re-encoded, using the same calibration as a normal export.
- Audio is not carried from the source; it is produced by the **existing mixdown path**
  (so no new way for audio to drift).
- **If the conditions do not hold, it silently falls back to the normal export.**
  It falls back when: the output codec differs from the source / variable frame rate /
  interlaced / rotation metadata (phone footage) / HDR / any effect or modifier is applied.
  H.264, HEVC, VP9 and AV1 are supported. 29.97fps passes.
- Off switch: `FALCON_VSE_FASTPATH=0`

### 2. Playback is 5.6x faster

Prefetching decode. Per-frame playback cost **52.2ms → 9.40ms** (≈106fps).
2.40x even with H.264 GPU decoding. Depth is `BL_VSE_PREFETCH_N` (default 8).

> PNG image sequences are unchanged: there the 230ms-per-frame *save* is the bottleneck,
> so prefetching only moves the wait.

### 3. Color correction is 3.8x faster

The color strip modifiers were rewritten so auto-vectorization applies.
BRIGHT_CONTRAST alone: **15.82ms → 4.14ms per frame**; 1.26x with five stacked.
**Output pixels are identical to before in all 10 test pairs** (maxdiff = 0).

### 4. Preview resolution now affects strip modifiers

Strip modifiers used to **cost the same regardless of preview resolution** (a 480x270
preview still ran them at 4K). They are now applied after downscaling, so a 25% preview
is **13x cheaper**.

### 5. GPU video encoding (NVENC)

Video export can use the NVIDIA encoder, and it is **on by default**.
**It opens the encoder once before using it and falls back to CPU if that fails**, so
combinations the GPU cannot encode (some 10-bit, very small resolutions, BEST / REALTIME
quality) **never make the export fail**.

### 6. It no longer eats your RAM

The VSE caches honoured their configured limits, but **nothing was looking at the
machine's free memory**. They now release when free memory runs low.
Under constant memory pressure, peak RSS went **2450MB → 1443MB**, with
**maxdiff 0** on the output pixels. Floor defaults to 1024MB;
off switch `FALCON_VSE_MEM_FLOOR_MB=0`.

### 7. Image saving overlaps with rendering

In sequence exports, rendering the next frame no longer waits for the previous frame to
finish saving. EXR (including multilayer) saving hides down to about **230ms → 42ms**.

### 8. Option to flip the timeline vertically

Channel 1 on top. Off by default; enable with `FALCON_VSE_FLIP_CHANNELS=1`.

### 9. Renders go straight into the VSE (bundled add-on, enabled by default)

**Whatever the render wrote to the output path is added to the same scene's Sequencer**
— an Image strip for a frame sequence, a Movie strip for a video. The add-on
`Falcon VSE Bridge` is **bundled and enabled out of the box** (it is a "core" add-on, so
there is nothing to switch on).

- **Output Properties > Output** gains an **Output Name** field. Type `shot01` and the
  render writes `shot010001.png` … into the same folder. Frame digits and extension
  follow Blender's own rules. The path is **swapped only for the duration of the render
  and always restored**, so your `.blend`'s output path is never rewritten.
- **Share output with the VSE** (on by default) adds the result when the render finishes.
  Re-rendering the same files **replaces** the strip instead of stacking a new one, and
  keeps its channel.
- **"VSE で編集" button** in Output Properties and in the Image Editor header while it
  shows a render result: adds the strip and switches to Video Editing.
- The Sequencer's **Add menu gains "レンダー出力"**, so it is reachable from inside the
  Video Editing workspace too.

> ⚠ In Blender 5.x the Sequencer edits a scene chosen **per workspace** (the scene
> selector in its header). **If that is empty, Video Editing looks empty even when the
> strips exist.** This add-on points workspaces that have not chosen one yet at the
> rendered scene; workspaces that already have a choice are left alone.

---

## Render-side changes

### Base moved to Blender 5.2.1 LTS

v0.3 was 5.2.0 LTS. Upstream 5.2.1 (160 commits, 323 files) is merged in.

### Camera culling follows the Render Region

With Render Region enabled, camera culling still compared against the **whole frame**,
so objects outside the region were still loaded. It now follows the region, and can be
toggled from the panel (Simplify > Culling).

> ⚠ **A light outside the region is culled together with its lighting.**
> With the default margin (1.0) nothing changes, so existing files are unaffected.

### Vulkan: no longer crashes when GPU memory allocation fails

### Viewport GPU memory is evicted during final renders (on by default)

---

## Installing

```bash
tar xf falcon-render-v0.4-linux-x86_64.tar.xz
cd falcon-render-v0.4-linux-x86_64
./falcon-render.sh
```

---

## Known issues

See `KNOWN_ISSUES.en.md`. New in v0.4:

- **Only the copied ranges of a fast-path export are bit-identical.** The re-encoded
  joins differ from the source (roughly crf 16).
- **Fast-path output is slightly larger** than a normal export, because copied ranges
  keep the source bitrate.
- DNxHD export can fail. This happens in **stock Blender 5.2 as well** — it is an
  existing upstream bug.
