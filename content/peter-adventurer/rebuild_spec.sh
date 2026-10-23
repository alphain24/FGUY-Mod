#!/bin/sh
# Regenerates object-sculpt-spec.json from the assessment, then authors it (build_spec.py) and strict-validates.
# The review record (reviewHistory, visualEvidence, tier1Results, sculptPipeline) is carried over from the
# previous spec, so a rebuild never erases a recorded review.
SK=~/.claude/skills/img2threejs
export PYTHONUTF8=1
mkdir -p .work
[ -f object-sculpt-spec.json ] && cp object-sculpt-spec.json .work/prev-spec.json
python $SK/forge/stage2_spec/new_sculpt_spec.py "Peter adventurer outfit" --image reference.png --assessment assessment.json --domain generic --out object-sculpt-spec.json --force >/dev/null || exit 1
python - <<'EOF' || exit 1
import json, os
if os.path.exists('.work/prev-spec.json'):
    prev = json.load(open('.work/prev-spec.json', encoding='utf-8'))
    spec = json.load(open('object-sculpt-spec.json', encoding='utf-8'))
    for k in ('reviewHistory', 'visualEvidence', 'tier1Results', 'sculptPipeline'):
        if k in prev:
            spec[k] = prev[k]
    json.dump(spec, open('object-sculpt-spec.json', 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
EOF
python build_spec.py || exit 1
python $SK/forge/stage2_spec/validate_sculpt_spec.py object-sculpt-spec.json --strict-quality
