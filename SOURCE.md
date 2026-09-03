# ソースコードの入手

*[English version / 英語版はこちら](SOURCE.en.md)*

Blender は GNU GPL なので、**このビルドに対応するソースコードを受け取る権利が
あなたにあります**(有料・無料を問いません)。正確なライセンス条文は同梱の `license/` が正本です。

## 対応するソース

- ベース: Blender `v5.2.1` (`9e2066aef7e`)
- 改変部分: 上記に Falcon 系のコミットを重ねたもの
- このビルドのコミット: `bab756c6a9f`(公開スナップショット `aac1ac037cc` = tag `v0.4-r2`)

## 入手方法

**このビルドと同じ場所に `falcon-render-v0.4-src.tar.xz` を置いてあります。**
ビルドをダウンロードした人は、誰でもそちらも取得できます。

```bash
tar xf falcon-render-v0.4-src.tar.xz
```

中身はビルド時点のスナップショットです。開発中のコミット履歴は含みません
（GPL が求めているのは「このバイナリに対応するソース」であって、開発の過程ではないため）。

`lib/`（事前ビルド済みの外部ライブラリ群）は含まれていません。これは Blender 公式の
ソース配布と同じ形です。ビルドするには別途取得してください:

```bash
git submodule update --init --checkout lib/linux_x64
```

## ソースは GitHub でも公開しています

**https://github.com/Relrei/Falcon-Render**(この repo の `main`・tag `v0.4-r2`)
> 2026-09-04: ソースはこの repo に同居させました(`main` = Blender 5.2.1 ベースのスナップショット系列)。旧 `blender-cyclesf-falcon` は archive 済み(読み取り専用・履歴は残っています)。


このビルドに対応するソースは、同梱の `falcon-render-v0.4-src.tar.xz` と
**タグ `v0.4`**(GitHub)のどちらでも同じものが取れます。

ソースを公開しているのは、GPL が最低限そう求めているからではありません
(GPL が求めるのは**バイナリを受け取った人へ渡すこと**だけです)。
**公開しておくべきものだと考えているからです。**

## 仕組みの解説

コードそのものより、**何をどう考えて作ったか**の解説を別に書いています。
そちらの方が読んで面白いはずです（Patreon の記事一覧を見てください）。

## ビルド方法

```
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_CYCLES_DEVICE_OPTIX=ON \
  -DWITH_CYCLES_CUDA_BINARIES=ON \
  -DCYCLES_CUDA_BINARIES_ARCH=sm_86 \
  -DOPTIX_ROOT_DIR=<OptiX SDK のパス> \
  -DWITH_FALCON_SHARC=ON \
  -DFALCON_BUILD_FLAVOR=release \
  -DFALCON_DIST=ON \
  ..
ninja -j6
```

- `-DWITH_FALCON_SHARC=ON` が要ります(既定 OFF)
- `-DFALCON_DIST=ON` を付けると `Falcon Render v0.4` と名乗ります。
  付けない場合は版を名乗らない `Falcon Render`(開発者の手元の最新)になります
- `-DFALCON_BUILD_FLAVOR=release` は GPU の実験機能をビルドから外します(配布物はこちら)
- CUDA カーネルを触った場合は `touch intern/cycles/kernel/device/cuda/kernel.cu` してから
  ビルドしないと再コンパイルされません
- 並列数は 6 程度に抑えてください(それ以上はメモリを使い切ります)
