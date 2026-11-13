// LooperEngine.hpp - three stereo loop banks, each 1/2/4/8 bars long.
//
// Host-agnostic core of Slow Ops Looper (std library only; no plugin SDK).
// SPDX-License-Identifier: MIT
//
// Signal flow (per sample):
//
//   in ──┬────────────────────────────────────────────────────────► dry ──┐
//        ├──► history ring (always recording the dry input)              │
//        │       │ capture window (N bars) copied into a free buffer      ├─► out = dry*dryGain
//        │       ▼                                                        │        + wetGain * Σ bank
//        │    bank 1..3: loop ──► drift read ──► tone LP ──► vol/solo/mute ┘
//        │        ▲   │
//        └────────┘   └──► overdub write: loop = softclip(toneLP(loop) * feedback + in)
//
// Capture windows (N = the bank's length in bars):
//   Last Complete Bar  the last N complete bars, [downbeat(k-N), downbeat(k))
//   Rolling Bar        [now - N bars, now)
//   Next Bar           [next downbeat, + N bars)  (armed until then)
// Every window keeps playing seamlessly from "now", so the loop continues the
// music in time with the host's bars.
//
// Memory: kNumBanks + 1 loop buffers of maxLoopSeconds are pooled. The spare
// buffer receives a new capture so a bank that is already playing can crossfade
// from its old loop to the new one; the old buffer then becomes the spare.
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

// Order matters: the MPC adapter maps this 1:1 onto its Status choice parameters.
enum class Status : int
{
    NoHistory = 0,   // not enough audio history to capture this bank's length
    Ready,           // capture would succeed now
    Armed,           // Next Bar waiting for the downbeat, or a capture being copied
    Recording,       // Next Bar recording
    Playing,         // loop playing
    Overdubbing,     // loop playing and taking new audio
    TooLong          // the bank's length is longer than the loop memory at this tempo
};

// Why the last capture request did what it did (diagnostics / on-screen debug).
enum class CaptureResult : int { None = 0, Queued, Armed, Cancelled, NoHistory, TooLong, Busy };

// Snapshot of what the engine saw, refreshed every block (audio thread only;
// copy it out on the audio thread if another thread needs it).
struct Diagnostics
{
    bool    playing = false, ppqValid = false, hostLocked = false;
    double  bpm = 0.0, ppqPos = 0.0, barLen = 0.0;
    int     timeSigNumerator = 4, timeSigDenominator = 4;
    int     beat = 0;                 // beat within the bar, 0-based
    int64_t barsElapsed = 0;          // downbeats seen since prepare
    int64_t discontinuities = 0;      // transport jumps / starts
    int64_t captureRequests[3] = {};  // per bank
    int64_t clearRequests[3] = {};
    int     lastBank = -1;
    CaptureResult lastResult = CaptureResult::None;
    int64_t lastLength = 0;           // samples of the last queued capture
    float   loopPeak = 0.0f;          // peak of the summed banks (before Mix) in the last block
};

struct GlobalParams
{
    float tone = 1.0f;    // 0..1  loop lowpass, 400 Hz .. 18 kHz (1 = flat)
    float drift = 0.15f;  // 0..1  tape wow depth on loop playback
    float mix = 0.5f;     // 0..1  equal-power live/loops blend (dry only while no loop exists)
    CaptureMode  captureMode = CaptureMode::LastCompleteBar;
    PlaybackMode playbackMode = PlaybackMode::OverdubOnDemand;
};

struct BankParams
{
    int   lengthBars = 1;    // 1, 2, 4 or 8; applies to the next capture
    float feedback = 0.7f;   // 0..1  share of the previous pass kept while overdubbing
    float volume = 1.0f;     // 0..1  (squared law: 1 = unity)
    bool  solo = false;      // when any bank is soloed, only soloed banks are heard
    bool  mute = false;
    bool  overdub = false;   // used by OverdubOnDemand
};

class LooperEngine
{
public:
    static constexpr int    kNumBanks     = 3;
    static constexpr int    kNumBuffers   = kNumBanks + 1;
    static constexpr double kSeamSeconds  = 0.010;   // loop seam crossfade
    static constexpr double kSwapSeconds  = 0.020;   // old loop -> new loop crossfade
    static constexpr double kFadeSeconds  = 0.020;   // loop presence fade (first capture / clear)
    static constexpr double kGainSeconds  = 0.020;   // volume / solo / mute ramps
    static constexpr double kDubSeconds   = 0.010;   // overdub engage ramp
    static constexpr double kHoldSeconds  = 2.0;     // how long a refused capture stays on screen
    static constexpr double kMaxDriftSec  = 0.006;   // max drift read offset
    static constexpr int64_t kCopyChunk   = 131072;  // frames copied per block (memcpy)
    static constexpr int64_t kCopyMargin  = 16384;   // history kept free for in-flight copies
    static constexpr int    kMaxDownbeats = 16;

    // ---- setup (allocates) -------------------------------------------------
    // maxLoopSeconds: longest loop per bank. extraHistorySeconds: history beyond
    // that, so "last N complete bars" still fits when pressed late in a bar.
    void prepare(double sampleRate, int maxBlockSize, double maxLoopSeconds = 32.0, double extraHistorySeconds = 5.0)
    {
        (void)maxBlockSize;
        const bool changed = sampleRate != sr_ || maxLoopSeconds != maxLoopSec_ || extraHistorySeconds != extraHistSec_;
        sr_ = sampleRate;
        maxLoopSec_ = maxLoopSeconds;
        extraHistSec_ = extraHistorySeconds;
        maxLen_ = (int64_t)std::ceil(maxLoopSeconds * sampleRate);
        seam_ = (int64_t)std::lround(kSeamSeconds * sampleRate);
        swapLen_ = (int64_t)std::lround(kSwapSeconds * sampleRate);
        holdLen_ = (int64_t)std::lround(kHoldSeconds * sampleRate);
        maxDrift_ = (float)(kMaxDriftSec * sampleRate);
        if (changed)
        {
            hist_.allocate(maxLen_ + (int64_t)std::ceil(extraHistorySeconds * sampleRate) + seam_ + kCopyMargin);
            for (Buffer& b : bufs_) { b.l.assign((size_t)maxLen_, 0.0f); b.r.assign((size_t)maxLen_, 0.0f); }
            for (Bank& k : banks_) { k.valid = false; k.buf = k.oldBuf = -1; k.len = 0; }
            now_ = 0;
            validFrom_ = 0;
            dbCount_ = 0;
        }
        clock_.prepare(sampleRate);
        for (Bank& k : banks_)
        {
            k.presence.setTime((int)std::lround(kFadeSeconds * sampleRate));
            k.gain.setTime((int)std::lround(kGainSeconds * sampleRate));
            k.dub.setTime((int)std::lround(kDubSeconds * sampleRate));
        }
        drift_.prepare(sampleRate);
        prepared_ = true;
        reset();
    }

    // Clears filters, ramps and pending/armed captures. Keeps loop audio and
    // history (the transport gap is detected as a clock discontinuity).
    void reset()
    {
        clock_.reset();
        drift_.reset();
        copyBank_ = -1;
        for (Bank& k : banks_)
        {
            for (int c = 0; c < 2; ++c) { k.fbLp[c].reset(); k.outLp[c].reset(); }
            k.arm = Arm::None;
            k.copyPending = false;
            k.copyBuf = -1;
            if (k.clearing) { k.valid = false; k.buf = -1; k.clearing = false; }
            k.oldBuf = -1;
            k.swapRemain = 0;
            k.presence.snap(k.valid ? 1.0f : 0.0f);
            k.dub.snap(0.0f);
            k.holdRemain = 0;
            k.captureReq.store(false);
            k.clearReq.store(false);
            k.firstBlock = true;
        }
        mixFirst_ = true;
    }

    void requestCapture(int bank) { if (bank >= 0 && bank < kNumBanks) banks_[bank].captureReq.store(true, std::memory_order_release); }
    void requestClear(int bank)   { if (bank >= 0 && bank < kNumBanks) banks_[bank].clearReq.store(true, std::memory_order_release); }

    Status  status(int bank) const      { return banks_[bank].status; }
    bool    hasLoop(int bank) const     { return banks_[bank].valid && !banks_[bank].clearing; }
    int64_t loopLength(int bank) const  { return banks_[bank].valid ? banks_[bank].len : 0; }
    int     loopBars(int bank) const    { return banks_[bank].valid ? banks_[bank].bars : 0; }
    // Playback position 0..1 within the loop (0 when empty).
    float   progress(int bank) const
    {
        const Bank& k = banks_[bank];
        return (k.valid && k.len > 0) ? (float)((double)k.idx / (double)k.len) : 0.0f;
    }
    const Diagnostics& diagnostics() const { return diag_; }
    int64_t maxLoopLength() const   { return maxLen_; }
    int64_t samplePosition() const  { return now_; }
    const float* loopLeft(int bank) const  { const Bank& k = banks_[bank]; return k.buf >= 0 ? bufs_[k.buf].l.data() : nullptr; }
    const float* loopRight(int bank) const { const Bank& k = banks_[bank]; return k.buf >= 0 ? bufs_[k.buf].r.data() : nullptr; }

    // ---- audio ---------------------------------------------------------------
    // In-place safe: each frame's input is read before that frame's output is written.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames,
                 const TransportInfo& transport, const GlobalParams& g, const BankParams bp[kNumBanks])
    {
        if (!prepared_ || numFrames <= 0) return;
        if (inR == nullptr) inR = inL;

        const ClockBlock cb = clock_.advance(transport, numFrames);
        barLen_ = cb.barLen;
        mode_ = g.captureMode;
        diag_.playing = transport.playing;
        diag_.ppqValid = transport.ppqValid;
        diag_.hostLocked = cb.hostLocked;
        diag_.bpm = transport.bpm;
        diag_.ppqPos = transport.ppqPos;
        diag_.barLen = cb.barLen;
        diag_.timeSigNumerator = transport.timeSigNumerator;
        diag_.timeSigDenominator = transport.timeSigDenominator;
        {
            const int num = clampv(transport.timeSigNumerator, 1, 32);
            const double phaseEnd = std::fmod(cb.phase0 + numFrames, cb.barLen);
            diag_.beat = clampv((int)std::floor(phaseEnd / (cb.barLen / num)), 0, num - 1);
        }

        if (cb.discontinuity)
        {
            ++diag_.discontinuities;
            dbCount_ = 0;
            for (Bank& k : banks_)
            {
                if (k.arm == Arm::Recording) k.arm = Arm::Waiting;
                if (k.valid) resyncBank(k, cb.phase0);
            }
        }

        for (int b = 0; b < kNumBanks; ++b)
        {
            Bank& k = banks_[b];
            k.bars = (k.valid ? k.bars : 0);
            k.nextBars = clampBars(bp[b].lengthBars);
            if (k.clearReq.exchange(false, std::memory_order_acq_rel)) { ++diag_.clearRequests[b]; doClear(b); }
            if (k.captureReq.exchange(false, std::memory_order_acq_rel)) { ++diag_.captureRequests[b]; doCapture(b, g.captureMode); }
        }
        serviceCopy(cb.phase0);

        // ---- per-block control values ----
        const double toneHz = 400.0 * std::pow(18000.0 / 400.0, (double)clampv(g.tone, 0.0f, 1.0f));
        const float mixA = (float)(clampv(g.mix, 0.0f, 1.0f) * kPi * 0.5);
        const float dryT = g.mix >= 1.0f ? 0.0f : std::cos(mixA);
        const float wetT = g.mix <= 0.0f ? 0.0f : std::sin(mixA);
        const float drT = clampv(g.drift, 0.0f, 1.0f) * maxDrift_;
        if (mixFirst_) { dryMix_ = dryT; wetMix_ = wetT; driftDepth_ = drT; mixFirst_ = false; }
        const float inv = 1.0f / (float)numFrames;
        const float dDry = (dryT - dryMix_) * inv, dWet = (wetT - wetMix_) * inv, dDr = (drT - driftDepth_) * inv;

        bool anySolo = false;
        for (int b = 0; b < kNumBanks; ++b) anySolo = anySolo || bp[b].solo;
        for (int b = 0; b < kNumBanks; ++b)
        {
            Bank& k = banks_[b];
            for (int c = 0; c < 2; ++c)
            {
                if (g.tone >= 0.999f) { k.fbLp[c].coef = 1.0f; k.outLp[c].coef = 1.0f; }
                else { k.fbLp[c].setCutoff(toneHz, sr_); k.outLp[c].setCutoff(toneHz, sr_); }
            }
            const float fbT = clampv(bp[b].feedback, 0.0f, 1.0f);
            if (k.firstBlock) { k.fb = fbT; }
            k.dFb = (fbT - k.fb) * inv;
            const bool audible = !bp[b].mute && (!anySolo || bp[b].solo);
            const float v = clampv(bp[b].volume, 0.0f, 1.0f);
            const float gainT = audible ? v * v : 0.0f;
            if (k.firstBlock) k.gain.snap(gainT); else k.gain.setTarget(gainT);
            k.firstBlock = false;
            k.wantDub = k.valid && !k.clearing &&
                (g.playbackMode == PlaybackMode::ContinuousOverdub ||
                 (g.playbackMode == PlaybackMode::OverdubOnDemand && bp[b].overdub));
            k.dub.setTarget(k.wantDub ? 1.0f : 0.0f);
        }
        drift_.renormalise();

        float loopPeak = 0.0f;
        int dbi = 0;
        for (int i = 0; i < numFrames; ++i)
        {
            while (dbi < cb.numDownbeats && cb.downbeats[dbi] == i) { onDownbeat(cb.barLen); ++dbi; }

            const float xl = inL[i], xr = inR[i];   // read input before any output write
            hist_.write(now_, xl, xr);

            dryMix_ += dDry; wetMix_ += dWet; driftDepth_ += dDr;
            const float d = driftDepth_ * drift_.next();

            float wl = 0.0f, wr = 0.0f, presMax = 0.0f;
            for (Bank& k : banks_)
            {
                const float pres = k.presence.next();
                const float gain = k.gain.next();
                const float dub = k.dub.next();
                k.fb += k.dFb;
                if (!k.valid) continue;
                if (pres > presMax) presMax = pres;

                Buffer& B = bufs_[k.buf];
                float ll = 0.0f, lr = 0.0f;
                readLoop(B, k.len, k.idx, d, ll, lr);
                if (k.swapRemain > 0)
                {
                    float ol = 0.0f, orr = 0.0f;
                    readLoop(bufs_[k.oldBuf], k.oldLen, k.oldIdx, d, ol, orr);
                    const float x = 1.0f - (float)k.swapRemain / (float)swapLen_;
                    ll = ll * x + ol * (1.0f - x);
                    lr = lr * x + orr * (1.0f - x);
                    k.oldIdx = k.oldIdx + 1 >= k.oldLen ? 0 : k.oldIdx + 1;
                    if (--k.swapRemain == 0) k.oldBuf = -1;   // old buffer returns to the pool
                }
                ll = k.outLp[0].process(ll);
                lr = k.outLp[1].process(lr);

                if (dub > 0.0f)
                {
                    float& sl = B.l[(size_t)k.idx];
                    float& sr = B.r[(size_t)k.idx];
                    const float nl = softClip(k.fbLp[0].process(sl) * k.fb + xl);
                    const float nr = softClip(k.fbLp[1].process(sr) * k.fb + xr);
                    sl += dub * (nl - sl);
                    sr += dub * (nr - sr);
                }
                k.idx = k.idx + 1 >= k.len ? 0 : k.idx + 1;

                const float gp = gain * pres;
                wl += ll * gp;
                wr += lr * gp;
            }

            const float aw = std::fabs(wl) > std::fabs(wr) ? std::fabs(wl) : std::fabs(wr);
            if (aw > loopPeak) loopPeak = aw;
            const float dryG = 1.0f - presMax * (1.0f - dryMix_);
            outL[i] = xl * dryG + wl * wetMix_;
            outR[i] = xr * dryG + wr * wetMix_;

            ++now_;
            for (Bank& k : banks_)
                if (k.arm == Arm::Recording && now_ >= k.recStart + k.recLen)
                {
                    k.arm = Arm::None;
                    queueCopy(k, k.recStart, k.recLen, k.recBars);
                }
        }

        diag_.loopPeak = loopPeak;
        for (int b = 0; b < kNumBanks; ++b)
        {
            Bank& k = banks_[b];
            if (k.clearing && k.presence.value <= 0.0f)
            {
                k.valid = false; k.clearing = false;
                k.buf = k.oldBuf = -1; k.swapRemain = 0; k.len = 0;
            }
            if (k.holdRemain > 0) k.holdRemain -= numFrames;
            k.status = computeStatus(k);
        }
    }

private:
    enum class Arm { None, Waiting, Recording };

    struct Buffer { std::vector<float> l, r; };

    struct Bank
    {
        // loop
        int buf = -1;
        int64_t len = 0, idx = 0;
        double offsetBar = 0.0;   // playback offset within a bar, centred on 0
        int bars = 0;             // length of the current loop in bars
        bool valid = false, clearing = false;
        // crossfade from a previous loop
        int oldBuf = -1;
        int64_t oldLen = 0, oldIdx = 0, swapRemain = 0;
        // capture
        Arm arm = Arm::None;
        int nextBars = 1, recBars = 1;
        int64_t recStart = 0, recLen = 0;
        bool copyPending = false;
        int copyBuf = -1, copyBars = 1;
        int64_t copyStart = 0, copyLen = 0, copyDone = 0;
        // status
        Status status = Status::NoHistory, holdStatus = Status::NoHistory;
        int64_t holdRemain = 0;
        // control
        std::atomic<bool> captureReq{false}, clearReq{false};
        Ramp presence, gain, dub;
        OnePole fbLp[2], outLp[2];
        float fb = 0.7f, dFb = 0.0f;
        bool wantDub = false, firstBlock = true;
    };

    static int clampBars(int n) { return n <= 1 ? 1 : (n <= 2 ? 2 : (n <= 4 ? 4 : 8)); }

    void readLoop(const Buffer& B, int64_t len, int64_t idx, float d, float& outLeft, float& outRight) const
    {
        if (d <= 0.0f) { outLeft = B.l[(size_t)idx]; outRight = B.r[(size_t)idx]; return; }
        const float pos = (float)idx - d;
        const float fl = std::floor(pos);
        const float frac = pos - fl;
        const int64_t i0 = wrap((int64_t)fl, len);
        const int64_t i1 = i0 + 1 >= len ? 0 : i0 + 1;
        outLeft  = B.l[(size_t)i0] + frac * (B.l[(size_t)i1] - B.l[(size_t)i0]);
        outRight = B.r[(size_t)i0] + frac * (B.r[(size_t)i1] - B.r[(size_t)i0]);
    }

    int64_t oldestAvailable() const
    {
        const int64_t ringOldest = now_ - hist_.capacity() + kCopyMargin;
        return ringOldest > validFrom_ ? ringOldest : validFrom_;
    }

    // Downbeat k back from the latest (0 = latest). Requires k < dbCount_.
    int64_t downbeat(int back) const { return db_[(dbHead_ - 1 - back + kMaxDownbeats) % kMaxDownbeats]; }

    bool tempoMatches(const Bank& k) const { return std::fabs((double)k.len - k.bars * barLen_) <= k.bars + 1.0; }

    // Keep the loop's bar-within-loop, move to the host's bar phase (relocate / restart).
    void resyncBank(Bank& k, double phase0)
    {
        if (!tempoMatches(k)) return;
        const int bar = clampv((int)std::floor(((double)k.idx - k.offsetBar) / barLen_), 0, k.bars - 1);
        k.idx = wrap((int64_t)std::llround(bar * barLen_ + phase0 + k.offsetBar), k.len);
    }

    void refuse(Bank& k, Status s)
    {
        k.holdStatus = s; k.holdRemain = holdLen_;
        diag_.lastBank = (int)(&k - banks_);
        diag_.lastResult = s == Status::TooLong ? CaptureResult::TooLong : CaptureResult::NoHistory;
        diag_.lastLength = 0;
    }
    void noteResult(int b, CaptureResult r, int64_t len = 0) { diag_.lastBank = b; diag_.lastResult = r; diag_.lastLength = len; }

    void onDownbeat(double barLen)
    {
        if (dbCount_ > 0 && (double)(now_ - downbeat(0)) < barLen * 0.5) return;   // duplicate
        ++diag_.barsElapsed;
        db_[dbHead_] = now_;
        dbHead_ = (dbHead_ + 1) % kMaxDownbeats;
        if (dbCount_ < kMaxDownbeats) ++dbCount_;
        for (Bank& k : banks_)
        {
            if (k.arm == Arm::Waiting)
            {
                const int64_t len = (int64_t)std::llround(k.recBars * barLen);
                if (len > maxLen_) { k.arm = Arm::None; refuse(k, Status::TooLong); }
                else { k.arm = Arm::Recording; k.recStart = now_; k.recLen = len; }
            }
            // Snap to the nearest bar line of the loop so it never drifts from the host.
            if (k.valid && tempoMatches(k))
            {
                const int bar = (int)wrap((int64_t)std::floor(((double)k.idx - k.offsetBar) / barLen + 0.5), k.bars);
                k.idx = wrap((int64_t)std::llround(bar * barLen + k.offsetBar), k.len);
            }
        }
    }

    void doClear(int b)
    {
        Bank& k = banks_[b];
        k.arm = Arm::None;
        k.holdRemain = 0;
        if (k.copyPending) { k.copyPending = false; k.copyBuf = -1; if (copyBank_ == b) copyBank_ = -1; }
        if (k.valid) { k.clearing = true; k.presence.setTarget(0.0f); }
    }

    void doCapture(int b, CaptureMode mode)
    {
        Bank& k = banks_[b];
        if (k.arm != Arm::None) { k.arm = Arm::None; noteResult(b, CaptureResult::Cancelled); return; }   // second press cancels Next Bar
        if (k.copyPending) { noteResult(b, CaptureResult::Busy); return; }
        const int n = k.nextBars;
        const int64_t barsLen = (int64_t)std::llround(n * barLen_);
        int64_t start = 0, len = 0;
        switch (mode)
        {
            case CaptureMode::LastCompleteBar:
                if (dbCount_ < n + 1) { refuse(k, barsLen > maxLen_ ? Status::TooLong : Status::NoHistory); return; }
                start = downbeat(n); len = downbeat(0) - start;
                break;
            case CaptureMode::RollingBar:
                len = barsLen; start = now_ - len;
                break;
            case CaptureMode::NextBar:
                if (barsLen > maxLen_) { refuse(k, Status::TooLong); return; }
                k.holdRemain = 0;
                k.recBars = n;
                k.arm = Arm::Waiting;
                noteResult(b, CaptureResult::Armed, barsLen);
                return;
        }
        if (len > maxLen_) { refuse(k, Status::TooLong); return; }
        if (len < 4 * seam_ || start - seam_ < oldestAvailable()) { refuse(k, Status::NoHistory); return; }
        queueCopy(k, start, len, n);
        noteResult(b, CaptureResult::Queued, len);
    }

    void queueCopy(Bank& k, int64_t start, int64_t len, int bars)
    {
        k.holdRemain = 0;
        k.copyPending = true;
        k.copyStart = start;
        k.copyLen = len;
        k.copyDone = 0;
        k.copyBars = bars;
        k.copyBuf = -1;
    }

    int freeBuffer() const
    {
        bool used[kNumBuffers] = {};
        for (const Bank& k : banks_)
        {
            if (k.valid && k.buf >= 0) used[k.buf] = true;
            if (k.oldBuf >= 0) used[k.oldBuf] = true;
            if (k.copyBuf >= 0) used[k.copyBuf] = true;
        }
        for (int i = 0; i < kNumBuffers; ++i) if (!used[i]) return i;
        return -1;
    }

    // One capture copies at a time, in chunks, into a free pooled buffer.
    void serviceCopy(double phase0)
    {
        if (copyBank_ < 0)
            for (int b = 0; b < kNumBanks; ++b)
                if (banks_[b].copyPending) { copyBank_ = b; break; }
        if (copyBank_ < 0) return;
        Bank& k = banks_[copyBank_];
        if (!k.copyPending) { copyBank_ = -1; return; }
        if (k.copyBuf < 0)
        {
            k.copyBuf = freeBuffer();
            if (k.copyBuf < 0) return;   // wait for a crossfade to release a buffer
        }
        Buffer& B = bufs_[k.copyBuf];
        const int64_t n = (k.copyLen - k.copyDone) < kCopyChunk ? (k.copyLen - k.copyDone) : kCopyChunk;
        hist_.copyOut(k.copyStart + k.copyDone, n, B.l.data() + k.copyDone, B.r.data() + k.copyDone);
        k.copyDone += n;
        if (k.copyDone < k.copyLen) return;

        // Seam: blend the loop's last `seam` samples into the audio that
        // preceded the window start, so end -> start is continuous.
        const int64_t len = k.copyLen;
        const int64_t xf = seam_ < len / 4 ? seam_ : len / 4;
        for (int64_t i = 0; i < xf; ++i)
        {
            const double th = 0.5 * kPi * ((double)i + 0.5) / (double)xf;
            const float a = (float)std::cos(th), s = (float)std::sin(th);
            const int64_t j = len - xf + i;
            const int64_t pre = k.copyStart - xf + i;
            B.l[(size_t)j] = hist_.left(k.copyStart + j) * a + hist_.left(pre) * s;
            B.r[(size_t)j] = hist_.right(k.copyStart + j) * a + hist_.right(pre) * s;
        }

        if (k.valid && !k.clearing)
        {
            k.oldBuf = k.buf; k.oldLen = k.len; k.oldIdx = k.idx;
            k.swapRemain = swapLen_;
        }
        else if (k.valid)
        {
            k.presence.snap(0.0f);   // was fading out from a Clear: start the new loop from silence
        }
        k.buf = k.copyBuf;
        k.copyBuf = -1;
        k.len = len;
        k.bars = k.copyBars;
        k.idx = wrap(now_ - k.copyStart, len);
        double off = std::fmod((double)k.idx - phase0, barLen_);
        if (off < 0.0) off += barLen_;
        if (off > 0.5 * barLen_) off -= barLen_;
        k.offsetBar = off;
        k.valid = true;
        k.clearing = false;
        k.presence.setTarget(1.0f);
        k.fbLp[0].reset(); k.fbLp[1].reset();
        k.copyPending = false;
        copyBank_ = -1;
    }

    Status computeStatus(const Bank& k) const
    {
        if (k.holdRemain > 0) return k.holdStatus;
        if (k.arm == Arm::Waiting || k.copyPending) return Status::Armed;
        if (k.arm == Arm::Recording) return Status::Recording;
        if (k.valid && !k.clearing) return k.wantDub ? Status::Overdubbing : Status::Playing;
        const int n = k.nextBars;
        const int64_t len = (int64_t)std::llround(n * barLen_);
        if (len > maxLen_) return Status::TooLong;
        switch (mode_)
        {
            case CaptureMode::LastCompleteBar:
                return (dbCount_ >= n + 1 && downbeat(0) - downbeat(n) <= maxLen_ &&
                        downbeat(n) - seam_ >= oldestAvailable()) ? Status::Ready : Status::NoHistory;
            case CaptureMode::RollingBar:
                return (now_ - len - seam_ >= oldestAvailable()) ? Status::Ready : Status::NoHistory;
            case CaptureMode::NextBar:
            default:
                return Status::Ready;
        }
    }

    // configuration
    double  sr_ = 0.0, maxLoopSec_ = 0.0, extraHistSec_ = 0.0;
    int64_t maxLen_ = 0, seam_ = 0, swapLen_ = 0, holdLen_ = 0;
    float   maxDrift_ = 0.0f;
    bool    prepared_ = false;

    // timeline
    BarClock   clock_;
    StereoRing hist_;
    int64_t now_ = 0, validFrom_ = 0;
    int64_t db_[kMaxDownbeats] = {};
    int     dbHead_ = 0, dbCount_ = 0;
    double  barLen_ = 88200.0;
    CaptureMode mode_ = CaptureMode::LastCompleteBar;

    // banks
    Buffer bufs_[kNumBuffers];
    Bank   banks_[kNumBanks];
    int    copyBank_ = -1;

    Diagnostics diag_;

    // shared control
    DriftSource drift_;
    float dryMix_ = 1.0f, wetMix_ = 0.0f, driftDepth_ = 0.0f;
    bool  mixFirst_ = true;
};

} // namespace slowops::looper
