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

# Labelled image buttons (text drawn into the PNGs by scripts/make_buttons.py, so no
# Label sits on top of a Button). Each goes into an empty slot of the 4 x 4 grid:
#   LOOPS rows 2-4: CAPTURE n / OVERDUB n / CLEAR n     MIX rows 2-3: SOLO n / MUTE n
# CAPTURE n has one layer per Status n choice (IndexedEnabling), so the button shows
# the bank's state: grey no history, cyan ready, orange waiting, red recording,
# green playing, purple overdubbing.
import glob, shutil
for f in glob.glob('plugin/assets/buttons/*.png'):
    shutil.copy(f, 'plugin/skins/')
SLOT_X, SLOT_Y = [23, 343, 663, 983], [45, 196, 346, 497]
d = json.load(open(t))
tabs = {tb['tabName']: tb['componentDefinition'] for tb in d['pageData']['tabs']}
reps = {r['key'] for r in d['pageData']['replacementStrings']}
def rep(h, values):
    if h not in reps:
        d['pageData']['replacementStrings'].append({'key': h, 'value': values}); reps.add(h)
def button(name, img, h, col, row, only=None):
    b = {'version': 2 if only else 1, 'acceptsHWFocus': 'No', 'showWhenDataModelInvalid': 'Hide', 'whenVisible': 'Always',
         'boundsType': 'Absolute', 'bounds': '%d %d 248 84' % (SLOT_X[col] + 15, SLOT_Y[row] + 12)}
    if only: b['additionalInvalidatingHandles'] = [only]
    return {'version': 2, 'componentData': {'version': 1, 'name': name, 'type': 'Button',
            'data': {'version': 2, 'onImage': img + '_on.png', 'offImage': img + '_off.png', 'buttonId': 1,
                     'numButtonsInGroup': 1, 'handleName': h, 'gestureBehaviour': 'Instant'}},
            'handle remapping': {'version': 1, 'map': []}, 'bounds': b}
STATUS_LOOKS = ['nohist', 'ready', 'armed', 'rec', 'play', 'dub', 'toolong']
nb = 0
for bank in range(3):
    n = bank + 1
    P = lambda nm: 'Parameter %d' % names['%s %d' % (nm, n)]
    st = P('Status')
    rep(st, ['No Hist', 'Ready', 'Armed', 'Rec Bar', 'Playing', 'Overdub', 'Too Long'])
    for nm in ('Capture', 'Overdub', 'Clear', 'Solo', 'Mute'):
        rep(P(nm), ['Off', 'On'])
    for k, look in enumerate(STATUS_LOOKS):
        tabs['LOOPS']['componentsData'].append(button('Capture %d %s' % (n, look), 'capture_' + look, P('Capture'), bank, 1,
                                                       'IndexedEnabling/%d/7/%s' % (k, st)))
    tabs['LOOPS']['componentsData'].append(button('Overdub %d' % n, 'overdub', P('Overdub'), bank, 2))
    tabs['LOOPS']['componentsData'].append(button('Clear %d' % n, 'clear', P('Clear'), bank, 3))
    tabs['MIX']['componentsData'].append(button('Solo %d' % n, 'solo', P('Solo'), bank, 1))
    tabs['MIX']['componentsData'].append(button('Mute %d' % n, 'mute', P('Mute'), bank, 2))
    nb += 11
json.dump(d, open(t, 'w'), indent=2)
print('patched', t, '(%d image buttons)' % nb)

# Screen-mode knobs next to the buttons: Overdub n on LOOPS row 3 (a knob turn toggles
# it), nothing on Capture / Clear rows. Solo / Mute on MIX rows 2-3.
q = json.load(open(p))
def slot_qlink(row, col): return 13 + col - 4 * row
for e in q['Screen Mode Q-Links']['map']:
    tab_name = d['pageData']['tabs'][e['Tab'] - 1]['tabName']
    for bank in range(3):
        if tab_name == 'LOOPS':
            e['Q-Links']['Q-Link %d' % slot_qlink(2, bank)] = names['Overdub %d' % (bank + 1)]
        if tab_name == 'MIX':
            e['Q-Links']['Q-Link %d' % slot_qlink(1, bank)] = names['Solo %d' % (bank + 1)]
            e['Q-Links']['Q-Link %d' % slot_qlink(2, bank)] = names['Mute %d' % (bank + 1)]
json.dump(q, open(p, 'w'), indent=2)
print('patched', p, '(knobs next to Overdub / Solo / Mute)')

# Choice parameters as one-tap radio groups of image buttons (captions drawn into the
# images), with a plain name label above them: Capture Mode / Play Mode on LOOPS,
# Length n on SOUND.
d = json.load(open(t))
tabs = {tb['tabName']: tb['componentDefinition'] for tb in d['pageData']['tabs']}
reps = {r['key'] for r in d['pageData']['replacementStrings']}
def radio(tab, pname, choices, col, row, width):
    h = 'Parameter %d' % names[pname]
    if h not in reps:
        d['pageData']['replacementStrings'].append({'key': h, 'value': choices}); reps.add(h)
    x0, y0 = SLOT_X[col], SLOT_Y[row]
    entries = tabs[tab]['componentsData']
    entries.append({'version': 2, 'componentData': {'version': 1, 'name': pname + ' name', 'type': 'Label',
                    'data': {'version': 1, 'textStyle': {'version': 1, 'font': {'version': 1, 'name': 'Titillium Web', 'style': 'Regular', 'height': 24.0},
                                                          'colour': 'ff9a9aa0', 'justification': 'left', 'case': 'Original'},
                             'type': 'Name', 'handleName': h}},
                    'handle remapping': {'version': 1, 'map': []},
                    'bounds': {'version': 1, 'acceptsHWFocus': 'No', 'showWhenDataModelInvalid': 'Hide', 'whenVisible': 'Always',
                               'boundsType': 'Absolute', 'bounds': '%d %d 248 34' % (x0 + 15, y0 + 2)}})
    gap = 4
    total = len(choices) * width + (len(choices) - 1) * gap
    for k, c in enumerate(choices):
        img = 'seg_%s_%d' % (c.lower().replace(' ', ''), width)
        entries.append({'version': 2, 'componentData': {'version': 1, 'name': '%s %s' % (pname, c), 'type': 'Button',
                        'data': {'version': 2, 'onImage': img + '_on.png', 'offImage': img + '_off.png', 'buttonId': k,
                                 'numButtonsInGroup': len(choices), 'handleName': h, 'gestureBehaviour': 'Instant'}},
                        'handle remapping': {'version': 1, 'map': []},
                        'bounds': {'version': 1, 'acceptsHWFocus': 'No', 'showWhenDataModelInvalid': 'Hide', 'whenVisible': 'Always',
                                   'boundsType': 'Absolute', 'bounds': '%d %d %d 52' % (x0 + (278 - total) // 2 + k * (width + gap), y0 + 44, width)}})
radio('LOOPS', 'Capture Mode', ['Last Bar', 'Rolling', 'Next Bar'], 3, 1, 86)
radio('LOOPS', 'Play Mode', ['Freeze', 'On Demand', 'Continuous'], 3, 2, 86)
for bank in range(3):
    radio('SOUND', 'Length %d' % (bank + 1), ['1 Bar', '2 Bars', '4 Bars', '8 Bars'], bank, 0, 64)
json.dump(d, open(t, 'w'), indent=2)
q = json.load(open(p))
for e in q['Screen Mode Q-Links']['map']:
    tab_name = d['pageData']['tabs'][e['Tab'] - 1]['tabName']
    if tab_name == 'LOOPS':
        e['Q-Links']['Q-Link %d' % slot_qlink(1, 3)] = names['Capture Mode']
        e['Q-Links']['Q-Link %d' % slot_qlink(2, 3)] = names['Play Mode']
    if tab_name == 'SOUND':
        for bank in range(3):
            e['Q-Links']['Q-Link %d' % slot_qlink(0, bank)] = names['Length %d' % (bank + 1)]
json.dump(q, open(p, 'w'), indent=2)
print('patched', t, '(radio image buttons: Capture Mode, Play Mode, Length 1-3)')
PY
