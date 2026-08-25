#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""falcon_inspect_blend.py -- 見知らぬ他人から受け取った .blend の中身を、
開いて信用する前に「何が実行されうるか / 何が仕込まれているか」で見せる検査器。

なぜ要るか
----------
このビルドは WITH_PYTHON_SECURITY=ON で、.blend 内の Python 自動実行は既定オフ。
それでも二つ穴が残る:

  1. 開く前に中身が見えない。自動実行がオフでも「このファイルに何が入っているか」
     を利用者が知る手段が無い。Blender の警告バナーは小さく、反射で消される。
  2. ★Python だけが実行経路ではない。このフォークが足した Falcon プロパティは
     PropertyGroup の IDProperty なので、.blend 読み込み時に RNA の min/max が
     当て直されない = 全部が無検証の外部入力。実測で、値ひとつで任意ファイルが
     1GiB 破壊された(A-1)。「自動実行オフ」は Python しか守らない。

この検査器は上の両方を一枚の表にして出す。素の Blender には無い 2番目
(Falcon 固有の危険な値)まで見るのがフォークならではの価値。

安全性
------
この検査器は .blend の中身を一切「実行しない」。
  * 必ず --disable-autoexec (-Y) で走らせる → 埋め込み Python は読み込みでも動かない
  * レンダーを一切呼ばない → A-1(SHARC LIVE の 1GiB 書き込み)は device_update /
    render を通らないと発火しないので、列挙だけなら踏まない
つまり「読み込んで、中を数えて名前を出して、閉じる」だけ。判断材料を出すのが仕事で、
勝手にブロックはしない(既定を変えない方針)。

使い方
------
    blender -b -Y --factory-startup <調べたい.blend> \
            --python tools/falcon_inspect_blend.py

  -b                ヘッドレス(GUI を出さない)
  -Y                --disable-autoexec と同義。★これを外すと意味が無い。検査器は
                    自分でコマンドラインを見て、無ければ強く警告する。
  --factory-startup ★推奨。付けないと利用者のアドオンが全部読み込まれ、その
                    load_post ハンドラが「疑わしいファイルのデータ」を触る。
                    実測(2026-08-22)で 30 個のアドオンが有効なまま検査が走り、
                    BlendLuxCore の load_post が実際にファイル内のパスを書き換えた。

終了コード:
  2  HIGH が一つでもある(= 門で止めるべき)
  1  MED / INFO だけ(見てから判断)
  0  何も見つからず
CI やラッパーで門にできる。

将来の強化(まだやっていない・引き継ぎ)
  * ここは「読み込んでから」見る形。もっと強いのは .blend の DNA を Blender を
    通さず純 Python で読み、一切ロードせずに数える版(zero-execution)。TEXT ブロック
    数と autoexec 対象は DNA から取れる。Falcon の IDProperty も DNA から拾える。
  * GUI パネル(開いた直後に消せない形で出す)。UI 変更は理由を先に、が本人方針なので
    ここでは入れていない。まず CLI で判断材料を出す所から。
"""

import sys


def _p(s=""):
    print(s, flush=True)


def _rule(title):
    _p()
    _p("=" * 68)
    _p(title)
    _p("=" * 68)


def run():
    try:
        import bpy
    except ImportError:
        _p("!! この検査器は Blender の中で走らせてください:")
        _p("     blender -b -Y <file.blend> --python tools/falcon_inspect_blend.py")
        return 3

    findings = []          # (深刻度, 種別, 説明) 深刻度: 'HIGH'/'MED'/'INFO'
    _blocked_evidence = []  # Blender 自身が自動実行を止めた記録
    def flag(sev, kind, msg):
        findings.append((sev, kind, msg))

    blend_path = bpy.data.filepath or "(名前なし / 起動時ファイル)"
    _rule("Falcon .blend 検査器  —  %s" % blend_path)

    # --- 0. autoexec が本当に切れているか -----------------------------------
    # これが有効だと、この検査自体がファイルの Python を走らせてしまう。
    #
    # ★2026-08-22 実機で判明: ここを preferences.filepaths.use_scripts_auto_execute で
    #   見るのは誤り。あれは「ユーザー設定」で、-Y は設定を書き換えず G.f のフラグだけを
    #   落とすので、-Y 付きで走らせても True のまま返る。実測で -Y ありでも True・
    #   ファイル内テキストは実際にはスキップされていた。結果、素の Cube だけの .blend
    #   でも毎回 HIGH が立ち、exit 2 になっていた(= 門として使えない誤検出)。
    #   確実に取れるのはコマンドライン自身。
    disabled_flag, enabled_flag = _autoexec_cmdline()
    _p()
    if enabled_flag:
        _p("!!!! -y (--enable-autoexec) が指定されています。ファイル内の Python は")
        _p("     既に走っています。この検査結果は事後報告にしかなりません。")
        flag('HIGH', 'autoexec', "検査を -y (--enable-autoexec) 付きで実行した")
    elif not disabled_flag:
        _p("!!!! -Y (--disable-autoexec) が指定されていません。")
        _p("     利用者の設定次第では、ファイル内の Python が既に走っています。")
        _p("     -Y を付けて走らせ直してください。")
        flag('HIGH', 'autoexec', "検査を -Y (--disable-autoexec) 無しで実行した")
    else:
        _p("[OK] -Y 指定あり。読み込みでファイル内の Python は動いていません。")

    # Blender 自身が「自動実行を止めた」と言っているか。これは設定でなく実際の結果で、
    # 真なら『走るはずのものが確かに入っていた』という独立した証拠になる。
    if getattr(bpy.app, "autoexec_fail", False):
        msg = getattr(bpy.app, "autoexec_fail_message", "") or "(詳細なし)"
        _p("[証拠] Blender が自動実行を実際にブロックしました: %s" % msg)
        _blocked_evidence.append(msg)

    if _autoexec_pref_enabled():
        _p("[参考] 利用者設定(自動実行を許可)は オン。-Y 無しで開くと走ります。")
    else:
        _p("[参考] 利用者設定(自動実行を許可)は オフ。")

    # --- 1. Python 実行面 ---------------------------------------------------
    _rule("1. 実行されうる Python(自動実行をオンにすると動くもの)")

    texts = list(bpy.data.texts)
    reg_texts = [t for t in texts if getattr(t, "use_module", False)]
    py_like = [t for t in texts if t.name.endswith(".py")]
    if not texts:
        _p("  埋め込みテキスト: なし")
    else:
        _p("  埋め込みテキスト: %d 個" % len(texts))
        for t in texts:
            n_lines = len(t.lines)
            marks = []
            if getattr(t, "use_module", False):
                marks.append("★Register(読み込みで実行)")
            if t.name.endswith(".py"):
                marks.append(".py")
            head = (t.lines[0].body[:60] if t.lines else "").strip()
            _p("    - %-28s %5d 行  %s" % (t.name, n_lines, " ".join(marks)))
            if head:
                _p("        先頭: %s" % head)
        for t in reg_texts:
            flag('HIGH', 'text-register',
                 "テキスト '%s' は Register 付き = 自動実行オンなら読み込みで走る" % t.name)
        for t in py_like:
            if t not in reg_texts:
                flag('MED', 'text-py',
                     "テキスト '%s' は Python スクリプト(手動 or ドライバ経由で走りうる)" % t.name)

    # ドライバ(Scripted Expression = PyDriver)
    scripted = _collect_scripted_drivers()
    _p()
    if not scripted:
        _p("  スクリプト式ドライバ(PyDriver): なし")
    else:
        _p("  ★スクリプト式ドライバ(PyDriver): %d 個" % len(scripted))
        for owner, path_str, expr in scripted[:40]:
            _p("    - %-40s  式: %s" % (owner, expr[:60]))
        if len(scripted) > 40:
            _p("    …ほか %d 個" % (len(scripted) - 40))
        flag('HIGH', 'pydriver',
             "スクリプト式ドライバが %d 個 = 自動実行オンなら評価時に Python が走る" % len(scripted))

    # 自動実行がブロックされた記録があるのに、上のどれにも当たらなかった場合。
    # = この検査器が数え落としている実行面がある、という意味なので必ず出す。
    if _blocked_evidence and not reg_texts and not scripted:
        flag('HIGH', 'autoexec-blocked',
             "Blender は自動実行を止めた(%s)のに、この検査器はその出所を列挙できなかった"
             % "; ".join(_blocked_evidence))

    # 検査中に利用者のアドオンが動いていないか。動いていると、その load_post ハンドラが
    # 疑わしいファイルのデータを触る(実測: BlendLuxCore が実際にパスを書き換えた)。
    # ★--factory-startup の有無で見る。個数では見ない —— --factory-startup を付けても
    #   同梱アドオン(cycles / io_scene_* など)は 8 個ほど有効なままなので、
    #   個数で判定すると良性ファイルでも必ず 1 件出てしまう(実測 2026-08-22)。
    n_addons = 0
    try:
        n_addons = len(bpy.context.preferences.addons)
    except Exception:
        pass
    factory = ("--factory-startup" in sys.argv)
    _p()
    if not factory:
        _p("  [注意] --factory-startup 無しで検査しています(有効なアドオン %d 個)。"
           % n_addons)
        _p("         利用者のアドオンの load_post ハンドラが、疑わしいファイルの")
        _p("         データを触ります(実測: BlendLuxCore が実際にパスを書き換えた)。")
        flag('INFO', 'addons',
             "検査を --factory-startup 無しで実行(アドオン %d 個が読み込まれている)" % n_addons)
    else:
        _p("  [OK] --factory-startup 指定あり(同梱アドオン %d 個のみ)。" % n_addons)

    # --- 2. Falcon 固有の危険な値(Python を通らない経路) -------------------
    _rule("2. Falcon の値(★Python を通らずに効く外部入力。フォーク固有)")
    _p("  根: scene.cycles の Falcon プロパティは IDProperty なので .blend 読み込み時に")
    _p("  RNA の min/max が当て直されない。範囲外・外部パスをここで洗い出す。")
    _p()

    CACHE_HINT = "~/.cache/falcon_photon"
    # ★書き込み先になるパスと、読み込み元にしかならないパスを分ける。
    #   falcon_sharc_cache は A-1 そのもの(SHARC LIVE がここへ 1GiB を fwrite する)。
    #   das_map / error_map は read で開くだけなので、外部を指していても深刻度は下。
    WRITE_PATH_PROPS = {"falcon_sharc_cache"}
    READ_PATH_PROPS = {"falcon_das_map", "falcon_error_map"}
    PATH_PROPS = WRITE_PATH_PROPS | READ_PATH_PROPS

    any_scene = False
    for scene in bpy.data.scenes:
        cs = getattr(scene, "cycles", None)
        if cs is None:
            continue
        # .blend に実際に格納されている IDProperty のキーだけを見る(= 攻撃者が書いた値)
        try:
            keys = [k for k in cs.keys() if k.startswith("falcon_")]
        except Exception:
            keys = []
        if not keys:
            continue
        any_scene = True
        _p("  [シーン '%s']" % scene.name)
        rna_props = cs.bl_rna.properties
        for k in sorted(keys):
            raw = cs[k]
            rna = rna_props.get(k)
            note = ""
            # 数値: RNA の hard_min/hard_max を超えていないか(IDProperty バイパス検出)
            if rna is not None and hasattr(rna, "hard_max") and isinstance(raw, (int, float)):
                lo, hi = rna.hard_min, rna.hard_max
                if raw > hi or raw < lo:
                    note = "★範囲外 (RNA 上限/下限 %g..%g を超えている)" % (lo, hi)
                    flag('HIGH', 'falcon-range',
                         "%s = %g がRNA範囲 %g..%g の外(IDPropertyでRNA上限が効いていない)"
                         % (k, raw, lo, hi))
            # 文字列パス: キャッシュディレクトリの外を指していないか
            if k in PATH_PROPS and isinstance(raw, str) and raw:
                if not _under_cache_dir(raw):
                    if k in WRITE_PATH_PROPS:
                        # ★A-1 の発火条件そのもの。修正前のビルド(v0.2 以前)や、
                        #   閉じ込めを外した環境変数付きの環境では、このファイルを
                        #   開いてレンダーした時点でこのパスが 1GiB で潰される。
                        note = "★★書き込み先が %s の外" % CACHE_HINT
                        live = (scene.cycles.get("falcon_sharc_mode") == 3)
                        flag('HIGH', 'falcon-write-path',
                             "%s = '%s' が %s の外を指す(A-1: SHARC がここへ 1GiB 書く。"
                             "sharc_mode=%s%s)"
                             % (k, raw, CACHE_HINT,
                                scene.cycles.get("falcon_sharc_mode"),
                                " ★LIVE = 開いてビューポートを Rendered にした時点で発火"
                                if live else ""))
                    else:
                        note = "★外部パス(%s の外)" % CACHE_HINT
                        flag('MED', 'falcon-read-path',
                             "%s = '%s' が %s の外を指す(読み込みのみ。中身は外へ出ないが、"
                             "何を指しているかは見えるべき)" % (k, raw, CACHE_HINT))
            _p("    %-34s = %-24s %s" % (k, repr(raw), note))
        _p()
    if not any_scene:
        _p("  格納された Falcon の値なし(このファイルは Falcon 設定を持っていない)。")

    # --- まとめ -------------------------------------------------------------
    _rule("まとめ")
    if not findings:
        _p("  実行されうる Python・範囲外の Falcon 値・外部パス: 見つからず。")
        _p("  ※ これは『この検査器が見た範囲で』の話です。ジオメトリノードや")
        _p("     他のアドオンが読む独自プロパティまでは見ていません。")
        return 0

    order = {'HIGH': 0, 'MED': 1, 'INFO': 2}
    findings.sort(key=lambda f: order.get(f[0], 9))
    n_high = sum(1 for f in findings if f[0] == 'HIGH')
    _p("  %d 件(うち HIGH %d 件):" % (len(findings), n_high))
    for sev, kind, msg in findings:
        _p("    [%-4s] %-16s %s" % (sev, kind, msg))
    _p()
    if n_high:
        _p("  → HIGH があるうちは、送り主を信用できない限り自動実行はオンにしないこと。")
        _p("     Falcon の範囲外値はエンジン側で切り詰め/読み替えされる(A-1〜A-3 修正済み)が、")
        _p("     『そういう値が仕込まれている』こと自体が送り主の意図を示す手掛かり。")
        return 2
    _p("  → HIGH なし。上の件は見てから判断するもので、門で止める必要はありません。")
    return 1


def _autoexec_cmdline():
    """コマンドラインから (自動実行を切ったか, わざと入れたか) を返す。

    ★ここを preferences.filepaths.use_scripts_auto_execute で見てはいけない。
      あれはユーザー設定で、-Y は設定を書き換えない(G.f のフラグだけ落とす)。
      -Y 付きでも True を返すため、良性ファイルでも毎回 HIGH になっていた。
    """
    import sys as _sys
    argv = _sys.argv
    disabled = ("-Y" in argv) or ("--disable-autoexec" in argv)
    enabled = ("-y" in argv) or ("--enable-autoexec" in argv)
    return disabled, enabled


def _autoexec_pref_enabled():
    """利用者設定として自動実行が許可されているか(参考表示用。判定には使わない)。"""
    try:
        import bpy
        return bool(bpy.context.preferences.filepaths.use_scripts_auto_execute)
    except Exception:
        return False


def _collect_scripted_drivers():
    """Scripted Expression 型のドライバ(PyDriver)を全 ID から集める。"""
    import bpy
    out = []
    seen = set()
    for attr in dir(bpy.data):
        try:
            coll = getattr(bpy.data, attr)
        except Exception:
            continue
        if not hasattr(coll, "__iter__") or isinstance(coll, (str, bytes)):
            continue
        for idblock in coll:
            ad = getattr(idblock, "animation_data", None)
            if not ad:
                continue
            for fcu in getattr(ad, "drivers", []):
                drv = fcu.driver
                if drv and drv.type == 'SCRIPTED' and drv.expression:
                    # 同じ ID が複数のコレクションから見えるので重複を潰す
                    # (実測: Cube 1個のドライバが 2件として数えられていた)
                    try:
                        key = (idblock.as_pointer(), fcu.data_path, fcu.array_index)
                    except Exception:
                        key = (getattr(idblock, "name", "?"), fcu.data_path,
                               getattr(fcu, "array_index", 0))
                    if key in seen:
                        continue
                    seen.add(key)
                    owner = "%s.%s[%s]" % (getattr(idblock, "name", "?"),
                                           fcu.data_path,
                                           getattr(fcu, "array_index", 0))
                    out.append((owner, fcu.data_path, drv.expression))
    return out


def _under_cache_dir(path):
    import os
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    cache_dir = os.path.realpath(os.path.join(base, "falcon_photon"))
    try:
        ap = os.path.realpath(os.path.expanduser(path))
    except Exception:
        return False
    return ap == cache_dir or ap.startswith(cache_dir + os.sep)


if __name__ == "__main__":
    sys.exit(run())
