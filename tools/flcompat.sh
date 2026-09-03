#!/usr/bin/env bash
# flcompat — Falcon Render の「互換性」を測る門。
#
#   「互換性が高い」は測ってから言う。(2026-09-02 本人の方針を受けて新設)
#
#   C1  素と同じ絵     公開デモの .blend を、素の Blender 5.2.1 と Falcon で
#                      同じ設定で撮って画素で比べる
#   C2  往復で壊れない Falcon で保存 → 素で開いて保存 → Falcon で開く。
#                      falcon_* を除いた中身の差分がゼロ
#   C3  アドオンと同居 同梱アドオンを全部有効化して例外ゼロ、かつ素の UI クラスが
#                      消えていない/乗っ取られていない
#
#   使い方: tools/flcompat.sh [--c1|--c2|--c3|--all] [--work DIR] [--scenes N]
#   rc 0 = 全部合格 / rc 1 = 1つでも不合格
#
# ★★C1 の物差しについて(2026-09-02 実測):
#   **Cycles の CPU レンダーは複数スレッドだと同じバイナリでも run-to-run で一致しない**
#   (BMW27 960x540 8spp・4スレッドで 369/518400 画素・最大差 1.9e-4)。
#   md5 も一致しない(EXR のヘッダに毎回違うバイトが入る)。
#   ⇒ **門はスレッド1本で撮る**。そこでは素 と Falcon が**ビット一致**する。
#   ⇒ 複数スレッドの数字は「参考」で、判定には使わない。
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="$HERE/flcompat"
WORK="${FLCOMPAT_WORK:-$HOME/Documents2/flcompat-20260902}"
STOCK="${FLCOMPAT_STOCK:-$HOME/Apps/blender-5.2.1-linux-x64/blender}"
FALCON="${FLCOMPAT_FALCON:-$HOME/Documents2/build_blender_5.2_dlss/bin/blender}"
TASKSET="${FLCOMPAT_TASKSET:-taskset -c 4-5,10-11 nice -n 10}"
SAMPLES="${FLCOMPAT_SAMPLES:-8}"
RES_X="${FLCOMPAT_RES_X:-960}"
RES_Y="${FLCOMPAT_RES_Y:-540}"
NSCENES=0

DO_C1=0; DO_C2=0; DO_C3=0
while [ $# -gt 0 ]; do
  case "$1" in
    --c1) DO_C1=1 ;;
    --c2) DO_C2=1 ;;
    --c3) DO_C3=1 ;;
    --all) DO_C1=1; DO_C2=1; DO_C3=1 ;;
    --work) shift; WORK="$1" ;;
    --scenes) shift; NSCENES="$1" ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "flcompat: 知らない引数 $1" >&2; exit 2 ;;
  esac
  shift
done
[ $((DO_C1+DO_C2+DO_C3)) -eq 0 ] && { DO_C1=1; DO_C2=1; DO_C3=1; }

mkdir -p "$WORK"/{out,tmp,img,c2,c3}
FAIL=0
G="\033[38;2;140;190;90m"; R="\033[38;2;220;110;110m"; Y="\033[38;2;220;190;100m"; D="\033[38;2;150;150;150m"; N="\033[0m"

for b in "$STOCK" "$FALCON"; do
  [ -x "$b" ] || { echo -e "${R}flcompat: 実行できない: $b${N}"; exit 2; }
done

echo -e "${D}素   $($STOCK --version 2>/dev/null | head -1) — $STOCK${N}"
echo -e "${D}改造 $($FALCON --version 2>/dev/null | head -1) — $FALCON${N}"

# ── 台帳から場面を読む ────────────────────────────────────────────
SCN_NAME=(); SCN_PATH=(); SCN_SRC=()
while IFS=$'\t' read -r name path src; do
  case "$name" in ''|\#*) continue ;; esac
  [ -f "$path" ] || { echo -e "${Y}flcompat: 見つからない: $path${N}"; continue; }
  SCN_NAME+=("$name"); SCN_PATH+=("$path"); SCN_SRC+=("$src")
done < "$PY/scenes.tsv"
[ "$NSCENES" -gt 0 ] && { SCN_NAME=("${SCN_NAME[@]:0:$NSCENES}"); SCN_PATH=("${SCN_PATH[@]:0:$NSCENES}"); }

# blender は Python が落ちても rc=0 で完走する ⇒ 成果物で判定する
render() { # $1=バイナリ $2=blend $3=出力ベース $4=スレッド数 [$5=FLC_SET]
  rm -f "$3.exr" "$3.done"
  env BLENDER_USER_RESOURCES="$WORK/tmp/bres" \
      FLC_OUT="$3" FLC_SAMPLES="$SAMPLES" FLC_RES_X="$RES_X" FLC_RES_Y="$RES_Y" \
      FLC_THREADS="$4" FLC_SET="${5:-}" \
      $TASKSET "$1" --factory-startup -b "$2" --python "$PY/render_one.py" \
      >"$3.log" 2>&1
  [ -s "$3.done" ]
}

compare() { # $1=A.exr $2=B.exr $3=out.json [$4=map.png] -> stdout に1行 JSON
  env BLENDER_USER_RESOURCES="$WORK/tmp/bres" \
      FLC_A="$1" FLC_B="$2" FLC_JSON="$3" FLC_MAP="${4:-}" \
      $TASKSET "$STOCK" --factory-startup -b --python "$PY/compare_exr.py" 2>/dev/null \
    | sed -n 's/^FLCOMPAT_COMPARE //p'
}

# ── C1 ────────────────────────────────────────────────────────────
if [ "$DO_C1" = 1 ]; then
  echo
  echo "== C1 素と同じ絵 (${RES_X}x${RES_Y} ${SAMPLES}spp CPU・スレッド1本) =="
  # ★★対照込みで測る。素の Blender を 2 回撮った差（=床）より Falcon との差が
  #   大きくなければ合格。★床を測らずに「一致しない」を出すと、Blender 自身の
  #   run-to-run のゆらぎを Falcon のせいにする（2026-09-02 実測: lone-monk は
  #   素同士でも 76.9% の画素が違う = コンポジタ側のゆらぎ）。
  C1FAIL=0
  : > "$WORK/out/c1_table.tsv"
  for i in "${!SCN_NAME[@]}"; do
    n="${SCN_NAME[$i]}"; p="${SCN_PATH[$i]}"
    a="$WORK/out/c1_${n}_stock"; a2="$WORK/out/c1_${n}_stock2"; b="$WORK/out/c1_${n}_falcon"
    render "$STOCK"  "$p" "$a"  1 || { echo -e "  ${R}$n: 素の側が撮れなかった (see $a.log)${N}"; C1FAIL=1; continue; }
    render "$STOCK"  "$p" "$a2" 1 || { echo -e "  ${R}$n: 素の2枚目が撮れなかった${N}"; C1FAIL=1; continue; }
    render "$FALCON" "$p" "$b"  1 || { echo -e "  ${R}$n: Falcon 側が撮れなかった (see $b.log)${N}"; C1FAIL=1; continue; }
    compare "$a.exr" "$a2.exr" "$WORK/out/c1_${n}_floor.json" > /dev/null
    compare "$a.exr" "$b.exr"  "$WORK/out/c1_${n}.json" "$WORK/img/c1_${n}_map.png" > /dev/null
    v=$(python3 "$PY/verdict_c1.py" "$WORK/out/c1_${n}_floor.json" "$WORK/out/c1_${n}.json")
    printf '%s\t%s\n' "$n" "$v" >> "$WORK/out/c1_table.tsv"
    case "$v" in
      BIT_IDENTICAL*) echo -e "  ${G}$n  ビット一致${N}" ;;
      WITHIN_FLOOR*)  echo -e "  ${Y}$n  床の内側 $v${N}" ;;
      *) echo -e "  ${R}$n  $v${N}"; C1FAIL=1 ;;
    esac
  done
  [ "$C1FAIL" = 0 ] && echo -e "${G}C1 PASS${N}" || { echo -e "${R}C1 FAIL${N}"; FAIL=1; }
fi

# ── C2 ────────────────────────────────────────────────────────────
if [ "$DO_C2" = 1 ]; then
  echo
  echo "== C2 往復で壊れない (Falcon 保存 → 素で保存 → Falcon で開く) =="
  C2FAIL=0
  for i in "${!SCN_NAME[@]}"; do
    n="${SCN_NAME[$i]}"; p="${SCN_PATH[$i]}"
    d="$WORK/c2/$n"; mkdir -p "$d"
    # ★保存先は元と同じディレクトリに置かない(元を汚さない)。相対パスは
    #   relative_remap=False で文字列のまま持ち越すので、DNA の比較には影響しない。
    # ★段0 を置く理由: Blender は保存時に**参照ゼロのデータブロックを捨てる**。
    #   しかも掃除は一段ずつ進む(テクスチャが消える→次の保存で画像が消える)ので、
    #   元ファイルと比べると「往復で壊れた」と誤読する。
    # ★★さらに対照を置く: 真ん中を素にした鎖(S)と、真ん中も Falcon にした鎖(F)を
    #   同じ回数だけ保存して比べる。⇒ 掃除の段数が両方で同じになり、
    #   **残った差だけが「素を通したせい」**になる。
    run_step() { # $1=バイナリ $2=読む .blend $3=出す json $4=保存先(空可) $5=ログ名
      rm -f "$3" "$3.done"
      env BLENDER_USER_RESOURCES="$WORK/tmp/bres" FLC_JSON="$3" FLC_SAVE_AS="$4" \
        $TASKSET "$1" --factory-startup -b "$2" --python "$PY/dump_state.py" \
        > "$d/$5.log" 2>&1
      [ -s "$3.done" ]
    }
    ok=1
    run_step "$FALCON" "$p"                 "$d/j0.json"  "$d/a0.blend" s0 || ok=0
    [ $ok = 1 ] && { run_step "$FALCON" "$d/a0.blend" "$d/j1.json"  "$d/a1.blend" s1 || ok=0; }
    # S = 素を通す鎖 / F = 全部 Falcon の鎖(対照)
    [ $ok = 1 ] && { run_step "$STOCK"  "$d/a1.blend" "$d/j2S.json" "$d/bS.blend" s2S || ok=0; }
    [ $ok = 1 ] && { run_step "$FALCON" "$d/a1.blend" "$d/j2F.json" "$d/bF.blend" s2F || ok=0; }
    [ $ok = 1 ] && { run_step "$FALCON" "$d/bS.blend" "$d/j3S.json" "" s3S || ok=0; }
    [ $ok = 1 ] && { run_step "$FALCON" "$d/bF.blend" "$d/j3F.json" "" s3F || ok=0; }
    if [ $ok = 0 ]; then
      echo -e "  ${R}$n: 途中で落ちた (see $d/*.log)${N}"; C2FAIL=1; continue
    fi
    # 素の側が .blend を読めたか(警告・エラーを拾う)
    warn=$(grep -cE '^(Warning|Error):' "$d/s2S.log" 2>/dev/null | head -1)
    python3 "$PY/diff_json.py" c2 "$d/j3F.json" "$d/j2S.json" "$d/j3S.json" "$d/diff.json" | sed "s/^/  $n /"
    [ "${PIPESTATUS[0]}" = 0 ] || C2FAIL=1
    echo -e "  ${D}$n 素で開いた時の警告/エラー行 ${warn:-0} 件 ($d/s2S.log)${N}"
  done
  [ "$C2FAIL" = 0 ] && echo -e "${G}C2 PASS${N}" || { echo -e "${R}C2 FAIL${N}"; FAIL=1; }
fi

# ── C3 ────────────────────────────────────────────────────────────
if [ "$DO_C3" = 1 ]; then
  echo
  echo "== C3 アドオンと同居 =="
  C3FAIL=0
  for mode in plain all; do
    for side in stock falcon; do
      bin="$STOCK"; [ "$side" = falcon ] && bin="$FALCON"
      js="$WORK/c3/${mode}_${side}.json"; rm -f "$js" "$js.done"
      env BLENDER_USER_RESOURCES="$WORK/tmp/bres" FLC_JSON="$js" \
          FLC_ENABLE_ALL="$([ "$mode" = all ] && echo 1 || echo)" \
        $TASKSET "$bin" --factory-startup -b --python "$PY/c3_addons.py" \
        > "$WORK/c3/${mode}_${side}.log" 2>&1
      [ -s "$js.done" ] || { echo -e "  ${R}C3 $mode/$side が落ちた (see $WORK/c3/${mode}_${side}.log)${N}"; C3FAIL=1; }
    done
    if [ -s "$WORK/c3/${mode}_stock.json.done" ] && [ -s "$WORK/c3/${mode}_falcon.json.done" ]; then
      echo "  -- $mode --"
      python3 "$PY/diff_json.py" c3 "$WORK/c3/${mode}_stock.json" "$WORK/c3/${mode}_falcon.json" \
        "$WORK/c3/${mode}_diff.json" | sed 's/^/  /'
      [ "${PIPESTATUS[0]}" = 0 ] || C3FAIL=1
    fi
  done
  [ "$C3FAIL" = 0 ] && echo -e "${G}C3 PASS${N}" || { echo -e "${R}C3 FAIL${N}"; FAIL=1; }
fi

echo
[ "$FAIL" = 0 ] && echo -e "${G}flcompat: 全部合格${N}" || echo -e "${R}flcompat: 不合格あり${N}"
exit "$FAIL"
