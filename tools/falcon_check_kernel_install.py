#!/usr/bin/env python3
"""配布したカーネルソースだけで GPU カーネルが組み立てられるかを確かめる門。

★この不具合はビルドでは絶対に見つからない。
GPU(CUDA/OptiX/HIP/oneAPI)のカーネルは**実行時に**
`<build>/bin/5.2/scripts/addons_core/cycles/source/` を読んでコンパイルされる。
`SRC_KERNEL_*_HEADERS` に載せ忘れたヘッダがあると、ビルドは通り CPU レンダーも通り、
**GPU レンダーだけが丸ごと失敗する**。

2026-08-23 実測: フォーク固有の `falcon_sharc_size.h` と `falcon_lighttrace.h` の2つが
載っておらず、**配布ビルド build_falcon_render_v0.3 でも CUDA のカーネルが
コンパイルできなかった**(GPUレンダー不可)。

使い方:
    python3 tools/falcon_check_kernel_install.py <build>/bin/5.2/scripts/addons_core/cycles/source

見るのは**フォークが足したファイル(falcon_*)**に限る。上流のヘッダは
`#ifdef` の中でしか使われない物(device/cpu/*・oneapi/context_*)があり、
素朴な閉包だと嘘の欠けが出るため。フォークが足した物は無条件に読まれる。
"""
import os
import re
import sys

INC = re.compile(r'#\s*include\s+"(kernel/[^"]*falcon_[^"]+)"')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    kernel = os.path.join(root, "kernel")
    if not os.path.isdir(kernel):
        print(f"RED: カーネルソースが無い: {kernel}")
        return 2

    wanted = {}
    for dirpath, _dirs, files in os.walk(kernel):
        for f in files:
            if not f.endswith((".h", ".cu", ".cpp", ".cl")):
                continue
            p = os.path.join(dirpath, f)
            try:
                text = open(p, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for m in INC.findall(text):
                wanted.setdefault(m, set()).add(os.path.relpath(p, root))

    missing = {k: v for k, v in wanted.items()
               if not os.path.exists(os.path.join(root, k))}
    print(f"falcon_* への参照 {len(wanted)} 種類")
    if not missing:
        print("GREEN: 配布されたソースだけで閉じている")
        return 0
    print(f"RED: {len(missing)} 個が配布されていない(GPUレンダーが落ちる)")
    for k, froms in sorted(missing.items()):
        print(f"  {k}\n      ← {', '.join(sorted(froms))}")
    print("\n直し方: intern/cycles/kernel/CMakeLists.txt の SRC_KERNEL_*_HEADERS に足す")
    return 1


sys.exit(main())
