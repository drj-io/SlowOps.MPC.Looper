# Controls, automation and external triggering

What each control is, how it can be driven on an MPC insert, and what has been
**verified in the simulator** versus what still needs **verification on the
Live III**. The simulator evidence comes from `scripts/test.sh` (SDK `host_sim`
plus `tests/scenario_host.cpp`) at SDK revision `febd2ca`.

## Parameters (index order is permanent)

| # | Name | Values | Default | Touchscreen | Screen Q-Link | Program Q-Link | Automatable |
|---|---|---|---|---|---|---|---|
| 0 | Feedback | 0–100 % | 70 % | knob | 13 | 1 | yes |
| 1 | Tone | 400 Hz … 18 kHz, 100 % = Flat | Flat | knob | 14 | 2 | yes |
| 2 | Drift | 0–100 % | 15 % | knob | 15 | 3 | yes |
| 3 | Mix | 0–100 % (equal power) | 50 % | fader | 16 | 4 | yes |
| 4 | Capture Mode | Last Bar / Rolling / Next Bar | Last Bar | 3-way switch | 5 | 5 | yes |
| 5 | Play Mode | Freeze / On Demand / Continuous | On Demand | 3-way switch | 6 | 6 | yes |
| 6 | Overdub | Off / On (latched) | Off | switch | 10 | 7 | yes |
| 7 | Capture | trigger | Off | switch | — | — | yes |
| 8 | Clear | trigger | Off | switch | — | — | yes |
| 9 | Status | No Hist / Ready / Armed / Rec Bar / Playing / Overdub / Too Long | — | value display | — | — | **no** |

**Triggers.** The MPC skin format has no momentary button, only a toggle. Capture and Clear therefore
*self-reset*: setting the value to On fires the action once at the next audio block, and the plugin writes
the value back to Off. A state or project load clears them without firing.

**Why the triggers are off the Q-Links.** Capture and Clear are deliberately left off every Q-Link. The
endless Q-Link encoders would push the value past 50 % and fire a capture while you are only nudging a
knob. To change this, edit `scripts/regen_skins.sh`.

**Status** is written by the plugin. It is not automatable (the host reads `effCanBeAutomated` = 0), so it
never shows up in automation lanes.

## What can drive what

| Route | Feedback, Tone, Drift, Mix | Modes, Overdub | Capture, Clear | Evidence |
|---|---|---|---|---|
| Touchscreen | yes | yes | yes (tap the switch) | Skin generated and validated: `validate.py` reports 0 errors and all handles in range. **Hardware: confirm.** |
| Q-Links (screen and program mode) | yes | yes | no, by design | `plugin/skins/Q-Links.json` |
| Host automation lanes | yes | yes | yes: a lane step to On fires once | Sim: `effCanBeAutomated` = 1 for indices 0–8 and 0 for Status (`host_sim dump`). A `setParameter` from outside the audio block fires and self-resets (`scenario_host` "automation writes"). **Hardware: confirm that the lanes are offered for inserts and that the recorded steps fire.** |
| Project / preset load | restored | restored | never fire | Sim: `scenario_host` "State recall" (values and display text identical, loop empty, a chunk saved with Capture=On does not fire), and `host_sim` check 3. |
| MIDI notes or CC sent to the insert | — | — | — | **Not possible.** The MPC sends no MIDI to effect plugins (SDK `host-contract.md`), so a plugin-side MIDI learn cannot exist. |
| MPC MIDI Learn / Q-Link mapping from an external controller | ? | ? | ? | **Hardware only.** The SDK documents nothing here. If MPC OS can map an external CC to an insert parameter, all automatable parameters qualify. |

## Live III checklist

Before you start, check MPC OS is 3.10.1 or later (Preferences → Info). Then turn on
Preferences → Enhancements (Beta) → **Enable Enhancements** and **External Plugin Support**.

1. Insert **Slow Ops Looper** on the *Ichigo* track (External Plugins), with drums on their own track.
   Play: the track should sound unchanged. After one bar, Status reads **Ready**.
2. Capture Mode = **Last Bar**. Tap **Capture** while the motif plays. On the next block the loop should be
   playing in time with the drums, and Status reads **Playing**. Check that the Capture switch drops back to
   Off. If it stays lit, note it: it means the screen does not redraw values the plugin writes.
3. **Rolling**: tap Capture mid-bar. The loop is the last bar ending at your tap, one bar behind.
   **Next Bar**: tap → **Armed**, then **Rec Bar** at the downbeat, then **Playing**.
4. Play Mode = **On Demand**, Feedback about 60 %. Turn **Overdub** on, play a new phrase, and keep
   Overdub on for a few bars. The older layers should fade each pass while the drums keep going. Turn
   Overdub off and the loop freezes as it is. Try **Continuous** and **Freeze**.
5. Turn Tone, Drift and Mix on Q-Links 1–4 (Program/Track Q-Link mode) and on the knobs next to them on
   screen.
6. **Clear**: the loop fades out and the track is dry again.
7. Record automation: one step of Capture to On, then Overdub on and off. Play it back and check it fires.
   Try MIDI Learn from an external controller on Capture and on Mix.
8. Save the project and reload it: the settings come back, the loop is empty, and Status reads Ready or
   No Hist.
9. Watch the CPU meter at a 128-sample buffer. Change the buffer size while a loop plays: no crash. The loop
   survives a buffer-size change but not a sample-rate change.
10. Stop the transport: the loop keeps playing on the internal clock. Start again from bar 1: it realigns
    to the downbeat.
