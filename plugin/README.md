# Slow Ops Looper

A one-bar stereo looper used as an insert effect on Akai MPC standalone hardware, by Slow Ops.

You capture a bar from the track (Last Bar, Rolling or Next Bar), then freeze it or overdub onto it. Feedback
sets how much of each earlier pass survives, so older layers fade while the project keeps playing. Tone darkens
the loop, Drift adds slow tape wow, and Mix blends the live track with the loop.

## Install

1. Copy the folder `Slow Ops - VST - Slow Ops Looper` into a folder named `VST2 Plugins` at the root of a USB
   drive, an SD card, or the MPC's internal drive.
2. The MPC needs MPC OS 3.10.1 or later. Turn on Preferences → Enhancements (Beta) → **Enable Enhancements**
   and **External Plugin Support**.
3. Add it to a track as an insert: it is listed under External Plugins.

## Controls

- **Feedback, Tone, Drift, Mix**: knobs on screen and Q-Links 1–4.
- **Capture Mode** and **Play Mode**: segment switches.
- **Capture** and **Clear**: tap them. They fire once and switch back off.
- **Overdub**: stays on until you tap it again.
- **Status**: shows No Hist, Ready, Armed, Rec Bar, Playing, Overdub or Too Long.

Full details are in `docs/CONTROLS.md` in the source repository.

Settings are saved with the project. Captured audio is not: a reloaded project starts with an empty loop.

## Status: prototype

Verified in the SDK host simulator and a scripted-transport test host. Not yet verified on hardware. CPU and
the screen behaviour still need checking on a Live III.

## Source

The looper DSP lives in `core/` of the source repository and is shared with future desktop builds. This folder
holds only the MPC adapter. Build everything with `scripts/build.sh` from the repository root.

Licence: MIT.
