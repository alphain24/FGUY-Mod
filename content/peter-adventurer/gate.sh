#!/bin/sh
# Runs the deterministic review gates for one pass on its captures (review/captures/<prefix>_*.png, made by the
# review page's captureAll) and marks the state checklist up to the AI review:
#   sh gate.sh <pass-id> <prefix>
SK=~/.claude/skills/img2threejs
export PYTHONUTF8=1
PASS=$1; P=$2; C=review/captures; S=.img2threejs/state.json
mv -f ../../build/research/${P}_*.png $C/ 2>/dev/null
m() { python $SK/forge/state.py mark $1 --state $S --evidence "$2" | grep -i "error"; }
m render-capture $C/${P}_refview.png
m review-contract-read $SK/grimoire/review/self_correction.md
python $SK/forge/stage4_review/diagnose_render.py --reference reference.png --render $C/${P}_refview.png \
  --map-stripped-render $C/${P}_refview_nomaps.png --spec object-sculpt-spec.json --pass-id $PASS --in-place --json \
  > review/tier1_${P}.json 2>&1
python - "$P" <<'EOF'
import json, sys
t = open(f'review/tier1_{sys.argv[1]}.json', encoding='utf-8').read()
d = json.loads(t[t.index('{'):])
print('tier1', d['passed'], d.get('failures'), {k: v for k, v in d['checks'].items() if k != 'colorDelta'})
EOF
m tier1-diagnostics review/tier1_${P}.json
python $SK/forge/stage4_review/diagnose_render_multi_angle.py --reference $C/${P}_front.png --orbit $C/${P}_right.png \
  --orbit $C/${P}_rear.png --orbit $C/${P}_left.png --json > review/multi_${P}.json
python -c "import json;print('degenerate', json.load(open('review/multi_${P}.json'))['degenerate'])"
python $SK/forge/stage4_review/turntable_gate.py --capture 0=$C/${P}_front.png --capture 90=$C/${P}_left.png \
  --capture 180=$C/${P}_rear.png --capture 270=$C/${P}_right.png --json > review/turntable_${P}.json; echo turntable rc=$?
m multi-angle-review review/multi_${P}.json
python $SK/forge/stage3_build/orchestrate_passes.py check object-sculpt-spec.json --pass-id $PASS > review/check_${P}.txt 2>&1
tail -1 review/check_${P}.txt
m pass-gate-check review/check_${P}.txt
python $SK/forge/stage4_review/make_comparison_sheet.py --reference reference.png --render $C/${P}_refview.png \
  --out review/cmp_${P}.png --json > /dev/null
python $SK/forge/next.py --state $S object-sculpt-spec.json 2>&1 | sed -n 1p
