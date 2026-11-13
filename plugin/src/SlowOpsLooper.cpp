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
        kDbgTransport,   // DEBUG page: what the MPC reports and what the engine saw
        kDbgPosition,
        kDbgBars,
        kDbgTaps,
        kDbgLastCapture,
        kDbgLoopLevel,
        kLoopState,      // Idle / Armed / Rec / Play / Dub: colours the beat boxes
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

        const int defaultLength[kBanks] = {0, 1, 2};   // 1, 2 and 4 bars
        for (int b = 0; b < kBanks; ++b)
        {
            const std::string n = std::to_string(b + 1);
            auto id = [&](const char* base, const char* legacy) { return (b == 0 && legacy) ? std::string(legacy) : std::string(base) + n; };
            auto name = [&](const char* base) { return std::string(base) + " " + n; };
            addParam(mpc::choiceParam(id("len", nullptr).c_str(), name("Length").c_str(), {"1 Bar", "2 Bars", "4 Bars", "8 Bars"}, defaultLength[b]));
            addParam(mpc::floatParam(id("vol", nullptr).c_str(), name("Volume").c_str(), 0.0f, 100.0f, 100.0f, "%", 0));
            addParam(mpc::floatParam(id("fb", "feedback").c_str(), name("Feedback").c_str(), 0.0f, 100.0f, 70.0f, "%", 0));
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
            }
            if (getNormalised(idx(b, kClear)) >= 0.5f)
            {
                setNormalised(idx(b, kClear), 0.0f);
                notifyHostParameterChanged(idx(b, kClear));
                engine_.requestClear(b);
            }
        }

        looper::GlobalParams g;
        g.mix = get(kMix) * 0.01f;
        g.tone = get(kTone) * 0.01f;
        g.drift = get(kDrift) * 0.01f;
        g.captureMode = (looper::CaptureMode)getInt(kCaptureMode);
        g.playbackMode = (looper::PlaybackMode)getInt(kPlayMode);
        looper::BankParams bp[kBanks];
        for (int b = 0; b < kBanks; ++b)
        {
            bp[b].lengthBars = 1 << getInt(idx(b, kLength));
            bp[b].volume = get(idx(b, kVolume)) * 0.01f;
            bp[b].feedback = get(idx(b, kFeedback)) * 0.01f;
            bp[b].solo = getBool(idx(b, kSolo));
            bp[b].mute = getBool(idx(b, kMute));
            bp[b].overdub = getBool(idx(b, kOverdub));
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
        std::atomic<int> playing{0}, ppqValid{0}, locked{0}, num{4}, den{4}, lastBank{-1}, lastResult{0};
        std::atomic<float> bpm{0.0f}, ppq{0.0f}, peakDb{-120.0f}, lastSeconds{0.0f};
        std::atomic<int64_t> bars{0}, jumps{0}, caps[kBanks], clears[kBanks];
        Snap() { for (int b = 0; b < kBanks; ++b) { caps[b] = 0; clears[b] = 0; } }
    } snap_;

    void snapshotDiagnostics(const mpc::Transport& t)
    {
        const looper::Diagnostics& d = engine_.diagnostics();
        snap_.playing = t.valid && t.playing ? 1 : 0;
        snap_.ppqValid = t.valid && t.ppqValid ? 1 : 0;
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
            case 0: return (uint64_t)snap_.playing * 1000003u + (uint64_t)snap_.ppqValid * 7919u + (uint64_t)snap_.locked * 131u
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
            case 0:   // e.g. "Play 92.0 4/4" / "Stop 92.0 4/4" ; "!ppq" when the host gives no position
                std::snprintf(buf, sizeof(buf), "%s %.1f %d/%d%s", snap_.playing ? "Play" : "Stop", (double)snap_.bpm.load(),
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
    int progressMinSamples_ = 1764;
    int progressAge_[kBanks] = {};
};

MPC_PLUGIN_ENTRY(SlowOpsLooper)
