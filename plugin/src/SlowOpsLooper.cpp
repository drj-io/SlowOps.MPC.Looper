// Slow Ops Looper - MPC insert effect (thin adapter around the host-agnostic core).
//
// SPDX-License-Identifier: MIT
//
// All looper DSP lives in core/include/slowops/looper/ (no SDK dependency, so it
// can be reused in a desktop plugin). This file only:
//   * declares the MPC parameters (order and ids are permanent),
//   * turns the self-resetting Capture / Clear parameters into engine requests,
//   * maps the MPC transport onto the engine's TransportInfo,
//   * writes the engine status into the read-only Status parameter.
//
// Signal flow (see LooperEngine.hpp for detail):
//   in -> history ring -> [Capture] -> loop A/B -> drift -> tone -> wet ─┐
//   in ───────────────────────────────────────────────────────────── dry ┴-> Mix -> out
//   overdub: loop = softclip(tone(loop) * Feedback + in)

#include "mpc_plugin.h"

#include "../../core/include/slowops/looper/LooperEngine.hpp"

#include <cmath>
#include <cstdio>

namespace looper = slowops::looper;

class SlowOpsLooper : public mpc::Plugin
{
public:
    // Parameter order = MPC parameter index. Never reorder; append new ones at the end.
    // 0..3 are the default Program-mode Q-Links 1..4.
    enum Param
    {
        kFeedback = 0,   // % of the previous pass kept while overdubbing
        kTone,           // loop lowpass: 0 % = 400 Hz, 100 % = flat
        kDrift,          // tape wow on loop playback
        kMix,            // equal-power live/loop blend; dry only while no loop exists
        kCaptureMode,    // Last Bar | Rolling | Next Bar
        kPlayMode,       // Freeze | On Demand | Continuous
        kOverdub,        // latched; used by On Demand
        kCapture,        // trigger: set to On, the plugin fires once and sets it back to Off
        kClear,          // trigger: same behaviour
        kStatus,         // read-only display written by the plugin (not automatable)
        kNumParams
    };

    SlowOpsLooper() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("feedback", "Feedback", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(mpc::floatParam("tone", "Tone", 0.0f, 100.0f, 100.0f, "", 0));
        addParam(mpc::floatParam("drift", "Drift", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(mpc::floatParam("mix", "Mix", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::choiceParam("capmode", "Capture Mode", {"Last Bar", "Rolling", "Next Bar"}, 0));
        addParam(mpc::choiceParam("playmode", "Play Mode", {"Freeze", "On Demand", "Continuous"}, 1));
        addParam(mpc::boolParam("overdub", "Overdub", false));
        addParam(mpc::boolParam("capture", "Capture", false));
        addParam(mpc::boolParam("clear", "Clear", false));
        mpc::Param status = mpc::choiceParam("status", "Status",
            {"No Hist", "Ready", "Armed", "Rec Bar", "Playing", "Overdub", "Too Long"}, 0);
        status.automatable = false;
        addParam(status);
        if (numParams() != kNumParams) mpc::log("SlowOpsLooper: parameter count mismatch");
        // No built-in programs beyond "Default": a program writes every parameter,
        // which must never fire Capture or Clear.
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        engine_.prepare(sampleRate, maxBlockSize);   // the only allocation (~8.5 MB)
        statusHoldSamples_ = (int)(0.25 * sampleRate);
    }

    // Resume and state load. Clear trigger values without firing them, and leave
    // the Status value alone for 250 ms so a just-loaded state reads back intact.
    void reset() override
    {
        engine_.reset();
        if (getNormalised(kCapture) != 0.0f) { setNormalised(kCapture, 0.0f); notifyHostParameterChanged(kCapture); }
        if (getNormalised(kClear) != 0.0f)   { setNormalised(kClear, 0.0f);   notifyHostParameterChanged(kClear); }
        statusHoldLeft_ = statusHoldSamples_;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // Triggers: On means "fire once"; the plugin sets them back to Off and tells
        // the host (audioMasterAutomate), so the MPC's own copy of the value and the
        // switch on screen follow. Without this the MPC kept showing "On" (hardware
        // test, MPC OS 3.10.1) and the next tap would send Off instead of firing.
        if (getNormalised(kCapture) >= 0.5f)
        {
            setNormalised(kCapture, 0.0f);
            notifyHostParameterChanged(kCapture);
            engine_.requestCapture();
        }
        if (getNormalised(kClear) >= 0.5f)
        {
            setNormalised(kClear, 0.0f);
            notifyHostParameterChanged(kClear);
            engine_.requestClear();
        }

        looper::Params p;
        p.feedback = get(kFeedback) * 0.01f;
        p.tone = get(kTone) * 0.01f;
        p.drift = get(kDrift) * 0.01f;
        p.mix = get(kMix) * 0.01f;
        p.captureMode = (looper::CaptureMode)getInt(kCaptureMode);
        p.playbackMode = (looper::PlaybackMode)getInt(kPlayMode);
        p.overdub = getBool(kOverdub);

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
        engine_.process(inL, inR, outL, outR, ctx.numFrames, ti, p);

        if (statusHoldLeft_ > 0) { statusHoldLeft_ -= ctx.numFrames; return; }
        const float statusNorm = param(kStatus).toNormalised((float)engine_.status());
        if (getNormalised(kStatus) != statusNorm)
        {
            setNormalised(kStatus, statusNorm);
            notifyHostParameterChanged(kStatus);   // a few times per capture at most
        }
    }

    // Tone shows the cutoff it sets, e.g. "2.4k", or "Flat" at 100 %.
    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index != kTone) return false;
        if (plainValue >= 99.9f) { out = "Flat"; return true; }
        const double hz = 400.0 * std::pow(18000.0 / 400.0, plainValue * 0.01);
        char buf[16];
        if (hz >= 1000.0) std::snprintf(buf, sizeof(buf), "%.1fk", hz / 1000.0);
        else std::snprintf(buf, sizeof(buf), "%.0f", hz);
        out = buf;
        return true;
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Slow Ops Looper";              // == plugin.json "name"
        info.vendor = "Slow Ops";                   // == plugin.json "manufacturer"
        info.uniqueId = MPC_FOURCC('S', 'o', 'L', 'p');   // == plugin.json "uid"
        info.version = 1000;
        info.isInstrument = false;
        info.numInputs = 2;
        info.numOutputs = 2;
        info.tailSamples = 0;                       // unknown: the loop plays until cleared
        return info;
    }

    looper::LooperEngine engine_;
    int statusHoldSamples_ = 11025;
    int statusHoldLeft_ = 0;
};

MPC_PLUGIN_ENTRY(SlowOpsLooper)
