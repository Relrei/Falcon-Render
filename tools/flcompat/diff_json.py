#!/usr/bin/env python3
"""flcompat: JSON の突き合わせ(C2 の往復・C3 の UI クラス)。system python で走る。

  diff_json.py c2 <J1 falcon> <J2 stock> <J3 falcon> <out.json>
  diff_json.py c3 <stock.json> <falcon.json> <out.json>

rc 0 = 合格 / rc 1 = 不合格
"""
import json
import sys

IGNORE_KEYS = {
    # 開いた .blend の場所そのもの(往復で変わって当たり前)
    "filepath",
    # ビルドの素性
    "app", "version",
    # 出力先の文字列(こちらが指定して撮る物)
    "filepath_raw",
}


def walk(a, b, path, diffs, ignore_falcon=True):
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k in IGNORE_KEYS:
                continue
            if ignore_falcon and (k.startswith("falcon") or k.endswith("_falcon")):
                continue
            p = path + "/" + str(k)
            if k not in a:
                diffs.append({"path": p, "kind": "only_in_B", "b": b[k]})
            elif k not in b:
                diffs.append({"path": p, "kind": "only_in_A", "a": a[k]})
            else:
                walk(a[k], b[k], p, diffs, ignore_falcon)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            diffs.append({"path": path, "kind": "len", "a": len(a), "b": len(b)})
            return
        for i, (x, y) in enumerate(zip(a, b)):
            walk(x, y, "%s[%d]" % (path, i), diffs, ignore_falcon)
    else:
        if a != b:
            diffs.append({"path": path, "kind": "value",
                          "a": a if not isinstance(a, (dict, list)) else "<%s>" % type(a).__name__,
                          "b": b if not isinstance(b, (dict, list)) else "<%s>" % type(b).__name__})


def load(p):
    with open(p) as f:
        return json.load(f)


def falcon_props(js):
    """falcon_* の袋だけを集める(素が捨てた後に既定へ戻るかを見るため)。"""
    out = {}

    def rec(o, path):
        if isinstance(o, dict):
            for k, v in o.items():
                if k.endswith("_falcon") or k.startswith("falcon"):
                    out[path + "/" + k] = v
                else:
                    rec(v, path + "/" + k)
    rec(js, "")
    return out


def c2(argv):
    j1, j2, j3, outp = argv
    A, B, C = load(j1), load(j2), load(j3)
    res = {}
    d13 = []
    walk(A, C, "", d13)
    res["falcon_vs_roundtrip"] = d13
    d12 = []
    walk(A, B, "", d12)
    res["falcon_vs_stock_open"] = d12

    f1, f3 = falcon_props(A), falcon_props(C)
    dropped = {k: v for k, v in f1.items() if f3.get(k) != v}
    res["falcon_props_changed_by_roundtrip"] = dropped
    res["falcon_props_count"] = len(f1)

    ok = len(d13) == 0
    res["verdict"] = "PASS" if ok else "FAIL"
    with open(outp, "w") as f:
        json.dump(res, f, ensure_ascii=False, indent=1)
    print("C2 %s  本体の差分 %d 件 (素で開いた時点 %d 件) / falcon_* %d 件のうち往復で変わった %d 件"
          % (res["verdict"], len(d13), len(d12), len(f1), len(dropped)))
    for d in d13[:20]:
        print("   ", d)
    return 0 if ok else 1


def c3(argv):
    stockp, falconp, outp = argv
    S, F = load(stockp), load(falconp)
    # ★素でも落ちるアドオンは Falcon の非互換ではない(上流のまま)。
    #   落ちる物の集合が素と同じかどうかだけを見る。
    sf = {d["addon"] for d in S.get("enable_failed", [])}
    ff = {d["addon"] for d in F.get("enable_failed", [])}
    res = {"enable_failed_falcon": F.get("enable_failed", []),
           "enable_failed_stock": S.get("enable_failed", []),
           "enable_failed_only_falcon": sorted(ff - sf),
           "enable_failed_only_stock": sorted(sf - ff),
           "enabled_ok_falcon": len(F.get("enabled_ok", [])),
           "enabled_ok_stock": len(S.get("enabled_ok", [])),
           "bundled_only_in_falcon": sorted(set(F.get("bundled_addons", [])) - set(S.get("bundled_addons", []))),
           "bundled_only_in_stock": sorted(set(S.get("bundled_addons", [])) - set(F.get("bundled_addons", [])))}

    for phase in ("ui_before", "ui_after"):
        su, fu = S.get(phase, {}), F.get(phase, {})
        missing = sorted(set(su) - set(fu))          # ★素に在って Falcon に無い = 消えた
        added = sorted(set(fu) - set(su))            # Falcon が足した物(足すのは可)
        hijacked = []
        extended = []   # ★append/prepend で足しただけ(元の draw が残っている)= 安全
        for k in sorted(set(su) & set(fu)):
            a, b = su[k], fu[k]
            for field in ("draw", "poll"):
                fa, fb = a.get(field), b.get(field)
                ka = (fa or {}).get("mod"), (fa or {}).get("qual")
                kb = (fb or {}).get("mod"), (fb or {}).get("qual")
                if ka == kb:
                    continue
                if field == "draw":
                    fns = b.get("draw_funcs") or []
                    keys = [((f or {}).get("mod"), (f or {}).get("qual")) for f in fns]
                    if keys and keys[0] == ka:
                        extended.append({"cls": k, "kept": ka,
                                         "added": [x for x in keys[1:]]})
                        continue
                hijacked.append({"cls": k, "field": field, "stock": ka, "falcon": kb})
            for field in ("bl_idname", "space", "region", "context", "parent", "base"):
                if a.get(field) != b.get(field):
                    hijacked.append({"cls": k, "field": field,
                                     "stock": a.get(field), "falcon": b.get(field)})
        res[phase] = {"stock_classes": len(su), "falcon_classes": len(fu),
                      "missing_in_falcon": missing, "added_in_falcon": added,
                      "changed": hijacked, "extended": extended}

    ok = (not res["enable_failed_only_falcon"]
          and not res["ui_before"]["missing_in_falcon"]
          and not res["ui_before"]["changed"]
          and not res["ui_after"]["missing_in_falcon"]
          and not res["ui_after"]["changed"])
    res["verdict"] = "PASS" if ok else "FAIL"
    with open(outp, "w") as f:
        json.dump(res, f, ensure_ascii=False, indent=1)
    print("C3 %s  アドオン登録の失敗 falcon=%d stock=%d(Falcon だけ落ちる物 %d)" %
          (res["verdict"], len(res["enable_failed_falcon"]), len(res["enable_failed_stock"]),
           len(res["enable_failed_only_falcon"])))
    for a in res["enable_failed_only_falcon"]:
        print("      Falcon だけ落ちた:", a)
    for phase in ("ui_before", "ui_after"):
        r = res[phase]
        print("   %-9s 素 %d クラス / Falcon %d(新設 %d・元を残して足した %d)"
              "・消えた %d・乗っ取り %d"
              % (phase, r["stock_classes"], r["falcon_classes"], len(r["added_in_falcon"]),
                 len(r["extended"]), len(r["missing_in_falcon"]), len(r["changed"])))
        for e in r["extended"]:
            print("      足した(元は残っている):", e["cls"], "<-", e["added"])
        for m in r["missing_in_falcon"][:10]:
            print("      消えた:", m)
        for c in r["changed"][:10]:
            print("      変わった:", c)
    return 0 if ok else 1


if __name__ == "__main__":
    mode = sys.argv[1]
    sys.exit({"c2": c2, "c3": c3}[mode](sys.argv[2:]))
