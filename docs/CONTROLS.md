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
| 0 | Mix | 0–100 % (equal power, live vs. all banks) | 50 % | LOOPS | 4 | yes |
| 1 | Tone | 400 Hz … 18 kHz, 100 % = Flat | Flat | MIX | 8 | yes |
| 2 | Drift | 0–100 % | 15 % | MIX | 12 | yes |
| 3 | Capture Mode | Last Bar / Rolling / Next Bar | Last Bar | LOOPS | — | yes |
| 4 | Play Mode | Freeze / On Demand / Continuous | On Demand | LOOPS | — | yes |

Per bank *n* = 1, 2, 3, at index 5 + 10 × (*n* − 1) + offset:

| +offset | Name | Values | Default | Page | Program Q-Link | Automatable |
|---|---|---|---|---|---|---|
| +0 | Length *n* | 1 / 2 / 4 / 8 bars (for the next capture) | 1, 2, 4 bars | SOUND | 9, 10, 11 | yes |
| +1 | Volume *n* | 0–100 % (squared law, 100 % = unity) | 100 % | MIX | 1, 2, 3 | yes |
| +2 | Feedback *n* | 0–100 % | 70 % | SOUND | 5, 6, 7 | yes |
| +3 | Solo *n* | Off / On | Off | MIX | — | yes |
| +4 | Mute *n* | Off / On | Off | MIX | — | yes |
| +5 | Overdub *n* | Off / On (latched, used by On Demand) | Off | LOOPS | — | yes |
| +6 | Capture *n* | trigger | Off | LOOPS | — | yes |
| +7 | Clear *n* | trigger | Off | MIX | — | yes |
| +8 | Bank *n* | progress dial, shows "bar/bars" (e.g. 3/4) or "--" | — | LOOPS | — | **no** |
| +9 | Status *n* | No Hist / Ready / Armed / Rec Bar / Playing / Overdub / Too Long | — | LOOPS | — | **no** |

**Screen layout.** Each page has one column per bank (1, 2, 3). The fourth column holds the shared controls:

- **LOOPS:** Bank dials · Status · Capture · Overdub, with Mix, Capture Mode and Play Mode.
- **MIX:** Volume · Solo · Mute · Clear, with Tone and Drift.
- **SOUND:** Length · Feedback.

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
   unchanged. On LOOPS, Status 1 reads **Ready** after a bar, and Status 3 after 4 bars.
2. Tap **Capture 1** while the motif plays. Bank 1 should loop the last bar in time, and its dial should go
   round once per bar, showing 1/1. Capture 1 should flip back to Off by itself.
3. Play a different part and tap **Capture 3** (4 bars). The Bank 3 dial should step through 1/4 … 4/4. Then
   tap **Capture 2** (2 bars).
4. On MIX: change Volume 1–3, then Solo 2, then Mute 3. Only the loops change; the live track does not.
5. Overdub 1 on with Play Mode = On Demand: play a new phrase and leave it on. Bank 1's older layers fade at
   Feedback 1 while banks 2 and 3 stay unchanged.
6. **Clear 2**: bank 2 fades out, and the other banks keep playing.
7. On SOUND, set Length 1 to 8 Bars and capture again. Below 60 BPM in 4/4, Status shows **Too Long**.
8. Record automation for Capture 2 and Mute 3, and check that it plays back. Try MIDI Learn on Volume 1.
9. Save and reload the project: the settings come back, all banks are empty, and the dials show "--".
10. Watch the CPU meter with all three banks overdubbing at a 128-sample buffer.

Report back: do the dials move smoothly, does Status update by itself, and does the CPU meter look sane?
