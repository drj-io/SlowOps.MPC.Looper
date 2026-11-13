// Dsp.hpp - small DSP helpers for the Slow Ops looper core.
// Host-agnostic: standard library only. SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <cstdint>

namespace slowops::looper {

constexpr double kPi = 3.14159265358979323846;

template <typename T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Positive modulo for loop indices.
inline int64_t wrap(int64_t i, int64_t len) { int64_t r = i % len; return r < 0 ? r + len : r; }

// Gentle cubic soft clipper: ~unity below 0.5, saturates to +-1 at |x| >= 1.5.
// Overdub limiter: exactly linear up to kClipKnee (about -2 dBFS), so a loop kept at 100 %
// Feedback never loses level pass after pass; above it a tanh shoulder (same slope at the
// knee) approaches +-1 smoothly, so stacked layers are limited, not hard-clipped.
constexpr float kClipKnee = 0.8f;
inline float softClip(float x)
{
    const float a = x < 0.0f ? -x : x;
    if (a <= kClipKnee) return x;
    const float y = kClipKnee + (1.0f - kClipKnee) * std::tanh((a - kClipKnee) / (1.0f - kClipKnee));
    return x < 0.0f ? -y : y;
}

// One-pole lowpass. coef == 1 is an exact pass-through.
struct OnePole
{
    float state = 0.0f;
    float coef = 1.0f;
    void setCutoff(double hz, double sr)
    {
        if (hz >= 0.45 * sr) { coef = 1.0f; return; }
        coef = (float)(1.0 - std::exp(-2.0 * kPi * hz / sr));
    }
    float process(float x) { state += coef * (x - state); return state; }
    void reset() { state = 0.0f; }
};

// Linear ramp toward a target over a fixed number of samples (click-free gains).
struct Ramp
{
    float value = 0.0f, target = 0.0f, step = 0.0f;
    int   samples = 1;
    void setTime(int n) { samples = n < 1 ? 1 : n; }
    void setTarget(float t)
    {
        if (t == target) return;
        target = t;
        step = (target - value) / (float)samples;
    }
    void snap(float v) { value = target = v; step = 0.0f; }
    float next()
    {
        if (value != target)
        {
            value += step;
            if ((step > 0.0f && value >= target) || (step < 0.0f && value <= target) || step == 0.0f) value = target;
        }
        return value;
    }
};

// Tape-style drift source: a slow sine (recursive oscillator, no per-sample
// transcendental calls) blended with smoothed random wander. Output 0..1.
struct DriftSource
{
    double s = 0.0, c = 1.0, cw = 1.0, sw = 0.0;
    float  rnd = 0.0f, rndTarget = 0.0f, rndCoef = 0.0f;
    int    holdLeft = 0, holdLen = 1;
    uint32_t seed = 0x9E3779B9u;

    void prepare(double sr, double rateHz = 0.37)
    {
        const double w = 2.0 * kPi * rateHz / sr;
        cw = std::cos(w); sw = std::sin(w);
        rndCoef = (float)(1.0 - std::exp(-1.0 / (0.4 * sr)));
        holdLen = (int)(0.8 * sr);
        reset();
    }
    void reset() { s = 0.0; c = 1.0; rnd = rndTarget = 0.0f; holdLeft = 0; }
    float nextRandom()
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return (float)((double)seed / 4294967295.0 * 2.0 - 1.0);
    }
    // Call once per block to keep the oscillator on the unit circle.
    void renormalise()
    {
        const double m = std::sqrt(s * s + c * c);
        if (m > 0.0) { s /= m; c /= m; } else { s = 0.0; c = 1.0; }
    }
    float next()
    {
        const double ns = s * cw + c * sw;
        c = c * cw - s * sw; s = ns;
        if (--holdLeft <= 0) { holdLeft = holdLen; rndTarget = nextRandom(); }
        rnd += rndCoef * (rndTarget - rnd);
        return 0.5f + 0.5f * (0.7f * (float)s + 0.3f * rnd);
    }
};

} // namespace slowops::looper
