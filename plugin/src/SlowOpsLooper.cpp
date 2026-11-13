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

#include <cmath>
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
    static constexpr int kNumParams = kNumGlobal + kBanks * kPerBank;
    static int idx(int bank, int p) { return kNumGlobal + bank * kPerBank + p; }

    SlowOpsLooper() : mpc::Plugin(makeInfo())
    {
        // Ids from the single-loop prototype are kept for the shared controls and
        // bank 1 so projects saved with it keep their settings.
        addParam(mpc::floatParam("mix", "Mix", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam("tone", "Tone", 0.0f, 100.0f, 100.0f, "", 0));
        addParam(mpc::floatParam("drift", "Drift", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(mpc::choiceParam("capmode", "Capture Mode", {"Last Bar", "Rolling", "Next Bar"}, 0));
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
        if (numParams() != kNumParams) mpc::log("SlowOpsLooper: parameter count mismatch");
        // No built-in programs beyond "Default": a program writes every parameter,
        // which must never fire Capture or Clear.
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        engine_.prepare(sampleRate, maxBlockSize);   // the only allocation (~58 MB)
        statusHoldSamples_ = (int)(0.25 * sampleRate);
        progressMinSamples_ = (int)(0.04 * sampleRate);
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

        if (statusHoldLeft_ > 0) { statusHoldLeft_ -= ctx.numFrames; return; }
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
        if (index >= kNumGlobal && (index - kNumGlobal) % kPerBank == kProgress)   // "bar/bars", e.g. "3/4"
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

    looper::LooperEngine engine_;
    int statusHoldSamples_ = 11025;
    int statusHoldLeft_ = 0;
    int progressMinSamples_ = 1764;
    int progressAge_[kBanks] = {};
};

MPC_PLUGIN_ENTRY(SlowOpsLooper)
