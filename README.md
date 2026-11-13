# Slow Ops Looper

A stereo looper with three loop banks, for use as an insert effect on the Akai MPC standalone
(MPC OS 3.10.1+; tested on an MPC XL, built for the Live III and other 64-bit MPCs).
Each bank captures 1, 2, 4 or 8 bars from the track. You overdub on top, and the older layers fade while the
rest of the project keeps playing. Each bank has its own volume, solo, mute and a progress dial.
This repository holds the working prototype.

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

- **Banks.** There are three independent loops. Each has a Length (1, 2, 4 or 8 bars), Feedback, Volume, Solo,
  Mute, Capture, Overdub and Clear, plus a progress dial and a Status display. Capture Mode, Play Mode, Tone,
  Drift and Mix are shared.
- **Capture modes** (N = the bank's length):
  - **Last Bar** takes the last N complete bars.
  - **Rolling** takes the N bars ending at the tap.
  - **Next Bar** arms, then records N bars from the next downbeat.
  - In every mode the loop continues seamlessly from where the captured audio ended.
- **Play modes:**
  - **Freeze** repeats the loop.
  - **On Demand** overdubs each bank while its Overdub is on.
  - **Continuous** overdubs every bank on every pass.
  - Overdubbing writes `loop = softclip(tone(loop) × Feedback + input)`.
- **Timing.** Tempo, time signature and bar position come from the MPC transport. Loops snap to the host's bar
  lines. While the transport is stopped, an internal bar clock at the project tempo keeps everything running.
- **Mix.** An equal-power blend of the live track and the sum of the banks. Before any capture, the track
  passes through untouched.
- **Click-free.** 10 ms seam crossfade at the loop point, 20 ms crossfade when re-capturing into a playing
  bank, and 20 ms ramps for the first capture, Clear, volume, solo and mute.
- **State.** All settings are saved with the project. Captured audio is temporary.

## Limits of this prototype

- Each bank holds up to 32 s. That's 8 bars down to 60 BPM in 4/4, or 4 bars down to 30 BPM. A longer bank
  shows *Too Long*.
- Memory is about 58 MB per instance, all allocated in `prepare()`: 4 pooled 32 s stereo buffers (3 banks plus
  a spare for crossfading re-captures) and 37 s of history.
- A loop keeps its captured length. If the tempo changes, it stops snapping to bar lines. Changing Length only
  affects the next capture.
- One capture copies at a time: re-capturing several full banks at once queues them, each a few ms apart.
- CPU is < 0.2 % per block on the desktop simulator. ARM figures still need measuring on the MPC.

## Licence

MIT. The SDK is MIT-licensed by inMusic.
