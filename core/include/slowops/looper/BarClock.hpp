// BarClock.hpp - turns host transport into bar length, bar phase and downbeat
// positions per block. Locks to the host while it plays; free-runs at the host
// tempo/time signature while stopped. SPDX-License-Identifier: MIT
#pragma once

#include "Dsp.hpp"

#include <cmath>

namespace slowops::looper {

struct TransportInfo
{
    bool   playing = false;
    bool   ppqValid = false;
    bool   barsValid = false;
    double bpm = 120.0;
    double ppqPos = 0.0;        // quarter notes since song start
    double barStartPpq = 0.0;   // quarter-note position of the current bar's downbeat
    int    timeSigNumerator = 4;
    int    timeSigDenominator = 4;
};

struct ClockBlock
{
    static constexpr int kMaxDownbeats = 8;
    double barLen = 0.0;        // samples per bar at the current tempo
    double phase0 = 0.0;        // samples since the last downbeat, at block start
    bool   discontinuity = false;
    bool   hostLocked = false;
    int    numDownbeats = 0;
    int    downbeats[kMaxDownbeats] = {};   // sample offsets inside the block, ascending
};

class BarClock
{
public:
    void prepare(double sampleRate) { sr_ = sampleRate; reset(); }
    void reset() { havePrev_ = false; prevHost_ = false; internalPhase_ = 0.0; }

    ClockBlock advance(const TransportInfo& t, int numFrames)
    {
        ClockBlock b;
        double bpm = std::isfinite(t.bpm) ? clampv(t.bpm, 20.0, 400.0) : 120.0;
        const int num = clampv(t.timeSigNumerator, 1, 32);
        int den = t.timeSigDenominator;
        if (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32) den = 4;
        const double barQ = num * 4.0 / den;
        const double samplesPerQ = sr_ * 60.0 / bpm;
        b.barLen = barQ * samplesPerQ;

        const bool host = t.playing && t.ppqValid && std::isfinite(t.ppqPos);
        double phase0;
        if (host)
        {
            double phaseQ = t.ppqPos - t.barStartPpq;
            if (!t.barsValid || phaseQ < -1e-6 || phaseQ > barQ + 1e-6)
            {
                phaseQ = std::fmod(t.ppqPos, barQ);
                if (phaseQ < 0.0) phaseQ += barQ;
            }
            phase0 = (phaseQ < 0.0 ? 0.0 : phaseQ) * samplesPerQ;
            if (!havePrev_ || !prevHost_) b.discontinuity = true;
            else
            {
                const double expected = prevPpq_ + prevFrames_ / prevSamplesPerQ_;
                const double tolSamples = std::fmax(2.0, 0.005 * sr_);
                if (std::fabs(t.ppqPos - expected) * samplesPerQ > tolSamples) b.discontinuity = true;
            }
            prevPpq_ = t.ppqPos;
            prevSamplesPerQ_ = samplesPerQ;
        }
        else
        {
            phase0 = internalPhase_;
            if (!havePrev_) b.discontinuity = true;
        }
        if (phase0 >= b.barLen) phase0 = std::fmod(phase0, b.barLen);
        b.phase0 = phase0;
        b.hostLocked = host;

        // Downbeats inside this block: the sample nearest each exact downbeat time,
        // so ppq rounding (phase 88199.99999 vs 0.00001) cannot shift it by a sample.
        // A downbeat that rounds into both this block and the previous one is
        // de-duplicated by the engine.
        for (int k = 0; b.numDownbeats < ClockBlock::kMaxDownbeats; ++k)
        {
            const double off = std::floor(k * b.barLen - phase0 + 0.5);
            if (off >= numFrames) break;
            if (off >= 0.0 && (b.numDownbeats == 0 || (int)off != b.downbeats[b.numDownbeats - 1]))
                b.downbeats[b.numDownbeats++] = (int)off;
        }

        internalPhase_ = std::fmod(phase0 + numFrames, b.barLen);
        havePrev_ = true;
        prevHost_ = host;
        prevFrames_ = numFrames;
        return b;
    }

private:
    double sr_ = 44100.0;
    bool   havePrev_ = false, prevHost_ = false;
    double prevPpq_ = 0.0, prevSamplesPerQ_ = 1.0;
    int    prevFrames_ = 0;
    double internalPhase_ = 0.0;
};

} // namespace slowops::looper
