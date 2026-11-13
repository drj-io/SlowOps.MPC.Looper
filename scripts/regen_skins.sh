#!/usr/bin/env bash
# Regenerate plugin/skins/ from plugin/ui.json, then keep the one-shot triggers
# (Capture n, Clear n) and the read-only displays (Bank n, Status n) off the
# Q-Link knobs, so turning a knob never fires a capture by accident. build.py copies plugin/skins/ verbatim.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/third_party/mpc-plugin-builder-sdk"
cd "$ROOT"
python3 "$SDK/tools/build.py" plugin --target native
rm -rf plugin/skins
python3 "$SDK/tools/gen_ui.py" --params plugin/build/params.json --ui plugin/ui.json --out plugin/skins --kit-cache plugin/build
python3 - <<'PY'
import json
p = 'plugin/skins/Q-Links.json'
d = json.load(open(p))
names = {x['name']: x['index'] for x in json.load(open('plugin/build/params.json'))['params']}
# One-shot triggers and read-only displays never go on a knob.
off = {i for n, i in names.items() if n.split(' ')[0] in ('Capture', 'Clear', 'Status', 'Bank')}
for e in d['Screen Mode Q-Links']['map']:
    for k, v in e['Q-Links'].items():
        if v in off: e['Q-Links'][k] = -1
# Program mode (4x4, Q-Link 1 = bottom left): one column per bank plus a shared column.
#   Q-Links 1-4:  Volume 1-3, Mix      Q-Links 5-8:  Feedback 1-3, Tone
#   Q-Links 9-12: Length 1-3, Drift    Q-Links 13-16: unassigned
prog = ['Volume 1', 'Volume 2', 'Volume 3', 'Mix',
        'Feedback 1', 'Feedback 2', 'Feedback 3', 'Tone',
        'Length 1', 'Length 2', 'Length 3', 'Drift']
d['Program Mode Q-Links'] = {'Q-Link %d' % (q + 1): (names[prog[q]] if q < len(prog) else -1) for q in range(16)}
json.dump(d, open(p, 'w'), indent=2)
print('patched', p)

# Segment captions: gen_ui (SDK febd2ca) binds them to an unresolved "Data" handle
# with showWhenDataModelInvalid "Hide", so the MPC hides them (seen on hardware).
# Show them unconditionally and shrink the font so "Continuous" fits 80 px.
t = 'plugin/skins/TUI.json'
d = json.load(open(t))
n = 0
for tab in d['pageData']['tabs']:
    for e in tab['componentDefinition']['componentsData']:
        cd = e['componentData']
        if cd['type'] == 'Label' and ' seg ' in cd['name']:
            e['bounds']['showWhenDataModelInvalid'] = 'Show'
            cd['data']['textStyle']['font']['height'] = 18.0
            n += 1
json.dump(d, open(t, 'w'), indent=2)
print('patched', t, '(%d segment captions)' % n)
PY
