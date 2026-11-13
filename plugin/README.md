# Slow Ops Looper 1.0

A three-bank stereo looper for Akai MPC standalone. It sits as an insert effect on a track and captures
1, 2, 4 or 8 bars of what that track is playing, in time with the MPC. You can overdub onto each loop,
start and stop banks on the beat, and practise with the built-in metronome.

Made by Slow Ops. Free, MIT licence. Third-party plugin: Akai does not support it; use at your own risk.

## Install

You need MPC OS 3.10.1 or later.

1. Unzip. Copy the folder `Slow Ops - VST - Slow Ops Looper` into a folder named `VST2 Plugins` at the top
   level of a USB drive, an SD card or the MPC's internal drive. Create `VST2 Plugins` if it isn't there.
2. On the MPC, open **Preferences → Enhancements (Beta)** and switch on **Enable Enhancements** and
   **External Plugin Support**.
3. Add **Slow Ops Looper** (under External Plugins) as an insert on the track you want to loop. Restart the MPC
   if it doesn't appear.

`INSTALL.txt` has the same steps in more detail.

## Quick start

1. Put the looper on an audio track that is monitoring your input (or on a track playing back audio).
2. On **LOOPS**, tap **METRONOME** if you want a click, then tap **CAPTURE 1**. With the default Capture Mode
   (**Next Bar**) it waits for the next "1" (amber), records the bar length set for bank 1 (red), then plays it
   back (green).
3. Tap **OVERDUB 1** to layer more onto it, and tap it again to stop. Then capture banks 2 and 3 the same way.

## Pages

**LOOPS** is the page you play from. Each bank has a column, and the fourth column holds the shared controls.

- The **beat boxes** along the top show the beat of the bar. Their colour shows what the looper is doing:
  cyan idle, amber waiting for the 1, red recording, green playing, purple overdubbing.
- The **Bank dial** shows where the bank is in its loop (for example 3/4 means bar 3 of 4).
- **CAPTURE n** records into the bank and shows its state: Ready, Waiting for 1, Recording, Playing, Overdub,
  No History or Too Long. Capturing into a bank that already has a loop replaces it with a crossfade.
- **OVERDUB n** layers new playing onto the loop while it is on (in the On Demand play mode).
- **SOLO n / MUTE n** act on that bank only. The live track is never soloed or muted.
- **PLAY/STOP n** starts and stops the bank without losing it. **CLEAR n** erases it.
- **METRONOME** cycles through Off → On → Standby. Standby means it is silent for now and switches back On at the
  next CLEAR ALL.
- **Capture Mode** controls when a capture starts. **Last Bar** takes the bar(s) you just played. **Rolling**
  takes the last N bars up to now. **Next Bar** waits for the next "1" and records from there.
- **Play Mode** controls what happens to the loop as it plays. **Freeze** never changes the loop. **On Demand**
  overdubs while OVERDUB is on. **Continuous** overdubs all the time.
- **PLAY/STOP ALL** is the master. It follows the MPC transport: Stop stops the looper and Play starts it.
  Pressed with the transport stopped, it starts every loop from the top without starting the MPC.
  **CLEAR ALL** erases every bank.

**MIX**

- **Volume 1–3**.
- **Play Timing** sets when PLAY/STOP taps take effect: **Immediate**, **Bar End** (the next bar line) or
  **Loop End** (when that bank's loop comes back round). A waiting tap shows STARTING or STOPPING in amber.
- **Latency** makes loops play slightly early to make up for the MPC's audio delay. The default is 12 ms. If
  your loops sound late against the MPC's drums, turn it up; if they sound early, turn it down. It applies to
  loops already recorded, so you can adjust it while a loop plays.
- **Monitor** (on by default): the live input goes through the plugin and **Mix** blends it with the loops.
  Turn Monitor off if the MPC already monitors your input and you hear it twice. The loops then play at full
  level and Mix has no effect.
- **Auto Standby** (on by default): the metronome goes to Standby as soon as your first loop is captured, so
  the click is only there while you lay down the first loop.
- **Tone** darkens the loops, and the layers below get darker on each overdub. **Drift** adds slow tape wobble.
  **Mix** blends the live track with the loops. **Click Level** sets the metronome volume.

**SOUND**

- **Length 1–3** sets the length of the bank's next capture: 1, 2, 4 or 8 bars. The default is 4.
- **Feedback 1–3** sets how much of the loop survives each overdub pass. At the default of 100 % layers stack
  and nothing fades. Turn it down for a tape-echo fade.

## Good to know

- **Loops are not saved with the project.** Settings are, but a reloaded project starts with empty banks.
  Resample the track to keep what you made.
- The metronome is added to the plugin's output after the loops. It is never captured into a bank, but it will
  be in a resample of the track.
- The looper follows the MPC's tempo and time signature. Changing the tempo after capturing does not stretch a
  loop.
- Each bank holds up to 32 seconds, so 8 bars at a very slow tempo shows "Too Long".

## Tested on

An MPC XL running MPC OS 3.10.1. It should run on any MPC that supports external plugins, but this is the only
model it has been tested on.

## Source and licence

Source code: https://github.com/drj-io/SlowOps.MPC.Looper (private for now). MIT licence. Built with the
inMusic MPC Plugin Builder SDK (MIT). "MPC" and "Akai Professional" are trademarks of inMusic Brands, Inc.
"VST" is a trademark of Steinberg Media Technologies GmbH.
