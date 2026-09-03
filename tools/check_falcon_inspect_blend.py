#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""falcon_inspect_blend.py が本当に門として使えるかを実機で確かめる。

    python3 tools/check_falcon_inspect_blend.py <blender の実行ファイル>

見るのは4つ。どれが欠けても検査器は門にならない:

  1. 悪意ある .blend を HIGH として検出し exit 2 を返すか
  2. ★良性の .blend で何も出さず exit 0 を返すか
     (誤検出のほうが実害が大きい。「全部 HIGH」なら誰も使わない)
  3. ★検査の過程で .blend の中身が実行されていないか
     Register 付きテキストと PyDriver に「印を書く」仕掛けを入れて、印が
     出来ていないことを見る。-Y が実際に効いているかを結果で確かめる。
  4. ★対照実験: 同じファイルを -y (--enable-autoexec) で開くと印が出来ること。
     これが出来ないなら、3 の「印が無い」は単に仕掛けが不発だっただけになる。

作業場所は tmpfs を避けてディスクに置く(既定 ~/Documents2/.falcon-inspect-check)。
1GiB を書く操作は一切しない(レンダーもベイクもしない)ので、メモリは Blender 1本ぶん。
"""

import os
import subprocess
import sys

BLENDER = sys.argv[1] if len(sys.argv) > 1 else None
WORK = os.environ.get("FALCON_INSPECT_WORK",
                      os.path.expanduser("~/Documents2/.falcon-inspect-check"))
HERE = os.path.dirname(os.path.abspath(__file__))
INSPECTOR = os.path.join(HERE, "falcon_inspect_blend.py")

MARKER_TEXT = os.path.join(WORK, "MARKER_text.txt")
MARKER_DRIVER = os.path.join(WORK, "MARKER_driver.txt")

# 仕掛け: 走ったらファイルを1つ書くだけ。壊すものは何も無い。
PAYLOAD = ("with open(%r, 'a') as f:\n"
           "    f.write('EXECUTED\\n')\n"
           "print('FALCON_INSPECT_PAYLOAD_RAN')\n")

MAKE = r'''
import bpy, os, sys
argv = sys.argv[sys.argv.index("--") + 1:]
OUT, MARKER_TEXT, MARKER_DRIVER = argv[0], argv[1], argv[2]
PAYLOAD = %r %% MARKER_TEXT

def reset():
    bpy.ops.wm.read_homefile(use_empty=False)
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    sc.render.resolution_x = sc.render.resolution_y = 32
    return sc

def save(n):
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, n))

# 良性: 素の Cube のまま
reset(); save("benign.blend")

# A-1 相当: SHARC LIVE + キャッシュ外への書き込み先
sc = reset()
sc.cycles["falcon_sharc_mode"] = 3
sc.cycles["falcon_sharc_cache"] = os.path.join(OUT, "victim.txt")
save("evil_sharc.blend")

# A-3 相当: RNA 上限 8.0 を超える半径
sc = reset(); sc.cycles["falcon_photon_radius"] = 1e9
save("evil_radius.blend")

# Python 実行面 1: Register 付きテキスト(読み込みで走る)
reset()
t = bpy.data.texts.new("innocent_looking.py"); t.write(PAYLOAD); t.use_module = True
save("evil_text.blend")

# Python 実行面 2: スクリプト式ドライバ(評価時に走る)
reset()
ob = bpy.data.objects.get("Cube") or bpy.data.objects[0]
fcu = ob.driver_add("location", 0)
fcu.driver.type = 'SCRIPTED'
fcu.driver.expression = "__import__('builtins').open(%%r,'a').write('EXECUTED\n') or 0" %% MARKER_DRIVER
save("evil_driver.blend")

print("SAMPLES_DONE")
''' % (PAYLOAD,)


def run(args, timeout=600):
    env = dict(os.environ)
    env["__NGX_DISABLE_UPDATER"] = "1"
    for k in list(env):
        if k.startswith("FALCON_") and k != "FALCON_INSPECT_WORK":
            del env[k]
    p = subprocess.run([BLENDER] + args, env=env, timeout=timeout,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def inspect(blend, extra=()):
    """推奨コマンドラインで検査器をかける。"""
    return run(["-b", "-Y", "--factory-startup", os.path.join(WORK, blend),
                "--python", INSPECTOR] + list(extra))


def kinds(out):
    """まとめ欄から (深刻度, 種別) を拾う。"""
    got = []
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("[") and "]" in s:
            sev = s[1:s.index("]")].strip()
            rest = s[s.index("]") + 1:].strip().split(None, 1)
            if sev in ("HIGH", "MED", "INFO") and rest:
                got.append((sev, rest[0]))
    return got


def main():
    if not BLENDER or not os.access(BLENDER, os.X_OK):
        raise SystemExit("使い方: check_falcon_inspect_blend.py <blender の実行ファイル>")
    if not os.path.exists(INSPECTOR):
        raise SystemExit("検査器が見つかりません: %s" % INSPECTOR)
    os.makedirs(WORK, exist_ok=True)
    for m in (MARKER_TEXT, MARKER_DRIVER):
        if os.path.exists(m):
            os.remove(m)

    make_py = os.path.join(WORK, "_make.py")
    with open(make_py, "w") as f:
        f.write(MAKE)
    rc, out = run(["-b", "-Y", "--factory-startup", "--python", make_py,
                   "--", WORK, MARKER_TEXT, MARKER_DRIVER])
    if "SAMPLES_DONE" not in out:
        print(out[-3000:])
        raise SystemExit("!! 検体を作れませんでした")

    results = []

    def check(name, ok, detail):
        results.append((name, ok, detail))
        print("  %-34s %s  %s" % (name, "PASS" if ok else "FAIL", detail))

    print("\n=== 1/2. 検出と誤検出 ===")
    rc, out = inspect("benign.blend")
    got = kinds(out)
    check("良性 .blend は素通り", rc == 0 and not got,
          "exit=%d 指摘=%s" % (rc, got or "なし"))

    for blend, want in (("evil_sharc.blend", "falcon-write-path"),
                        ("evil_radius.blend", "falcon-range"),
                        ("evil_text.blend", "text-register"),
                        ("evil_driver.blend", "pydriver")):
        rc, out = inspect(blend)
        got = kinds(out)
        hit = ("HIGH", want) in got
        check("%s -> HIGH %s" % (blend, want), rc == 2 and hit,
              "exit=%d 指摘=%s" % (rc, got))

    print("\n=== 3. 検査中に中身が実行されていないか ===")
    for name, marker in (("Register付きテキスト", MARKER_TEXT),
                         ("スクリプト式ドライバ", MARKER_DRIVER)):
        check("%s は走っていない" % name, not os.path.exists(marker),
              "印 %s: %s" % (marker, "存在する(実行された!)"
                             if os.path.exists(marker) else "無し"))

    print("\n=== 4. 対照実験(-y なら本当に走ることの確認)===")
    rc, out = run(["-b", "-y", "--factory-startup",
                   os.path.join(WORK, "evil_text.blend"),
                   "--python-expr", "import bpy"])
    ran = os.path.exists(MARKER_TEXT) or "FALCON_INSPECT_PAYLOAD_RAN" in out
    check("-y では仕掛けが実際に走る", ran,
          "印=%s / 出力に痕跡=%s"
          % (os.path.exists(MARKER_TEXT), "FALCON_INSPECT_PAYLOAD_RAN" in out))
    for m in (MARKER_TEXT, MARKER_DRIVER):
        if os.path.exists(m):
            os.remove(m)

    print("\n=== 5. -Y を忘れたら止まるか ===")
    rc, out = run(["-b", "--factory-startup", os.path.join(WORK, "benign.blend"),
                   "--python", INSPECTOR])
    check("-Y 無しは HIGH で止まる", rc == 2 and ("HIGH", "autoexec") in kinds(out),
          "exit=%d 指摘=%s" % (rc, kinds(out)))

    bad = [n for n, ok, _ in results if not ok]
    print("\n=== まとめ ===")
    print("  %d 件中 %d 件 PASS" % (len(results), len(results) - len(bad)))
    for n in bad:
        print("  FAIL: %s" % n)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
