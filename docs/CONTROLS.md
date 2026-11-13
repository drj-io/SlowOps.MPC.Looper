# Controls, automation and external triggering

What each control is, how it can be driven on an MPC insert, and what has been
**verified in the simulator** versus what still needs **verification on the
Live III**. The simulator evidence comes from `scripts/test.sh` (SDK `host_sim`
plus `tests/scenario_host.cpp`) at SDK revision `febd2ca`.

## Parameters

Parameter ids are permanent: projects find values by id. The shared controls and bank 1 keep the ids from the
single-loop prototype. The index order is what automation and Q-Link maps use.

| # | Name | Values | Default | Page | Program Q-Link | Automatable |
|---|---|---|---|---|---|---|
| 0 | Mix | 0–100 % (equal power, live vs. all banks) | 50 % | MIX | 4 | yes |
| 1 | Tone | 400 Hz … 18 kHz, 100 % = Flat | Flat | MIX | 8 | yes |
| 2 | Drift | 0–100 % | 15 % | MIX | 12 | yes |
| 3 | Capture Mode | Last Bar / Rolling / Next Bar | Next Bar | LOOPS | — | yes |
| 4 | Play Mode | Freeze / On Demand / Continuous | On Demand | LOOPS | — | yes |

Per bank *n* = 1, 2, 3, at index 5 + 10 × (*n* − 1) + offset:

| +offset | Name | Values | Default | Page | Program Q-Link | Automatable |
|---|---|---|---|---|---|---|
| +0 | Length *n* | 1 / 2 / 4 / 8 bars (for the next capture) | 4 bars | SOUND | 9, 10, 11 | yes |
| +1 | Volume *n* | 0–100 % (squared law, 100 % = unity) | 100 % | MIX | 1, 2, 3 | yes |
| +2 | Feedback *n* | 0–100 % | 100 % | SOUND | 5, 6, 7 | yes |
| +3 | Solo *n* | Off / On | Off | LOOPS | — | yes |
| +4 | Mute *n* | Off / On | Off | LOOPS | — | yes |
| +5 | Overdub *n* | Off / On (latched, used by On Demand) | Off | LOOPS | — | yes |
| +6 | Capture *n* | trigger | Off | LOOPS | — | yes |
| +7 | Clear *n* | trigger | Off | LOOPS | — | yes |
| +8 | Bank *n* | progress dial, shows "bar/bars" (e.g. 3/4) or "--" | — | LOOPS | — | **no** |
| +9 | Status *n* | No Hist / Ready / Armed / Rec Bar / Playing / Overdub / Too Long (also shown on the Capture *n* button) | — | DEBUG | — | **no** |

**Screen layout.** Each page has one column per bank (1, 2, 3). The fourth column holds the shared controls.

- **LOOPS:**
  - Across the top: four beat boxes. The box for the current beat lights, and the colour shows the state:
    cyan idle, orange waiting for the next 1, red recording, green playing, purple overdubbing.
  - Below them, per bank: the Bank dial, CAPTURE, then OVERDUB with SOLO and MUTE side by side under it,
    then PLAY/STOP and CLEAR side by side.
  - Shared column: METRONOME, Capture Mode, Play Mode, then the master PLAY/STOP and CLEAR ALL.
  - Each CAPTURE button shows its own bank's state in the same colours, with the state written underneath.
- **MIX:** Volume per bank, Play Timing (under the volumes), with Tone, Drift, Latency (next to Drift), Mix, Click Level, the Monitor switch (default On) next to Mix and the
  Auto Standby switch next to Click Level.
- **SOUND:** Length (1/2/4/8 bars) and Feedback per bank.
- **DEBUG:** what the MPC reports and what the plugin saw, plus Loop State and Status 1–3.

The buttons and the choice selectors are images with the text drawn in (`scripts/make_buttons.py`), so no text
label sits on top of a button and taps always reach it.

**Metronome.** A short pop on every beat, a little higher and louder on the 1. Click Level 50 % peaks at
about −19 dBFS. It sounds whether or not the MPC transport plays: stopped, it keeps time at the
project tempo; playing, it locks to the song position. It is added to the plugin's output after the
loops, so it is never recorded into a bank. It is part of the track's output, though, so it will be in a
resample or a bounce of that track.

The METRONOME button has three states, shown on the button. Each tap steps Off → On → Standby → Off.
- **Off:** silent.
- **On:** popping.
- **Standby:** silent for now, and switches back On by itself at the next CLEAR ALL, or when the last loop is
  cleared (bank by bank).

**Auto Standby** (MIX page, next to Click Level; on by default) is for laying down the first loop to the click: when the
first loop since CLEAR ALL is captured, the metronome goes from On to Standby. It does this once per
CLEAR ALL, so if you tap it back On it stays On. With Auto Standby off, On stays On.

Appended parameters (indices 43–58): Clear All (trigger), Metronome (Off / Standby / On; 0 / 0.5 / 1, so a
project saved when this was an Off/On switch loads the same), Click Level (0–100 %, default 50 %),
Auto Standby (Off/On, default On), Metro Tap (trigger: the METRONOME button), Monitor (Off/On, default On), Latency (−50…50 ms, default 12 ms), Play All (master PLAY/STOP, default On), Play 1–3 (per bank, default On), Play Timing (Immediate / Bar End /
Loop End, default Immediate), Play State 1–3 and Play All State (read-only: Stopped / Playing / Starting / Stopping).

**Monitor** (MIX page, next to Mix; on by default). On: the live input goes through the plugin and Mix
blends it with the loops. Off: the plugin outputs only the loops, at full level (Mix is ignored), and the
metronome; use it when the MPC already monitors the input, so you don't hear it twice.

**Latency** (MIX page, next to Drift; default 12 ms). You play along to what you hear, which is already a
little late, and your playing then takes a little longer to reach the plugin, so a loop's downbeat lands just
after the bar line. Latency makes loop playback run that far ahead so the downbeat sounds on the beat. 12 ms is
an estimate for the MPC XL at 44.1 kHz with 128-sample buffers. Turn it while a loop plays (it glides, no
clicks) until the loop sits with the MPC's drums: higher if the loop drags, lower if it rushes. It applies to
every loop, including ones already recorded, and overdubs stay in line with it.

**PLAY/STOP.** The buttons show the state: green with ▶ = playing, dark with ■ = stopped. Tap to change it.
- **Master** (bottom of the shared column) starts and stops the whole looper. It follows the MPC transport:
  Stop stops the looper, Play starts it again. Pressing it never starts the MPC transport. Pressed while the
  transport is stopped, the looper starts its own bar 1 right then and every loop plays from its beginning
  (an armed Next Bar capture starts recording at that moment).
- **Per bank** (next to CLEAR n) stops or starts that bank. A stopped bank keeps its place, so it comes back in
  time. A stopped bank does not overdub.
- Capturing into a bank turns that bank's PLAY on (and the master's, if it was stopped), so you hear what you
  captured. Stopping never loses a loop; only CLEAR does.
- **Play Timing** (MIX page): when a PLAY/STOP tap takes effect. **Immediate**: now (5 ms fade).
  **Bar End**: on the next bar line. **Loop End**: when that bank's loop comes round to its start (for the
  master, each bank at its own loop end). While a tap waits, the button turns amber and reads STARTING or
  STOPPING; a start on a boundary comes in at full level on its first sample. The transport always acts
  immediately (MPC Stop stops at once), and master PLAY from fully stopped always starts at once, from the top. Captures always
take the live input, whatever Monitor is set to.

**Solo and Mute** act on the loop banks only. The live track is always governed by Mix. When any bank is
soloed, only the soloed banks are heard. A bank that is both muted and soloed stays muted. Muted banks keep
playing (and overdubbing) silently, so they come back in time.

**Play Mode is shared.** In Continuous, every bank that holds a loop overdubs on every pass. In On Demand,
each bank overdubs while its own Overdub is on.

**Triggers.** The MPC skin format has no momentary button, only a toggle. Capture *n* and Clear *n*
therefore *self-reset*: setting the value to On fires the action once at the next audio block, and the plugin
writes the value back to Off and tells the MPC (`audioMasterAutomate`). A state or project load turns them
off without firing.

**Knobs.** The triggers and the read-only displays are left off every Q-Link, so nudging a knob can never
fire a capture.

**Read-only displays.** Status *n* and Bank *n* are written by the plugin and are not automatable. Progress
moves in 16th-note steps of the loop, and the MPC is told about a change at most every 40 ms per bank. In the
simulator that came to 8 updates per second for a 4-bar loop at 120 BPM.

## What can drive what

| Route | Feedback, Tone, Drift, Mix | Modes, Overdub | Capture, Clear | Evidence |
|---|---|---|---|---|
| Touchscreen | yes | yes | yes (tap the switch) | Skin generated and validated: `validate.py` reports 0 errors and all handles in range. **Hardware: confirm.** |
| Q-Links (screen and program mode) | yes | yes | no, by design | `plugin/skins/Q-Links.json` |
| Host automation lanes | yes | yes | yes: a lane step to On fires once | Sim: `effCanBeAutomated` = 1 for indices 0–8 and 0 for Status (`host_sim dump`). A `setParameter` from outside the audio block fires and self-resets (`scenario_host` "automation writes"). **Hardware: confirm that the lanes are offered for inserts and that the recorded steps fire.** |
| Project / preset load | restored | restored | never fire | Sim: `scenario_host` "State recall" (values and display text identical, loop empty, a chunk saved with Capture=On does not fire), and `host_sim` check 3. |
| MIDI notes or CC sent to the insert | — | — | — | **Not possible.** The MPC sends no MIDI to effect plugins (SDK `host-contract.md`), so a plugin-side MIDI learn cannot exist. |
| MPC MIDI Learn / Q-Link mapping from an external controller | ? | ? | ? | **Hardware only.** The SDK documents nothing here. If MPC OS can map an external CC to an insert parameter, all automatable parameters qualify. |

## MPC checklist (Live III / XL)

Before you start, check MPC OS is 3.10.1 or later (Preferences → Info). Then turn on
Preferences → Enhancements (Beta) → **Enable Enhancements** and **External Plugin Support**.

After installing a new build, save the project and restart the MPC. It only rereads the screen layout on
restart.

1. Insert **Slow Ops Looper** on the *Ichigo* track, with drums on their own track. Play: the track sounds
   unchanged, and the beat boxes count 1–4 in cyan.
2. Tap **CAPTURE** in bank 1 (every bank defaults to 4 bars and Next Bar). The beat boxes and the button turn
   orange ("WAITING FOR 1"), red from the next 1 for four bars ("RECORDING"), then green ("PLAYING"). The Bank 1
   dial steps through 1/4 … 4/4.
3. Capture banks 2 and 3 the same way. On SOUND, try 1, 2 and 8 bars.
4. On MIX change Volume 1–3; on LOOPS tap Solo 2, then Mute 3. Only the loops change; the live track does not.
5. Overdub 1 on with Play Mode = On Demand: play a new phrase and leave it on. Bank 1's older layers fade at
   Feedback 1 while banks 2 and 3 stay unchanged.
6. **CLEAR** in bank 2: bank 2 fades out, and the other banks keep playing. **CLEAR ALL** empties every bank.
7. On SOUND, set Length 1 to 8 Bars and capture again. Below 60 BPM in 4/4, Status shows **Too Long**.
8. Record automation for Capture 2 and Mute 3, and check that it plays back. Try MIDI Learn on Volume 1.
9. Save and reload the project: the settings come back, all banks are empty, and the dials show "--".
10. Watch the CPU meter with all three banks overdubbing at a 128-sample buffer.

Report back: do the dials move smoothly, does Status update by itself, and does the CPU meter look sane?
