# DLSS ランタイムライブラリについて

このパッケージには NVIDIA DLSS のランタイムライブラリ
(`libnvidia-ngx-dlssd.so` / `libnvidia-ngx-dldenoiser.so`)を同梱しています。
入手元は NVIDIA 公式 DLSS SDK(<https://github.com/NVIDIA/DLSS>)の
`lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.7.0` で、改変は加えていません
(SHA256 で一致を確認済み)。

## 実行時の結びつき方(GPLとの関係の根拠)

このライブラリは `blender` 本体にリンクされていません。ビルド時に参照するのは
関数のシグネチャを得るためのヘッダファイルだけで、ビルド生成物(`blender`
バイナリ)を `ldd` しても NVIDIA のライブラリへの参照は一切出てきません。

実際の結びつきは**実行時の `dlopen()` のみ**です。`blender` はまずドライバ側の
`libnvidia-ngx.so.1` を `dlopen` し、そこから NVIDIA のランタイムが
`libnvidia-ngx-dlssd.so` を独自に読み込みます。これは OptiX や CUDA を
Blender 本体が扱う仕組みと同じ形で、プラグインを実行時に差し込む構造です。
静的リンクも、ソースコードの取り込みもありません。

## NVIDIA RTX SDKs License について

同梱ライブラリは NVIDIA RTX SDKs License(v. January 23, 2023)のもとで配布します。
全文は本ファイルと同じ場所の `license/NVIDIA-RTX-SDKs-License.txt` を参照してください。

要点:

- ソースコードの改変・派生物には「This software contains source code provided by
  NVIDIA Corporation.」の表示が必要(本アプリはソースを含まないバイナリ配布のため
  直接は該当しないが、明記しておく)
- アプリケーションは SDK 単体を超える実質的な機能を持つ必要がある
  (本ビルドは Blender フォーク全体であり該当しない)
- **GPLとの兼ね合い**: ライセンスには「SDKをオープンソースライセンスの対象と
  なるような形で使ってはならない」という条項がある。上記のとおり実行時
  `dlopen` のみで静的リンクもソース取り込みも無いため、単なる同梱
  (mere aggregation)であり GPL の結合著作物には当たらないと判断している。
  最終的な法的判断ではない

## 未完了の義務

- **NVIDIA への事前通知**: DLSS SDK を組み込んだアプリケーションの商用リリース前に、
  <https://developer.nvidia.com/sw-notification> から通知する義務がある
  (会社名/発行者名/開発者名、使用SDK、アプリ名、対象プラットフォーム、
  出荷予定日、製品/動画へのリンクを記載)。**未実施**
- **NVIDIA Marks の表示**: スプラッシュ画面とアバウトボックスへの NVIDIA商標の
  表示が求められている(ライセンス6.1(b))。表示にはNVIDIA商標の使用ガイドライン
  順守と、使用サンプルの事前提出(実施2週間前)が必要。**未実施**

この2点が終わるまでは、公開版に同梱した状態を確定させない。
