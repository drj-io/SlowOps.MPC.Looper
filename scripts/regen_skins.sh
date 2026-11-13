#!/usr/bin/env bash
# Regenerate plugin/skins/ from plugin/ui.json, then keep the one-shot triggers
# (Capture, Clear) and the read-only Status off the Q-Link knobs, so turning a
# knob never fires a capture by accident. build.py copies plugin/skins/ verbatim.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/third_party/mpc-plugin-builder-sdk"
cd "$ROOT"
[ -f plugin/build/params.json ] || python3 "$SDK/tools/build.py" plugin --target native
rm -rf plugin/skins
python3 "$SDK/tools/gen_ui.py" --params plugin/build/params.json --ui plugin/ui.json --out plugin/skins --kit-cache plugin/build
python3 - <<'PY'
import json
p = 'plugin/skins/Q-Links.json'
d = json.load(open(p))
names = {x['name']: x['index'] for x in json.load(open('plugin/build/params.json'))['params']}
off = {names['Capture'], names['Clear'], names['Status']}
for e in d['Screen Mode Q-Links']['map']:
    for k, v in e['Q-Links'].items():
        if v in off: e['Q-Links'][k] = -1
# Program mode: Feedback, Tone, Drift, Mix, Capture Mode, Play Mode, Overdub on Q-Links 1-7.
d['Program Mode Q-Links'] = {'Q-Link %d' % q: (q - 1 if q <= 7 else -1) for q in range(1, 17)}
json.dump(d, open(p, 'w'), indent=2)
print('patched', p)
PY
