#!/usr/bin/env python3
"""flcompat C1 の判定 — 「床(素を2回撮った差)」と「信号(素 対 Falcon)」を比べる。

  verdict_c1.py <floor.json> <signal.json>
  -> "<判定>\t<信号の画素数>\t<%>\trmse\tmax\t床px\t床rmse"

★床を測らずに「一致しない」と言わない。Cycles + コンポジタは同じバイナリでも
  run-to-run で一致しないことがある(2026-09-02 実測)。
"""
import json
import sys

fl = json.load(open(sys.argv[1]))
sg = json.load(open(sys.argv[2]))

fp = fl.get("pixels_diff", -1)
sp = sg.get("pixels_diff", -1)
fm = float(fl.get("max_abs", 0.0) or 0.0)
fr_floor = float(fl.get("rmse", 0.0) or 0.0)
sm = float(sg.get("max_abs", 0.0) or 0.0)
fr = float(sg.get("rmse", 0.0) or 0.0)

if sg.get("verdict") == "SHAPE_MISMATCH":
    v = "SHAPE_MISMATCH"
elif sp == 0:
    v = "BIT_IDENTICAL"
elif fp == 0:
    # 床がビット一致 = この場面は再現する ⇒ 1画素でも違えば Falcon の差
    v = "OVER_FLOOR"
elif sp <= fp * 1.5 and fr <= fr_floor * 1.5:
    # ★判定は RMSE と画素数で行う。max_abs は 50万画素の最大値=極値統計で
    #   ばらつきが大きく、単独では床を張れない(参考として出すだけ)。
    v = "WITHIN_FLOOR"
else:
    # ★ここだけが「Falcon が絵を動かした」を疑う根拠になる
    v = "OVER_FLOOR"

print("%s\t%d\t%s\trmse=%.3g\tmax=%.3g\t床px=%d\t床rmse=%.3g"
      % (v, sp, sg.get("pixels_diff_pct", ""), fr, sm, fp, fr_floor))
