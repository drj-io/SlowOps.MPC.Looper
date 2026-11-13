// LooperEngine.hpp - one stereo, bar-length loop per instance.
//
// Host-agnostic core of Slow Ops Looper (std library only; no plugin SDK).
// SPDX-License-Identifier: MIT
//
// Signal flow (per sample):
//
//   in ──┬──────────────────────────────────────────────► dry ─┐
//        ├──► history ring (always recording the dry input)    ├─► out = dry*dryGain + loop*wetGain
//        │        │ capture window [start, end) copied         │
//        │        ▼ (chunked, then seam crossfade)             │
//        │     loop buffer A/B ──► drift read ──► tone LP ─────┘
//        │        ▲    │
//        └────────┘    └──► overdub write: loop = softclip(toneLP(loop) * feedback + in)
//
// Capture windows (all three modes pick a range of history):
//   Last Complete Bar  [previous downbeat, last downbeat)
//   Rolling Bar        [now - bar, now)
//   Next Bar           [next downbeat, next downbeat + bar)  (armed until then)
//
// Thread model: prepare()/reset() on a non-realtime thread (or between
// blocks); process() on the audio thread; requestCapture()/requestClear() from
// any thread (atomic flags). prepare() is the only function that allocates.
#pragma once

#include "BarClock.hpp"
#include "Dsp.hpp"
#include "StereoRing.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace slowops::looper {

enum class CaptureMode : int { LastCompleteBar = 0, RollingBar = 1, NextBar = 2 };
enum class PlaybackMode : int { Freeze = 0, OverdubOnDemand = 1, ContinuousOverdub = 2 };

// Order matters: the MPC adapter maps this 1:1 onto its Status choice parameter.
enum class Status : int
{
    NoHistory = 0,   // not enough audio history to capture in the current mode
    Ready,           // capture would succeed now
    Armed,           // Next Bar waiting for the downbeat, or a capture being copied
    Recording,       // Next Bar recording its bar
    Playing,         // loop playing
    Overdubbing,     // loop playing and taking new audio
    TooLong          // one bar is longer than the loop memory (very slow tempo)
};

struct Params
{
    float feedback = 0.7f;   // 0..1  share of the previous pass kept while overdubbing
    float tone = 1.0f;       // 0..1  loop lowpass, 400 Hz .. 18 kHz (1 = flat)
    float drift = 0.15f;     // 0..1  tape wow depth on loop playback
    float mix = 0.5f;        // 0..1  equal-power live/loop blend (dry only while no loop)
    CaptureMode  captureMode = CaptureMode::LastCompleteBar;
    PlaybackMode playbackMode = PlaybackMode::OverdubOnDemand;
    bool overdub = false;    // used by OverdubOnDemand
};

class LooperEngine
{
public:
    static constexpr double kSeamSeconds  = 0.010;   // loop seam crossfade
    static constexpr double kSwapSeconds  = 0.020;   // old loop -> new loop crossfade
    static constexpr double kFadeSeconds  = 0.020;   // loop presence fade (first capture / clear)
    static constexpr double kDubSeconds   = 0.010;   // overdub engage ramp
    static constexpr double kHoldSeconds  = 2.0;     // how long a refused capture stays on screen
    static constexpr double kMaxDriftSec  = 0.006;   // max drift read offset
    static constexpr int64_t kCopyChunk   = 32768;   // frames copied per block
    static constexpr int64_t kCopyMargin  = 16384;   // history kept free for in-flight copies

    // ---- setup (allocates) -------------------------------------------------
    void prepare(double sampleRate, int maxBlockSize, double maxBarSeconds = 6.0)
    {
        (void)maxBlockSize;
        const bool srChanged = sampleRate != sr_ || maxBarSeconds != maxBarSec_;
        sr_ = sampleRate;
        maxBarSec_ = maxBarSeconds;
        maxLen_ = (int64_t)std::ceil(maxBarSeconds * sampleRate);
        seam_ = (int64_t)std::lround(kSeamSeconds * sampleRate);
        swapLen_ = (int64_t)std::lround(kSwapSeconds * sampleRate);
        holdLen_ = (int64_t)std::lround(kHoldSeconds * sampleRate);
        maxDrift_ = (float)(kMaxDriftSec * sampleRate);
        if (srChanged || hist_.capacity() < maxLen_)
        {
            // Last Complete Bar can reach back almost two bars (pressed just before a downbeat).
            hist_.allocate(2 * maxLen_ + seam_ + kCopyMargin + (int64_t)(0.25 * sampleRate));
            for (Loop& L : loops_) { L.l.assign((size_t)maxLen_, 0.0f); L.r.assign((size_t)maxLen_, 0.0f); L.valid = false; L.len = 0; }
            now_ = 0;
            validFrom_ = 0;
            lastDB_ = prevDB_ = -1;
        }
        clock_.prepare(sampleRate);
        presence_.setTime((int)std::lround(kFadeSeconds * sampleRate));
        dub_.setTime((int)std::lround(kDubSeconds * sampleRate));
        drift_.prepare(sampleRate);
        prepared_ = true;
        reset();
    }

    // Clears filters, ramps and pending/armed captures. Keeps loop audio and
    // history (the transport gap is detected as a clock discontinuity).
    void reset()
    {
        clock_.reset();
        for (int c = 0; c < 2; ++c) { fbLp_[c].reset(); outLp_[c].reset(); }
        drift_.reset();
        arm_ = Arm::None;
        copyPending_ = false;
        clearing_ = false;
        swapRemain_ = 0;
        if (loops_[1 - active_].valid && loops_[active_].valid) loops_[1 - active_].valid = false;
        presence_.snap(loops_[active_].valid ? 1.0f : 0.0f);
        dub_.snap(0.0f);
        holdRemain_ = 0;
        captureReq_.store(false);
        clearReq_.store(false);
        mixFirst_ = true;
    }

    void requestCapture() { captureReq_.store(true, std::memory_order_release); }
    void requestClear()   { clearReq_.store(true, std::memory_order_release); }

    Status status() const { return status_; }
    bool   hasLoop() const { return loops_[active_].valid && !clearing_; }
    int64_t loopLength() const { return loops_[active_].valid ? loops_[active_].len : 0; }
    int64_t maxLoopLength() const { return maxLen_; }
    int64_t samplePosition() const { return now_; }

    // Read-only view of the active loop (tests, future waveform display).
    const float* loopLeft() const  { return loops_[active_].l.data(); }
    const float* loopRight() const { return loops_[active_].r.data(); }

    // ---- audio ---------------------------------------------------------------
    // In-place safe: each frame's input is read before that frame's output is written.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames,
                 const TransportInfo& transport, const Params& p)
    {
        if (!prepared_ || numFrames <= 0) return;
        if (inR == nullptr) inR = inL;

        const ClockBlock cb = clock_.advance(transport, numFrames);
        barLen_ = cb.barLen;
        const int64_t barLenI = (int64_t)std::llround(cb.barLen);
        mode_ = p.captureMode;

        if (cb.discontinuity)
        {
            lastDB_ = prevDB_ = -1;
            if (arm_ == Arm::Recording) arm_ = Arm::Waiting;
            const int64_t ph = (int64_t)std::llround(cb.phase0);
            for (Loop& L : loops_)
                if (L.valid) L.idx = wrap(ph + L.offset, L.len);
        }

        if (clearReq_.exchange(false, std::memory_order_acq_rel)) doClear();
        if (captureReq_.exchange(false, std::memory_order_acq_rel)) doCapture(p.captureMode, barLenI);
        serviceCopy(cb.phase0);

        // ---- per-block control values ----
        const double toneHz = 400.0 * std::pow(18000.0 / 400.0, (double)clampv(p.tone, 0.0f, 1.0f));
        for (int c = 0; c < 2; ++c)
        {
            if (p.tone >= 0.999f) { fbLp_[c].coef = 1.0f; outLp_[c].coef = 1.0f; }
            else { fbLp_[c].setCutoff(toneHz, sr_); outLp_[c].setCutoff(toneHz, sr_); }
        }
        const float mixA = (float)(clampv(p.mix, 0.0f, 1.0f) * kPi * 0.5);
        const float dryT = p.mix >= 1.0f ? 0.0f : std::cos(mixA);
        const float wetT = p.mix <= 0.0f ? 0.0f : std::sin(mixA);
        const float fbT = clampv(p.feedback, 0.0f, 1.0f);
        const float drT = clampv(p.drift, 0.0f, 1.0f) * maxDrift_;
        if (mixFirst_) { dryMix_ = dryT; wetMix_ = wetT; fb_ = fbT; driftDepth_ = drT; mixFirst_ = false; }
        const float inv = 1.0f / (float)numFrames;
        const float dDry = (dryT - dryMix_) * inv, dWet = (wetT - wetMix_) * inv;
        const float dFb = (fbT - fb_) * inv, dDr = (drT - driftDepth_) * inv;

        const bool wantDub = !clearing_ &&
            (p.playbackMode == PlaybackMode::ContinuousOverdub ||
             (p.playbackMode == PlaybackMode::OverdubOnDemand && p.overdub));
        dub_.setTarget(wantDub ? 1.0f : 0.0f);
        drift_.renormalise();

        int dbi = 0;
        for (int i = 0; i < numFrames; ++i)
        {
            while (dbi < cb.numDownbeats && cb.downbeats[dbi] == i) { onDownbeat(cb.barLen); ++dbi; }

            const float xl = inL[i], xr = inR[i];   // read input before any output write
            hist_.write(now_, xl, xr);

            dryMix_ += dDry; wetMix_ += dWet; fb_ += dFb; driftDepth_ += dDr;
            const float pres = presence_.next();
            const float dub = dub_.next();
            const float d = driftDepth_ * drift_.next();

            float ll = 0.0f, lr = 0.0f;
            Loop& A = loops_[active_];
            if (A.valid)
            {
                readLoop(A, d, ll, lr);
                if (swapRemain_ > 0)
                {
                    Loop& O = loops_[fadeFrom_];
                    float ol = 0.0f, orr = 0.0f;
                    readLoop(O, d, ol, orr);
                    const float x = 1.0f - (float)swapRemain_ / (float)swapLen_;
                    ll = ll * x + ol * (1.0f - x);
                    lr = lr * x + orr * (1.0f - x);
                    O.idx = O.idx + 1 >= O.len ? 0 : O.idx + 1;
                    if (--swapRemain_ == 0) O.valid = false;
                }
                ll = outLp_[0].process(ll);
                lr = outLp_[1].process(lr);

                if (dub > 0.0f)
                {
                    float& sl = A.l[(size_t)A.idx];
                    float& sr = A.r[(size_t)A.idx];
                    const float nl = softClip(fbLp_[0].process(sl) * fb_ + xl);
                    const float nr = softClip(fbLp_[1].process(sr) * fb_ + xr);
                    sl += dub * (nl - sl);
                    sr += dub * (nr - sr);
                }
                A.idx = A.idx + 1 >= A.len ? 0 : A.idx + 1;
            }

            const float dryG = 1.0f - pres * (1.0f - dryMix_);
            const float wetG = pres * wetMix_;
            outL[i] = xl * dryG + ll * wetG;
            outR[i] = xr * dryG + lr * wetG;

            ++now_;
            if (arm_ == Arm::Recording && now_ >= recStart_ + recLen_)
            {
                arm_ = Arm::None;
                queueCopy(recStart_, recLen_);
            }
        }

        if (clearing_ && presence_.value <= 0.0f)
        {
            loops_[0].valid = loops_[1].valid = false;
            loops_[0].len = loops_[1].len = 0;
            clearing_ = false;
            swapRemain_ = 0;
        }

        if (holdRemain_ > 0) holdRemain_ -= numFrames;
        status_ = computeStatus(barLenI, wantDub);
    }

private:
    enum class Arm { None, Waiting, Recording };

    struct Loop
    {
        std::vector<float> l, r;
        int64_t len = 0, idx = 0, offset = 0;
        bool valid = false;
    };

    void readLoop(const Loop& L, float d, float& outLeft, float& outRight) const
    {
        if (d <= 0.0f) { outLeft = L.l[(size_t)L.idx]; outRight = L.r[(size_t)L.idx]; return; }
        const float pos = (float)L.idx - d;
        const float fl = std::floor(pos);
        const float frac = pos - fl;
        const int64_t i0 = wrap((int64_t)fl, L.len);
        const int64_t i1 = i0 + 1 >= L.len ? 0 : i0 + 1;
        outLeft  = L.l[(size_t)i0] + frac * (L.l[(size_t)i1] - L.l[(size_t)i0]);
        outRight = L.r[(size_t)i0] + frac * (L.r[(size_t)i1] - L.r[(size_t)i0]);
    }

    int64_t oldestAvailable() const
    {
        const int64_t ringOldest = now_ - hist_.capacity() + kCopyMargin;
        return ringOldest > validFrom_ ? ringOldest : validFrom_;
    }

    void refuse(Status s) { holdStatus_ = s; holdRemain_ = holdLen_; }

    void onDownbeat(double barLen)
    {
        if (lastDB_ >= 0 && (double)(now_ - lastDB_) < barLen * 0.5) return;   // duplicate
        prevDB_ = lastDB_;
        lastDB_ = now_;
        if (arm_ == Arm::Waiting)
        {
            const int64_t len = (int64_t)std::llround(barLen);
            if (len > maxLen_) { arm_ = Arm::None; refuse(Status::TooLong); }
            else { arm_ = Arm::Recording; recStart_ = now_; recLen_ = len; }
        }
        Loop& A = loops_[active_];
        if (A.valid && std::fabs((double)A.len - barLen) <= 1.0) A.idx = wrap(A.offset, A.len);
    }

    void doClear()
    {
        arm_ = Arm::None;
        copyPending_ = false;
        holdRemain_ = 0;
        if (loops_[active_].valid) { clearing_ = true; presence_.setTarget(0.0f); }
    }

    void doCapture(CaptureMode mode, int64_t barLenI)
    {
        if (arm_ != Arm::None) { arm_ = Arm::None; return; }   // second press cancels Next Bar
        if (copyPending_) return;
        int64_t start = 0, len = 0;
        switch (mode)
        {
            case CaptureMode::LastCompleteBar:
                if (prevDB_ < 0 || lastDB_ < 0) { refuse(Status::NoHistory); return; }
                start = prevDB_; len = lastDB_ - prevDB_;
                break;
            case CaptureMode::RollingBar:
                len = barLenI; start = now_ - len;
                break;
            case CaptureMode::NextBar:
                if (barLenI > maxLen_) { refuse(Status::TooLong); return; }
                holdRemain_ = 0;
                arm_ = Arm::Waiting;
                return;
        }
        if (len > maxLen_) { refuse(Status::TooLong); return; }
        if (len < 2 * seam_ || start - seam_ < oldestAvailable()) { refuse(Status::NoHistory); return; }
        queueCopy(start, len);
    }

    void queueCopy(int64_t start, int64_t len)
    {
        holdRemain_ = 0;
        copyPending_ = true;
        copyStart_ = start;
        copyLen_ = len;
        copyDone_ = 0;
    }

    void serviceCopy(double phase0)
    {
        if (!copyPending_) return;
        if (copyDone_ == 0)
        {
            if (swapRemain_ > 0) return;   // the other buffer is still fading out
            copyTarget_ = loops_[active_].valid ? 1 - active_ : active_;
        }
        Loop& L = loops_[copyTarget_];
        const int64_t n = (copyLen_ - copyDone_) < kCopyChunk ? (copyLen_ - copyDone_) : kCopyChunk;
        for (int64_t k = 0; k < n; ++k)
        {
            const int64_t t = copyStart_ + copyDone_ + k;
            L.l[(size_t)(copyDone_ + k)] = hist_.left(t);
            L.r[(size_t)(copyDone_ + k)] = hist_.right(t);
        }
        copyDone_ += n;
        if (copyDone_ < copyLen_) return;

        // Seam: blend the loop's last `seam` samples into the audio that
        // preceded the window start, so end -> start is continuous.
        const int64_t len = copyLen_;
        const int64_t xf = seam_ < len / 4 ? seam_ : len / 4;
        for (int64_t i = 0; i < xf; ++i)
        {
            const double th = 0.5 * kPi * ((double)i + 0.5) / (double)xf;
            const float a = (float)std::cos(th), b = (float)std::sin(th);
            const int64_t j = len - xf + i;
            const int64_t pre = copyStart_ - xf + i;
            L.l[(size_t)j] = hist_.left(copyStart_ + j) * a + hist_.left(pre) * b;
            L.r[(size_t)j] = hist_.right(copyStart_ + j) * a + hist_.right(pre) * b;
        }

        L.len = len;
        L.valid = true;
        L.idx = wrap(now_ - copyStart_, len);
        L.offset = wrap(L.idx - (int64_t)std::llround(phase0), len);
        if (copyTarget_ != active_ && loops_[active_].valid)
        {
            fadeFrom_ = active_;
            swapRemain_ = swapLen_;
        }
        active_ = copyTarget_;
        clearing_ = false;
        presence_.setTarget(1.0f);
        fbLp_[0].reset(); fbLp_[1].reset();
        copyPending_ = false;
    }

    Status computeStatus(int64_t barLenI, bool wantDub) const
    {
        if (holdRemain_ > 0) return holdStatus_;
        if (arm_ == Arm::Waiting || copyPending_) return Status::Armed;
        if (arm_ == Arm::Recording) return Status::Recording;
        if (loops_[active_].valid && !clearing_) return wantDub ? Status::Overdubbing : Status::Playing;
        if (barLenI > maxLen_) return Status::TooLong;
        switch (mode_)
        {
            case CaptureMode::LastCompleteBar:
                return (prevDB_ >= 0 && lastDB_ - prevDB_ <= maxLen_ && prevDB_ - seam_ >= oldestAvailable())
                           ? Status::Ready : Status::NoHistory;
            case CaptureMode::RollingBar:
                return (now_ - barLenI - seam_ >= oldestAvailable()) ? Status::Ready : Status::NoHistory;
            case CaptureMode::NextBar:
            default:
                return Status::Ready;
        }
    }

    // configuration
    double  sr_ = 0.0, maxBarSec_ = 0.0;
    int64_t maxLen_ = 0, seam_ = 0, swapLen_ = 0, holdLen_ = 0;
    float   maxDrift_ = 0.0f;
    bool    prepared_ = false;

    // timeline
    BarClock   clock_;
    StereoRing hist_;
    int64_t now_ = 0, validFrom_ = 0;
    int64_t lastDB_ = -1, prevDB_ = -1;
    double  barLen_ = 0.0;
    CaptureMode mode_ = CaptureMode::LastCompleteBar;

    // loops
    Loop loops_[2];
    int  active_ = 0, fadeFrom_ = 1;
    int64_t swapRemain_ = 0;
    bool clearing_ = false;

    // capture
    Arm arm_ = Arm::None;
    int64_t recStart_ = 0, recLen_ = 0;
    bool copyPending_ = false;
    int  copyTarget_ = 0;
    int64_t copyStart_ = 0, copyLen_ = 0, copyDone_ = 0;

    // status
    Status status_ = Status::NoHistory, holdStatus_ = Status::NoHistory;
    int64_t holdRemain_ = 0;

    // control
    std::atomic<bool> captureReq_{false}, clearReq_{false};
    Ramp presence_, dub_;
    OnePole fbLp_[2], outLp_[2];
    DriftSource drift_;
    float dryMix_ = 1.0f, wetMix_ = 0.0f, fb_ = 0.7f, driftDepth_ = 0.0f;
    bool  mixFirst_ = true;
};

} // namespace slowops::looper
