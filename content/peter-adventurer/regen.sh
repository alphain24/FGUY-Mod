#!/bin/sh
# rebuild the spec, regenerate the factory for a pass ($1, default blockout) and strip its types for the review page
SK=~/.claude/skills/img2threejs
export PYTHONUTF8=1
sh rebuild_spec.sh | tail -1
python $SK/forge/stage3_build/generate_threejs_factory.py object-sculpt-spec.json --out src/createObjectModel.ts --pass-id ${1:-material-pass} --force | tail -1
node -e "const m=require('node:module');const fs=require('fs');fs.writeFileSync('src/createObjectModel.js',m.stripTypeScriptTypes(fs.readFileSync('src/createObjectModel.ts','utf8'),{mode:'transform'}))" 2>/dev/null
