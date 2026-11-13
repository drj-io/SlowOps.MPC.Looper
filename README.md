# Slow Ops Looper

A one-bar stereo looper for use as an insert effect on the Akai MPC standalone (built and tested for the Live III, MPC OS 3.10.1+).
You capture a bar from the track, overdub on top of it, and the older layers fade while the rest of the
project keeps playing. This repository holds the first working prototype.

## Layout

| Path | What |
|---|---|
| `core/include/slowops/looper/` | Header-only looper DSP: `LooperEngine`, `BarClock`, `StereoRing`, `Dsp`. Standard library only, with no plugin SDK, so it can be reused in a desktop plugin. |
| `plugin/` | MPC Plugin Builder SDK project: `plugin.json`, `ui.json`, `skins/`, and `src/SlowOpsLooper.cpp` (the thin adapter). |
| `tests/core_tests.cpp` | Engine unit tests with a synthetic transport. |
| `tests/scenario_host.cpp` | Loads the real plugin binary through the VST2 ABI, scripts tempo, bars, transport and button presses, and renders `ichigo-sim.wav`. |
| `scripts/` | `build.sh`, `test.sh`, `regen_skins.sh` |
| `docs/CONTROLS.md` | Control, automation and trigger matrix, plus the Live III test checklist. |
| `third_party/mpc-plugin-builder-sdk` | Official SDK as a git submodule, pinned to `febd2ca5bd9eb16c4a412837196e145566bc981b` (2026-11-07, "Add plugin format to plugin.json"). |

## Build and test

```sh
git submodule update --init
./scripts/test.sh        # build (native + arm32 + arm64), core tests, host_sim x2, scenario host
```

On the first run, `build.py` installs Zig into `third_party/mpc-plugin-builder-sdk/.sdk-tools/` (about 50 MB).
The output folder is `plugin/build/Slow Ops - VST - Slow Ops Looper/`. Copy the whole folder into
`VST2 Plugins/` at the root of a drive the MPC can see.

You edit the UI in `plugin/ui.json`, then run `./scripts/regen_skins.sh`, which regenerates `plugin/skins/`
and keeps the triggers off the Q-Links.

## How it behaves

- **Capture modes**:
  - **Last Bar** (default) grabs the latest finished bar.
  - **Rolling** grabs one bar's worth of audio ending at the tap.
  - **Next Bar** arms, then records from the next downbeat for one bar.
- **Play modes**:
  - **Freeze** repeats the loop.
  - **On Demand** (default) adds incoming audio while **Overdub** is on.
  - **Continuous** adds incoming audio on every pass.
  - While overdubbing, `loop = softclip(tone(loop) × Feedback + input)`, so each pass keeps Feedback × the previous one.
- **Timing**: tempo, time signature and bar position come from the MPC transport, and the loop phase-locks to
  host downbeats. When the transport is stopped, an internal bar clock at the project tempo keeps capture and
  playback running.
- **Mix**: an equal-power blend. Before the first capture, and after Clear, the track passes through untouched
  whatever Mix is set to.
- **Click-free**:
  - 10 ms seam crossfade at the loop point.
  - 20 ms crossfade between an old loop and a new capture.
  - 20 ms fade on the first capture and on Clear.
  - 10 ms ramp when overdub engages.
- **Status** shows No Hist, Ready, Armed, Rec Bar, Playing, Overdub or Too Long. A refused capture shows
  *No Hist* or *Too Long* for 2 s.
- **State**: all settings are saved with the project. Captured audio is temporary and is not saved.

## Limits of this prototype

- The longest bar is 6 s, i.e. 40 BPM in 4/4. A slower tempo shows *Too Long*.
- If the tempo changes after a capture, the loop keeps its captured length and stops snapping to downbeats.
- The loop survives a buffer-size change but not a sample-rate change (the MPC is fixed at 44.1 kHz).
- Memory is about 8.5 MB per instance, all allocated in `prepare()`: 2 × 6 s loop buffers plus about 12.6 s of history.
- CPU on the host_sim desktop run is < 0.1 % per block. ARM numbers still have to be measured on the Live III.
- The prototype is only verified in simulation; hardware checks are listed in `docs/CONTROLS.md`.

## Licence

MIT. The SDK is MIT-licensed by inMusic.
