// Slow Ops Looper - MPC insert effect (thin adapter around the host-agnostic core).
//
// SPDX-License-Identifier: MIT
//
// All looper DSP lives in core/include/slowops/looper/ (no SDK dependency, so it
// can be reused in a desktop plugin). This file only:
//   * declares the MPC parameters (ids are permanent; see the table below),
//   * turns the self-resetting Capture / Clear parameters into engine requests,
//   * maps the MPC transport onto the engine's TransportInfo,
//   * writes each bank's status and playback progress into read-only parameters.
//
// Signal flow (see LooperEngine.hpp for detail):
//   in -> history ring -> [Capture n] -> bank n loop -> drift -> tone -> vol/solo/mute ─┐
//   in ──────────────────────────────────────────────────────────────────────── dry ──┴-> Mix -> out
//   overdub (per bank): loop = softclip(tone(loop) * Feedback n + in)

#include "mpc_plugin.h"

#include "../../core/include/slowops/looper/LooperEngine.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace looper = slowops::looper;

class SlowOpsLooper : public mpc::Plugin
{
public:
    static constexpr int kBanks = looper::LooperEngine::kNumBanks;

    // Shared parameters (indices 0..4).
    enum Global { kMix = 0, kTone, kDrift, kCaptureMode, kPlayMode, kNumGlobal };
    // Per-bank parameters: index = kNumGlobal + bank * kPerBank + offset.
    enum BankParam
    {
        kLength = 0,   // 1 / 2 / 4 / 8 bars, for the next capture
        kVolume,       // 0..100 %, squared law (100 % = unity)
        kFeedback,     // % of the previous pass kept while overdubbing
        kSolo,
        kMute,
        kOverdub,      // latched; used by On Demand
        kCapture,      // trigger: set to On, the plugin fires once and sets it back to Off
        kClear,        // trigger: same behaviour
        kProgress,     // read-only: playback position, shown as a dial and "bar/bars"
        kStatus,       // read-only: No Hist / Ready / Armed / Rec Bar / Playing / Overdub / Too Long
        kPerBank
    };
    // Read-only displays appended after the banks (indices never change once shipped).
    enum Extra
    {
        kBeat = kNumGlobal + kBanks * kPerBank,   // beat of the bar, 1..4: drives the 4 boxes on LOOPS
        kDbgTransport,   // read-only diagnostics (no page in the release skin): what the MPC reports and what the engine saw
        kDbgPosition,
        kDbgBars,
        kDbgTaps,
        kDbgLastCapture,
        kDbgLoopLevel,
        kLoopState,      // Idle / Armed / Rec / Play / Dub: colours the beat boxes
        kClearAll,       // trigger: clears every bank (same self-resetting behaviour as Clear n)
        kMetronome,      // Off / Standby / On. On: a pop on every beat, transport playing or stopped (output only,
                         // never recorded). Standby: silent, back On at the next Clear All or when the last loop is cleared.
        kClickLevel,     // metronome level, 0..100 % (50 % is about -19 dBFS peak)
        kMetroAuto,      // on (default): Metronome goes On -> Standby when the first loop since Clear All is captured
        kMetroTap,       // trigger (the METRONOME button): steps Metronome Off -> On -> Standby -> Off
        kMonitor,        // On (default): live input in the output, blended by Mix. Off: the MPC monitors the live
                         // input; the plugin outputs only the loops (full level, Mix ignored) and the metronome.
        kLatency,        // ms, -50..50 (default 12): loop playback runs this far ahead, to undo the MPC's round trip
        kPlayAll,        // master PLAY/STOP (On = playing). Follows the transport: Stop stops it, Play starts it.
                         // Turned On by hand with the transport stopped: every loop restarts from the top. Never
                         // starts the MPC transport.
        kPlay1,          // Play 1..3: per-bank PLAY/STOP (On = playing); stopped banks keep their place
        kPlay3 = kPlay1 + 2,
        kPlayTiming,     // Immediate / Bar End / Loop End: when a PLAY/STOP tap takes effect
        kPlayState1,     // read-only Play State 1..3, then Play All State: Stopped / Playing / Starting /
        kPlayStateAll = kPlayState1 + 3,   // Stopping; picks the PLAY/STOP button's image layer
        kEnd
    };
    static constexpr int kNumParams = kEnd;
    static constexpr int kFirstDebug = kDbgTransport, kNumDebug = kLoopState - kDbgTransport;
    static int idx(int bank, int p) { return kNumGlobal + bank * kPerBank + p; }

    SlowOpsLooper() : mpc::Plugin(makeInfo())
    {
        // Ids from the single-loop prototype are kept for the shared controls and
        // bank 1 so projects saved with it keep their settings.
        addParam(mpc::floatParam("mix", "Mix", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam("tone", "Tone", 0.0f, 100.0f, 100.0f, "", 0));
        addParam(mpc::floatParam("drift", "Drift", 0.0f, 100.0f, 15.0f, "%", 0));
        // Default Next Bar: Capture waits for the next "1", then records the bank's length.
        addParam(mpc::choiceParam("capmode", "Capture Mode", {"Last Bar", "Rolling", "Next Bar"}, 2));
        addParam(mpc::choiceParam("playmode", "Play Mode", {"Freeze", "On Demand", "Continuous"}, 1));

        const int defaultLength[kBanks] = {2, 2, 2};   // 4 bars each
        for (int b = 0; b < kBanks; ++b)
        {
            const std::string n = std::to_string(b + 1);
            auto id = [&](const char* base, const char* legacy) { return (b == 0 && legacy) ? std::string(legacy) : std::string(base) + n; };
            auto name = [&](const char* base) { return std::string(base) + " " + n; };
            addParam(mpc::choiceParam(id("len", nullptr).c_str(), name("Length").c_str(), {"1 Bar", "2 Bars", "4 Bars", "8 Bars"}, defaultLength[b]));
            addParam(mpc::floatParam(id("vol", nullptr).c_str(), name("Volume").c_str(), 0.0f, 100.0f, 100.0f, "%", 0));
            addParam(mpc::floatParam(id("fb", "feedback").c_str(), name("Feedback").c_str(), 0.0f, 100.0f, 100.0f, "%", 0));
            addParam(mpc::boolParam(id("solo", nullptr).c_str(), name("Solo").c_str(), false));
            addParam(mpc::boolParam(id("mute", nullptr).c_str(), name("Mute").c_str(), false));
            addParam(mpc::boolParam(id("overdub", "overdub").c_str(), name("Overdub").c_str(), false));
            addParam(mpc::boolParam(id("capture", "capture").c_str(), name("Capture").c_str(), false));
            addParam(mpc::boolParam(id("clear", "clear").c_str(), name("Clear").c_str(), false));
            mpc::Param prog = mpc::floatParam(id("prog", nullptr).c_str(), name("Bank").c_str(), 0.0f, 100.0f, 0.0f, "%", 0);
            prog.automatable = false;
            addParam(prog);
            mpc::Param status = mpc::choiceParam(id("status", "status").c_str(), name("Status").c_str(),
                {"No Hist", "Ready", "Armed", "Rec Bar", "Playing", "Overdub", "Too Long"}, 0);
            status.automatable = false;
            addParam(status);
        }
        mpc::Param beat = mpc::choiceParam("beat", "Beat", {"1", "2", "3", "4"}, 0);
        beat.automatable = false;
        addParam(beat);
        const char* dbgIds[kNumDebug] = {"dbg_transport", "dbg_position", "dbg_bars", "dbg_taps", "dbg_last", "dbg_level"};
        const char* dbgNames[kNumDebug] = {"Transport", "Position", "Bars Seen", "Taps", "Last Cap", "Loop Level"};
        for (int d = 0; d < kNumDebug; ++d)
        {
            mpc::Param p = mpc::floatParam(dbgIds[d], dbgNames[d], 0.0f, 100.0f, 0.0f, "", 0);
            p.automatable = false;
            addParam(p);
        }
        mpc::Param state = mpc::choiceParam("loopstate", "Loop State", {"Idle", "Armed", "Rec", "Play", "Dub"}, 0);
        state.automatable = false;
        addParam(state);
        addParam(mpc::boolParam("clearall", "Clear All", false));
        // Off = 0 and On = 1.0 normalised, as when this was an Off/On switch, so older saves load unchanged.
        addParam(mpc::choiceParam("metro", "Metronome", {"Off", "Standby", "On"}, 0));
        addParam(mpc::floatParam("clicklvl", "Click Level", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::boolParam("metroauto", "Auto Standby", true));   // id kept: older saves still load it
        addParam(mpc::boolParam("metrotap", "Metro Tap", false));
        addParam(mpc::boolParam("monitor", "Monitor", true));
        // MPC XL at 44.1 kHz / 128: 256-sample input + 384-sample output buffers plus USB and converters, about 12 ms.
        addParam(mpc::floatParam("latency", "Latency", -50.0f, 50.0f, 12.0f, "ms", 1));
        addParam(mpc::boolParam("playall", "Play All", true));
        for (int b = 0; b < kBanks; ++b)
            addParam(mpc::boolParam(("play" + std::to_string(b + 1)).c_str(), ("Play " + std::to_string(b + 1)).c_str(), true));
        addParam(mpc::choiceParam("playtiming", "Play Timing", {"Immediate", "Bar End", "Loop End"}, 0));
        for (int b = 0; b <= kBanks; ++b)
        {
            const std::string n = b < kBanks ? std::to_string(b + 1) : std::string("all");
            mpc::Param ps = mpc::choiceParam(("playstate" + n).c_str(),
                (b < kBanks ? "Play State " + n : std::string("Play All State")).c_str(),
                {"Stopped", "Playing", "Starting", "Stopping"}, 1);
            ps.automatable = false;
            addParam(ps);
        }
        if (numParams() != kNumParams) mpc::log("SlowOpsLooper: parameter count mismatch");
        // No built-in programs beyond "Default": a program writes every parameter,
        // which must never fire Capture or Clear.
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        engine_.prepare(sampleRate, maxBlockSize);   // the only allocation (~58 MB)
        statusHoldSamples_ = (int)(0.25 * sampleRate);
        progressMinSamples_ = (int)(0.04 * sampleRate);
        debugMinSamples_ = (int)(0.1 * sampleRate);
    }

    // Resume and state load. Turn off any trigger that would fire (a state saved
    // while Capture was On must not capture on load), and leave the read-only
    // displays alone for 250 ms so a just-loaded state reads back intact.
    void reset() override
    {
        engine_.reset();
        for (int b = 0; b < kBanks; ++b)
            for (int p : {kCapture, kClear})
                if (getNormalised(idx(b, p)) >= 0.5f) { setNormalised(idx(b, p), 0.0f); notifyHostParameterChanged(idx(b, p)); }
        for (int p : {kClearAll, kMetroTap})
            if (getNormalised(p) >= 0.5f) { setNormalised(p, 0.0f); notifyHostParameterChanged(p); }
        statusHoldLeft_ = statusHoldSamples_;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // Triggers: On means "fire once"; the plugin sets them back to Off and tells
        // the host (audioMasterAutomate), so the MPC's own copy of the value and the
        // switch on screen follow.
        for (int b = 0; b < kBanks; ++b)
        {
            if (getNormalised(idx(b, kCapture)) >= 0.5f)
            {
                setNormalised(idx(b, kCapture), 0.0f);
                notifyHostParameterChanged(idx(b, kCapture));
                engine_.requestCapture(b);
                // A capture is meant to be heard: its bank and the master go to PLAY (no restart).
                if (!getBool(kPlay1 + b)) setSwitch(kPlay1 + b, true);
                if (!getBool(kPlayAll)) { setSwitch(kPlayAll, true); playAllWas_ = true; }
            }
            if (getNormalised(idx(b, kClear)) >= 0.5f)
            {
                setNormalised(idx(b, kClear), 0.0f);
                notifyHostParameterChanged(idx(b, kClear));
                engine_.requestClear(b);
            }
        }

        if (getNormalised(kClearAll) >= 0.5f)
        {
            setNormalised(kClearAll, 0.0f);
            notifyHostParameterChanged(kClearAll);
            for (int b = 0; b < kBanks; ++b) engine_.requestClear(b);
            metroRearm_ = true;
        }
        if (getNormalised(kMetroTap) >= 0.5f)
        {
            setNormalised(kMetroTap, 0.0f);
            notifyHostParameterChanged(kMetroTap);
            const int st = getInt(kMetronome);
            setMetronome(st == kMetroOff ? kMetroOn : st == kMetroOn ? kMetroStandby : kMetroOff);
        }

        looper::GlobalParams g;
        g.mix = get(kMix) * 0.01f;
        g.monitor = getBool(kMonitor);
        g.latencyMs = get(kLatency);
        g.playTiming = (looper::PlayTiming)getInt(kPlayTiming);

        // Master PLAY/STOP turned On by hand while the transport is stopped: restart the
        // internal bar clock and every loop from the top. (Transport edges: after process.)
        {
            const bool playAll = getBool(kPlayAll);
            if (playAll && !playAllWas_ && !hostWasRunning_) engine_.restartFromTop();
            playAllWas_ = playAll;
            g.playAll = playAll;
        }
        g.tone = get(kTone) * 0.01f;
        g.drift = get(kDrift) * 0.01f;
        g.captureMode = (looper::CaptureMode)getInt(kCaptureMode);
        g.playbackMode = (looper::PlaybackMode)getInt(kPlayMode);
        g.metronome = getInt(kMetronome) == kMetroOn;
        g.clickLevel = get(kClickLevel) * 0.01f;
        looper::BankParams bp[kBanks];
        for (int b = 0; b < kBanks; ++b)
        {
            bp[b].lengthBars = 1 << getInt(idx(b, kLength));
            bp[b].volume = get(idx(b, kVolume)) * 0.01f;
            bp[b].feedback = get(idx(b, kFeedback)) * 0.01f;
            bp[b].solo = getBool(idx(b, kSolo));
            bp[b].mute = getBool(idx(b, kMute));
            bp[b].overdub = getBool(idx(b, kOverdub));
            bp[b].play = getBool(kPlay1 + b);
        }

        const mpc::Transport& t = ctx.transport;
        looper::TransportInfo ti;
        ti.playing = t.valid && t.playing;
        ti.ppqValid = t.valid && t.ppqValid;
        ti.barsValid = t.valid && t.ppqValid;
        ti.bpm = (t.valid && t.tempoValid) ? t.bpm : 120.0;
        ti.ppqPos = t.ppqPos;
        ti.barStartPpq = t.barStartPpq;
        ti.timeSigNumerator = t.timeSigNumerator;
        ti.timeSigDenominator = t.timeSigDenominator;

        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        engine_.process(inL, inR, outL, outR, ctx.numFrames, ti, g, bp);
        snapshotDiagnostics(t);

        // Master PLAY/STOP follows the transport: Stop stops the looper, Play starts it.
        // The engine already did so this block; make the switch show it.
        {
            const bool running = engine_.diagnostics().hostRunning;
            if (running != hostWasRunning_)
            {
                hostWasRunning_ = running;
                if (getBool(kPlayAll) != running) setSwitch(kPlayAll, running);
                playAllWas_ = running;
            }
        }

        // Auto Standby: the metronome is for laying down the first loops. Clear All, or
        // clearing the last loop, starts a fresh count and wakes a Standby metronome;
        // when the first loop after that is captured, On goes to Standby (once: a
        // metronome stepped back On by hand stays On).
        {
            bool anyLoop = false;
            for (int b = 0; b < kBanks; ++b) anyLoop = anyLoop || engine_.hasLoop(b);
            const int64_t done = engine_.diagnostics().loopsCompleted;
            if (metroRearm_ || (hadLoop_ && !anyLoop))
            {
                metroBase_ = done;
                metroAutoDone_ = false;
                if (getInt(kMetronome) == kMetroStandby) setMetronome(kMetroOn);
            }
            metroRearm_ = false;
            hadLoop_ = anyLoop;
            if (done - metroBase_ >= 1 && !metroAutoDone_)
            {
                metroAutoDone_ = true;
                if (getBool(kMetroAuto) && getInt(kMetronome) == kMetroOn) setMetronome(kMetroStandby);
            }
        }

        if (statusHoldLeft_ > 0) { statusHoldLeft_ -= ctx.numFrames; return; }

        // Beat boxes: lit box = beat of the bar (beats 5+ of long bars wrap to 1..4).
        const float beatNorm = (float)(engine_.diagnostics().beat % 4) / 3.0f;
        if (getNormalised(kBeat) != beatNorm) { setNormalised(kBeat, beatNorm); notifyHostParameterChanged(kBeat); }

        // Beat box colour: the most urgent state of any bank.
        //   Rec (red) > Armed (orange, waiting for the next "1") > Dub (purple) > Play (green) > Idle.
        int state = 0;
        for (int b = 0; b < kBanks; ++b)
        {
            int s = 0;
            switch (engine_.status(b))
            {
                case looper::Status::Recording:   s = 4; break;
                case looper::Status::Armed:       s = 3; break;
                case looper::Status::Overdubbing: s = 2; break;
                case looper::Status::Playing:     s = 1; break;
                default:                          s = 0; break;
            }
            if (s > state) state = s;
        }
        static const int kStateChoice[5] = {0, 3, 4, 1, 2};   // priority -> Idle, Play, Dub, Armed, Rec
        const float stateNorm = (float)kStateChoice[state] / 4.0f;
        if (getNormalised(kLoopState) != stateNorm) { setNormalised(kLoopState, stateNorm); notifyHostParameterChanged(kLoopState); }

        // PLAY/STOP buttons: one image layer per state; Starting / Stopping while a tap waits for
        // the Play Timing boundary. The master shows waiting while any bank waits.
        {
            bool anyStarting = false, anyStopping = false;
            for (int b = 0; b <= kBanks; ++b)
            {
                int ps;
                if (b < kBanks)
                {
                    ps = (int)engine_.playState(b);
                    anyStarting = anyStarting || ps == (int)looper::PlayState::Starting;
                    anyStopping = anyStopping || ps == (int)looper::PlayState::Stopping;
                }
                else
                    ps = (int)(getBool(kPlayAll) ? (anyStarting ? looper::PlayState::Starting : looper::PlayState::Playing)
                                                 : (anyStopping ? looper::PlayState::Stopping : looper::PlayState::Stopped));
                const float norm = (float)ps / 3.0f;
                if (getNormalised(kPlayState1 + b) != norm) { setNormalised(kPlayState1 + b, norm); notifyHostParameterChanged(kPlayState1 + b); }
            }
        }

        // DEBUG readouts: bump a readout's value whenever its text changes (so the MPC
        // redraws it), at most every 100 ms.
        debugAge_ += ctx.numFrames;
        if (debugAge_ >= debugMinSamples_)
        {
            debugAge_ = 0;
            for (int d = 0; d < kNumDebug; ++d)
            {
                const uint64_t key = debugKey(d);
                if (key == debugLastKey_[d]) continue;
                debugLastKey_[d] = key;
                debugCounter_[d] = (debugCounter_[d] + 1) % 100;
                setNormalised(kFirstDebug + d, (float)debugCounter_[d] / 99.0f);
                notifyHostParameterChanged(kFirstDebug + d);
            }
        }
        for (int b = 0; b < kBanks; ++b)
        {
            const int si = idx(b, kStatus);
            const float statusNorm = param(si).toNormalised((float)engine_.status(b));
            if (getNormalised(si) != statusNorm) { setNormalised(si, statusNorm); notifyHostParameterChanged(si); }

            // Progress: quantised to 16th notes of the loop and sent at most every 40 ms.
            progressAge_[b] += ctx.numFrames;
            const int bars = engine_.loopBars(b);
            const float steps = (float)(16 * (bars > 0 ? bars : 1));
            const float prog = bars > 0 ? std::floor(engine_.progress(b) * steps) / steps : 0.0f;
            const int pi = idx(b, kProgress);
            if (getNormalised(pi) != prog && (progressAge_[b] >= progressMinSamples_ || prog == 0.0f))
            {
                setNormalised(pi, prog);
                notifyHostParameterChanged(pi);
                progressAge_[b] = 0;
            }
        }
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        char buf[24];
        if (index == kTone)   // the cutoff it sets, e.g. "2.4k", or "Flat" at 100 %
        {
            if (plainValue >= 99.9f) { out = "Flat"; return true; }
            const double hz = 400.0 * std::pow(18000.0 / 400.0, plainValue * 0.01);
            if (hz >= 1000.0) std::snprintf(buf, sizeof(buf), "%.1fk", hz / 1000.0);
            else std::snprintf(buf, sizeof(buf), "%.0f", hz);
            out = buf;
            return true;
        }
        if (index >= kFirstDebug && index < kFirstDebug + kNumDebug) { out = debugText(index - kFirstDebug); return true; }
        if (index >= kNumGlobal && index < kBeat && (index - kNumGlobal) % kPerBank == kProgress)   // "bar/bars", e.g. "3/4"
        {
            const int bars = engine_.loopBars((index - kNumGlobal) / kPerBank);
            if (bars <= 0) { out = "--"; return true; }
            int bar = (int)std::floor(plainValue * 0.01f * (float)bars) + 1;
            if (bar > bars) bar = bars;
            std::snprintf(buf, sizeof(buf), "%d/%d", bar, bars);
            out = buf;
            return true;
        }
        return false;
    }

private:
    enum { kMetroOff = 0, kMetroStandby = 1, kMetroOn = 2 };   // Metronome choices
    // Set Metronome from the plugin and tell the host, so the MPC's copy and the button follow.
    void setSwitch(int i, bool on) { setNormalised(i, on ? 1.0f : 0.0f); notifyHostParameterChanged(i); }
    void setMetronome(int st) { setNormalised(kMetronome, (float)st * 0.5f); notifyHostParameterChanged(kMetronome); }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Slow Ops Looper";              // == plugin.json "name"
        info.vendor = "Slow Ops";                   // == plugin.json "manufacturer"
        info.uniqueId = MPC_FOURCC('S', 'o', 'L', 'p');   // == plugin.json "uid"
        info.version = 2000;
        info.isInstrument = false;
        info.numInputs = 2;
        info.numOutputs = 2;
        info.tailSamples = 0;                       // unknown: loops play until cleared
        return info;
    }

    // ---- diagnostics: copied from the engine on the audio thread, read by the UI thread ----
    struct Snap
    {
        std::atomic<int> playing{0}, moving{0}, ppqValid{0}, locked{0}, num{4}, den{4}, lastBank{-1}, lastResult{0};
        std::atomic<float> bpm{0.0f}, ppq{0.0f}, peakDb{-120.0f}, lastSeconds{0.0f};
        std::atomic<int64_t> bars{0}, jumps{0}, caps[kBanks], clears[kBanks];
        Snap() { for (int b = 0; b < kBanks; ++b) { caps[b] = 0; clears[b] = 0; } }
    } snap_;

    void snapshotDiagnostics(const mpc::Transport& t)
    {
        const looper::Diagnostics& d = engine_.diagnostics();
        snap_.playing = t.valid && t.playing ? 1 : 0;
        snap_.ppqValid = t.valid && t.ppqValid ? 1 : 0;
        snap_.moving = d.positionMoving ? 1 : 0;
        snap_.locked = d.hostLocked ? 1 : 0;
        snap_.num = d.timeSigNumerator; snap_.den = d.timeSigDenominator;
        snap_.bpm = (float)d.bpm; snap_.ppq = (float)d.ppqPos;
        snap_.bars = d.barsElapsed; snap_.jumps = d.discontinuities;
        for (int b = 0; b < kBanks; ++b) { snap_.caps[b] = d.captureRequests[b]; snap_.clears[b] = d.clearRequests[b]; }
        snap_.lastBank = d.lastBank; snap_.lastResult = (int)d.lastResult;
        snap_.lastSeconds = (float)((double)d.lastLength / sampleRate());
        // Loop level with a slow release so a short peak stays readable.
        const float db = d.loopPeak > 1e-6f ? 20.0f * std::log10(d.loopPeak) : -120.0f;
        const float prev = snap_.peakDb.load();
        snap_.peakDb = db > prev ? db : prev - 0.05f * (float)(prev > -120.0f);
    }

    uint64_t debugKey(int d) const
    {
        switch (d)
        {
            case 0: return (uint64_t)snap_.playing * 1000003u + (uint64_t)snap_.moving * 4999u + (uint64_t)snap_.ppqValid * 7919u + (uint64_t)snap_.locked * 131u
                         + (uint64_t)std::lround(snap_.bpm.load() * 10.0f) * 65537u + (uint64_t)snap_.num * 17u + (uint64_t)snap_.den;
            case 1: return (uint64_t)std::floor(snap_.ppq.load());
            case 2: return (uint64_t)snap_.bars.load() * 1000u + (uint64_t)snap_.jumps.load();
            case 3: { uint64_t k = 0; for (int b = 0; b < kBanks; ++b) k = k * 1000u + (uint64_t)snap_.caps[b].load() * 31u + (uint64_t)snap_.clears[b].load(); return k; }
            case 4: return (uint64_t)(snap_.lastBank + 1) * 100u + (uint64_t)snap_.lastResult + (uint64_t)snap_.caps[0] + (uint64_t)snap_.caps[1] * 7u + (uint64_t)snap_.caps[2] * 13u;
            case 5: return (uint64_t)std::lround(std::fmax(-60.0f, snap_.peakDb.load()) / 3.0f) + 100u;
            default: return 0;
        }
    }

    std::string debugText(int d) const
    {
        char buf[48];
        switch (d)
        {
            case 0:   // "Play 92.0 4/4" (host flag), "Run ..." (no flag, but the position moves), "Stop ...";
                      // "!ppq" when the host gives no position, "int" when the internal clock runs
                std::snprintf(buf, sizeof(buf), "%s %.1f %d/%d%s", snap_.playing ? "Play" : (snap_.moving ? "Run" : "Stop"), (double)snap_.bpm.load(),
                              snap_.num.load(), snap_.den.load(), snap_.ppqValid ? (snap_.locked ? "" : " int") : " !ppq");
                break;
            case 1:   // host position in quarter notes
                std::snprintf(buf, sizeof(buf), "q %.1f", (double)snap_.ppq.load());
                break;
            case 2:   // downbeats the engine detected, and transport jumps
                std::snprintf(buf, sizeof(buf), "%lld j%lld", (long long)snap_.bars.load(), (long long)snap_.jumps.load());
                break;
            case 3:   // capture taps received per bank
                std::snprintf(buf, sizeof(buf), "%lld %lld %lld", (long long)snap_.caps[0].load(), (long long)snap_.caps[1].load(), (long long)snap_.caps[2].load());
                break;
            case 4:
            {
                static const char* r[] = {"none", "OK", "Armed", "Cancel", "NoHist", "TooLong", "Busy"};
                const int res = snap_.lastResult.load();
                if (snap_.lastBank < 0) std::snprintf(buf, sizeof(buf), "none");
                else std::snprintf(buf, sizeof(buf), "B%d %s %.1fs", snap_.lastBank.load() + 1, r[res < 0 || res > 6 ? 0 : res], (double)snap_.lastSeconds.load());
                break;
            }
            case 5:
            {
                const float db = snap_.peakDb.load();
                if (db <= -60.0f) std::snprintf(buf, sizeof(buf), "silent");
                else std::snprintf(buf, sizeof(buf), "%.0f dB", (double)db);
                break;
            }
            default: buf[0] = 0;
        }
        return buf;
    }

    looper::LooperEngine engine_;
    int debugMinSamples_ = 4410, debugAge_ = 0;
    int debugCounter_[kNumDebug] = {};
    uint64_t debugLastKey_[kNumDebug] = {};
    int statusHoldSamples_ = 11025;
    int statusHoldLeft_ = 0;
    int64_t metroBase_ = 0;          // engine loopsCompleted at the last re-arm (Clear All / last loop cleared)
    bool metroRearm_ = false;        // Clear All pressed this block
    bool metroAutoDone_ = false;     // the first-loop On -> Standby already happened since the last re-arm
    bool hadLoop_ = false;           // any bank held a loop last block
    bool hostWasRunning_ = false;    // transport state last block (edges drive Play All)
    bool playAllWas_ = true;         // Play All last block (a hand-made Off -> On restarts from the top)
    int progressMinSamples_ = 1764;
    int progressAge_[kBanks] = {};
};

MPC_PLUGIN_ENTRY(SlowOpsLooper)
