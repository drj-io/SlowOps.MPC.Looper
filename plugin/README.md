# Slow Ops Looper

A three-bank stereo looper used as an insert effect on Akai MPC standalone hardware, by Slow Ops.

Each bank captures 1, 2, 4 or 8 bars from the track (Last Bar, Rolling or Next Bar), then freezes it or
overdubs onto it. Each bank's Feedback sets how much of each earlier pass survives, so older layers fade while
the project keeps playing. Each bank has its own volume, solo, mute and a progress dial. Tone darkens the loops,
Drift adds slow tape wow, and Mix blends the live track with the loops.

## Install

1. Copy the folder `Slow Ops - VST - Slow Ops Looper` into a folder named `VST2 Plugins` at the root of a USB
   drive, an SD card, or the MPC's internal drive.
2. The MPC needs MPC OS 3.10.1 or later. Turn on Preferences → Enhancements (Beta) → **Enable Enhancements**
   and **External Plugin Support**.
3. Add it to a track as an insert: it is listed under External Plugins.

## Controls

Each page has one column per bank. The fourth column holds the shared controls.

- **LOOPS:** progress dials (bar/bars), Status, Capture and Overdub per bank, plus Mix, Capture Mode and Play
  Mode.
- **MIX:** Volume, Solo, Mute and Clear per bank, plus Tone and Drift.
- **SOUND:** Length (1/2/4/8 bars) and Feedback per bank.
- **Capture** and **Clear**: tap them. They fire once and switch back off. **Overdub** stays on until you tap it
  again.
- **Q-Links in Program mode:** Volume 1–3 and Mix on 1–4, Feedback 1–3 and Tone on 5–8, Length 1–3 and Drift on
  9–12.

Full details are in `docs/CONTROLS.md` in the source repository.

Settings are saved with the project. Captured audio is not: a reloaded project starts with an empty loop.

## Status: prototype

Verified in the SDK host simulator and a scripted-transport test host. Not yet verified on hardware. CPU and
the screen behaviour still need checking on a Live III.

## Source

The looper DSP lives in `core/` of the source repository and is shared with future desktop builds. This folder
holds only the MPC adapter. Build everything with `scripts/build.sh` from the repository root.

Licence: MIT.
