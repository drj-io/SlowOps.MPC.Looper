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
params = json.load(open('plugin/build/params.json'))['params']
off = {x['index'] for x in params if not x['automatable']} | \
      {i for n, i in names.items() if n.split(' ')[0] in ('Capture', 'Clear')}
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

# Segment captions: gen_ui (SDK febd2ca) adds them at tab level bound to "Data",
# which does not resolve there. The MPC hid them (showWhenDataModelInvalid Hide);
# forcing them visible made them swallow taps, so no segment switch responded
# (seen on hardware). Bind each caption to its switch's own parameter instead:
# a valid Text label, which the MPC draws and which lets touches through.
t = 'plugin/skins/TUI.json'
d = json.load(open(t))
n = 0
for tab in d['pageData']['tabs']:
    entries = tab['componentDefinition']['componentsData']
    handles = {e['componentData']['name']: e['handle remapping']['map'][0]['value']
               for e in entries if e['componentData']['type'].startswith('radio')}
    for e in entries:
        cd = e['componentData']
        if cd['type'] == 'Label' and ' seg ' in cd['name']:
            e['handle remapping'] = {'version': 1, 'map': [{'key': 'Data', 'value': handles[cd['name'].rsplit(' seg ', 1)[0]]}]}
            e['bounds']['showWhenDataModelInvalid'] = 'Hide'
            cd['data']['textStyle']['font']['height'] = 18.0
            n += 1
json.dump(d, open(t, 'w'), indent=2)
print('patched', t, '(%d segment captions)' % n)

# Beat counter: four boxes across the top of the LOOPS page, a radio group bound to
# the read-only "Beat" parameter (the lit box is the current beat of the bar).
import struct, zlib
def png(path, w, h, fill, border):
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            edge = x < 2 or y < 2 or x >= w - 2 or y >= h - 2
            corner = (x < 4 or x >= w - 4) and (y < 4 or y >= h - 4) and not (2 <= x < w - 2 and 2 <= y < h - 2) and False
            c = border if edge else fill
            row += bytes(c)
        rows.append(bytes(row))
    raw = b''.join(rows)
    def chunk(t, data): return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
W, H, Y = 248, 30, 8
# Loop State choices: Idle, Armed, Rec, Play, Dub -> idle cyan, orange, red, green, purple.
COLOURS = [('idle', (0x19, 0xc3, 0xe6)), ('armed', (0xff, 0x8c, 0x1a)), ('rec', (0xe8, 0x2a, 0x2a)),
           ('play', (0x2e, 0xc8, 0x4e)), ('dub', (0xa0, 0x4c, 0xe8))]
for name, (r, g, b) in COLOURS:
    png('plugin/skins/beat_%s_on.png' % name, W, H, (r, g, b, 255), (min(255, r + 90), min(255, g + 90), min(255, b + 90), 255))
    dim = lambda c: int(c * 0.28 + 0x22 * 0.72)
    png('plugin/skins/beat_%s_off.png' % name, W, H, (dim(r), dim(g), dim(b), 255), (min(255, dim(r) + 40), min(255, dim(g) + 40), min(255, dim(b) + 40), 255))
for old in ('beat_on.png', 'beat_off.png'):
    import os
    if os.path.exists('plugin/skins/' + old): os.remove('plugin/skins/' + old)
beat = names['Beat']
state = names['Loop State']
handle = 'Parameter %d' % beat
shandle = 'Parameter %d' % state
d = json.load(open(t))
d['pageData']['replacementStrings'].append({'key': handle, 'value': ['1', '2', '3', '4']})
d['pageData']['replacementStrings'].append({'key': shandle, 'value': ['Idle', 'Armed', 'Rec', 'Play', 'Dub']})
tab = d['pageData']['tabs'][0]['componentDefinition']
def bounds(b, show='Show', only=None):
    out = {'version': 2 if only else 1, 'acceptsHWFocus': 'No', 'showWhenDataModelInvalid': show, 'whenVisible': 'Always',
           'boundsType': 'Absolute', 'bounds': b}
    if only: out['additionalInvalidatingHandles'] = [only]
    return out
strip = []
for si, (name, _) in enumerate(COLOURS):
    for k in range(4):
        x = 38 + 320 * k
        strip.append({'version': 2, 'componentData': {'version': 1, 'name': 'beat %s %d' % (name, k + 1), 'type': 'Button',
                      'data': {'version': 2, 'onImage': 'beat_%s_on.png' % name, 'offImage': 'beat_%s_off.png' % name, 'buttonId': k,
                               'numButtonsInGroup': 4, 'handleName': handle, 'gestureBehaviour': 'Instant'}},
                      'handle remapping': {'version': 1, 'map': []},
                      'bounds': bounds('%d %d %d %d' % (x, Y, W, H), 'Hide', 'IndexedEnabling/%d/5/%s' % (si, shandle))})
for k in range(4):
    x = 38 + 320 * k
    strip.append({'version': 2, 'componentData': {'version': 1, 'name': 'beat caption %d' % (k + 1), 'type': 'Label',
                  'data': {'version': 1, 'textStyle': {'version': 1, 'font': {'version': 1, 'name': 'Titillium Web', 'style': 'SemiBold', 'height': 22.0},
                                                        'colour': 'ffffffff', 'justification': 'horizontallyCentred verticallyCentred', 'case': 'Original'},
                           'type': 'Text', 'handleName': handle, 'text': str(k + 1)}},
                  'handle remapping': {'version': 1, 'map': []}, 'bounds': bounds('%d %d %d %d' % (x, Y, W, H))})
tab['componentsData'][1:1] = strip
json.dump(d, open(t, 'w'), indent=2)
print('patched', t, '(beat strip bound to', handle + ')')
PY
