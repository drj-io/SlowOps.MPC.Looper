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
// When a PLAY/STOP tap takes effect: now, on the next bar line, or when that bank's loop wraps.
enum class PlayTiming : int { Immediate = 0, BarEnd = 1, LoopEnd = 2 };
// A bank's play state; Starting / Stopping = waiting for the PlayTiming boundary.
enum class PlayState : int { Stopped = 0, Playing = 1, Starting = 2, Stopping = 3 };

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
    bool    positionMoving = false;   // host song position advancing with the audio (flag or not)
    bool    hostRunning = false;      // the transport counts as playing (flag, or position advancing)
    bool    running = true;           // the looper is counting (master on, or a bank still playing out)
    double  bpm = 0.0, ppqPos = 0.0, barLen = 0.0;
    int     timeSigNumerator = 4, timeSigDenominator = 4;
    int     beat = 0;                 // beat within the bar, 0-based
    int64_t barsElapsed = 0;          // downbeats seen since prepare
    int64_t discontinuities = 0;      // transport jumps / starts
    int64_t captureRequests[3] = {};  // per bank
    int64_t clearRequests[3] = {};
    int64_t loopsCompleted = 0;       // captures that finished and started playing (any bank)
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
    bool  monitor = true; // false: no live input in the output (the host monitors it) and loops at full level, Mix ignored
    PlayTiming playTiming = PlayTiming::Immediate;
    bool  playAll = true;    // master PLAY/STOP. The engine follows transport edges on the same block (see
                             // Diagnostics::hostRunning); the caller then sets this to match.
    float latencyMs = 0.0f;  // loop playback runs this far ahead (negative: behind), -50..50 ms. Compensates the
                             // round trip: you play to what you hear, so your playing reaches the plugin late.
    CaptureMode  captureMode = CaptureMode::LastCompleteBar;
    PlaybackMode playbackMode = PlaybackMode::OverdubOnDemand;
    bool  metronome = false;   // click on every beat, transport playing or stopped
    float clickLevel = 0.5f;   // 0..1 (squared law; 0.5 is about -18 dBFS peak)
};

struct BankParams
{
    int   lengthBars = 1;    // 1, 2, 4 or 8; applies to the next capture
    float feedback = 1.0f;   // 0..1  share of the previous pass kept while overdubbing (1 = layers stack, nothing fades)
    float volume = 1.0f;     // 0..1  (squared law: 1 = unity)
    bool  solo = false;      // when any bank is soloed, only soloed banks are heard
    bool  mute = false;
    bool  overdub = false;   // used by OverdubOnDemand
    bool  play = true;       // false: silent and not overdubbing (20 ms fade); keeps its place, so it comes back in time
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
    static constexpr double kGateSeconds  = 0.005;   // PLAY/STOP gate (a start from the top is instant)
    static constexpr double kDubSeconds   = 0.010;   // overdub engage ramp
    static constexpr double kHoldSeconds  = 2.0;     // how long a refused capture stays on screen
    static constexpr double kMaxDriftSec  = 0.006;   // max drift read offset
    static constexpr int64_t kCopyChunk   = 131072;  // frames copied per block (memcpy)
    static constexpr int64_t kCopyMargin  = 16384;   // history kept free for in-flight copies
    static constexpr int    kMaxDownbeats = 16;
    static constexpr double kClickSeconds = 0.012;   // metronome pop length

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
            k.gate.setTime((int)std::lround(kGateSeconds * sampleRate));
            k.dub.setTime((int)std::lround(kDubSeconds * sampleRate));
        }
        drift_.prepare(sampleRate);
        // Metronome pops: a very short sine burst whose pitch drops fast (like a mouth pop
        // or a tapped bottle), 1.4 kHz -> 500 Hz on the downbeat, 1.0 kHz -> 350 Hz otherwise.
        // 0.2 ms attack, 2 ms decay, so it is over in about 12 ms.
        const int clickLen = (int)std::lround(kClickSeconds * sampleRate);
        for (int a = 0; a < 2; ++a)
        {
            const double hzStart = a ? 1400.0 : 1000.0, hzEnd = a ? 500.0 : 350.0;
            clickTable_[a].assign((size_t)clickLen, 0.0f);
            double phase = 0.0;
            for (int i = 0; i < clickLen; ++i)
            {
                const double t = i / sampleRate;
                const double hz = hzEnd + (hzStart - hzEnd) * std::exp(-t / 0.0015);
                const double env = std::exp(-t / 0.002) * std::fmin(1.0, i / (0.0002 * sampleRate));
                clickTable_[a][(size_t)i] = (float)(env * std::sin(phase) * (a ? 1.0 : 0.7));
                phase += 2.0 * kPi * hz / sampleRate;
            }
        }
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
            k.fromRec = false;
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
            k.gate.snap(k.on ? 1.0f : 0.0f);
        }
        mixFirst_ = true;
        clickPos_ = -1;
        lastClick_ = -1;
    }

    void requestCapture(int bank) { if (bank >= 0 && bank < kNumBanks) banks_[bank].captureReq.store(true, std::memory_order_release); }
    // With the transport stopped: restart the internal bar clock and every loop from the top
    // at the next process() (audio thread only). Ignored while the transport plays.
    void restartFromTop() { restartReq_ = true; }
    void requestClear(int bank)   { if (bank >= 0 && bank < kNumBanks) banks_[bank].clearReq.store(true, std::memory_order_release); }

    Status  status(int bank) const      { return banks_[bank].status; }
    PlayState playState(int bank) const
    {
        const Bank& k = banks_[bank];
        if (k.on == k.wantOn) return k.on ? PlayState::Playing : PlayState::Stopped;
        return k.wantOn ? PlayState::Starting : PlayState::Stopping;
    }
    bool    hasLoop(int bank) const     { return banks_[bank].valid && !banks_[bank].clearing; }
    int64_t loopLength(int bank) const  { return banks_[bank].valid ? banks_[bank].len : 0; }
    int     loopBars(int bank) const    { return banks_[bank].valid ? banks_[bank].bars : 0; }
    // Playback position 0..1 within the loop (0 when empty).
    float   progress(int bank) const
    {
        const Bank& k = banks_[bank];
        return (k.valid && k.len > 0) ? (float)((double)k.idx / (double)k.len) : 0.0f;
    }
    // The bank's dial: while a capture records, how far it has got (bars = the capture's
    // length); otherwise the loop's playback position. dialBars 0 = nothing to show.
    int dialBars(int bank) const
    {
        const Bank& k = banks_[bank];
        if (k.arm == Arm::Recording) return k.recBars;
        if (k.copyPending && !k.valid) return k.copyBars;
        return loopBars(bank);
    }
    float dialProgress(int bank) const
    {
        const Bank& k = banks_[bank];
        if (k.arm == Arm::Recording && k.recLen > 0) return clampv((float)((double)(now_ - k.recStart) / (double)k.recLen), 0.0f, 0.999f);
        if (k.copyPending && !k.valid) return 0.0f;
        return progress(bank);
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

        // "Playing" = the host's flag OR its song position advancing in step with the
        // audio (some hosts leave the flag unset). Two consecutive advancing blocks count.
        TransportInfo tp = transport;
        {
            bool advancing = false;
            if (transport.ppqValid && havePrevPpq_ && std::isfinite(transport.ppqPos))
            {
                const double bpm = std::isfinite(transport.bpm) ? clampv(transport.bpm, 20.0, 400.0) : 120.0;
                const double expected = prevPpq_ + prevFrames_ * bpm / 60.0 / sr_;
                const double tol = std::fmax(2.0, 0.005 * sr_) * bpm / 60.0 / sr_;
                advancing = transport.ppqPos > prevPpq_ && std::fabs(transport.ppqPos - expected) <= tol;
            }
            advanceRun_ = advancing ? advanceRun_ + 1 : 0;
            // A playing flag over a song position that does not move is a stopped host
            // (the MPC may keep the flag set while stopped): two still blocks count.
            const bool still = transport.ppqValid && havePrevPpq_ && std::isfinite(transport.ppqPos) && transport.ppqPos == prevPpq_;
            stillRun_ = still && !(transport.playing && !prevFlag_) ? stillRun_ + 1 : 0;   // a fresh Play flag is believed
            prevFlag_ = transport.playing;
            havePrevPpq_ = transport.ppqValid && std::isfinite(transport.ppqPos);
            prevPpq_ = transport.ppqPos;
            prevFrames_ = numFrames;
            tp.playing = (transport.playing && stillRun_ < 2) || advanceRun_ >= 2;
            diag_.positionMoving = advanceRun_ >= 2;
            diag_.hostRunning = tp.playing;
        }
        // Master PLAY/STOP follows the transport from the very block it starts or stops,
        // until the caller's playAll catches up (it reads hostRunning after this block).
        bool startNow = false, stopNow = false;
        timing_ = g.playTiming;
        if (tp.playing != wasRunning_)
        {
            wasRunning_ = tp.playing;
            masterForced_ = tp.playing ? 1 : 0;
            startNow = tp.playing;
            stopNow = !tp.playing;
        }
        if (masterForced_ >= 0 && (masterForced_ == 1) == g.playAll) masterForced_ = -1;
        const bool master = masterForced_ >= 0 ? masterForced_ == 1 : g.playAll;
        // Master STOP also stops the count: a capture waiting for its "1" or recording is
        // cancelled. The metronome and the beat count run while the master is on, or while a
        // bank is still playing out to its Play Timing boundary.
        if (masterWas_ && !master)
            for (int b = 0; b < kNumBanks; ++b)
                if (banks_[b].arm != Arm::None) { banks_[b].arm = Arm::None; refreshGate(banks_[b]); noteResult(b, CaptureResult::Cancelled); }
        masterWas_ = master;
        bool running = master;
        for (const Bank& k : banks_) running = running || (k.valid && !k.clearing && k.on);
        diag_.running = running;
        if (restartReq_)
        {
            // From the top: the internal clock starts bar 1 now (a discontinuity, so an armed
            // Next Bar capture starts recording) and every loop goes back to its own bar 1.
            restartReq_ = false;
            if (!tp.playing)
            {
                startNow = true;
                clock_.reset();
                for (Bank& k : banks_) if (k.valid) k.idx = wrap((int64_t)std::llround(k.offsetBar), k.len);
            }
        }
        const ClockBlock cb = clock_.advance(tp, numFrames);
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
            diag_.beat = running ? clampv((int)std::floor(phaseEnd / (cb.barLen / num)), 0, num - 1) : 0;
        }

        if (cb.discontinuity)
        {
            ++diag_.discontinuities;
            dbCount_ = 0;
            for (Bank& k : banks_)
            {
                if (k.arm == Arm::Recording) { k.arm = Arm::Waiting; refreshGate(k); }   // the old loop plays again until the new 1
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
        const float dryT = !g.monitor ? 1.0f : g.mix >= 1.0f ? 0.0f : std::cos(mixA);   // Monitor Off: live gated by monT below
        const float wetT = !g.monitor ? 1.0f : g.mix <= 0.0f ? 0.0f : std::sin(mixA);
        const float monT = g.monitor ? 1.0f : 0.0f;
        const float drT = clampv(g.drift, 0.0f, 1.0f) * maxDrift_;
        const float latT = (float)(clampv((double)g.latencyMs, -50.0, 50.0) * 0.001 * sr_);
        if (mixFirst_) { dryMix_ = dryT; wetMix_ = wetT; driftDepth_ = drT; monMix_ = monT; latency_ = latT; mixFirst_ = false; }
        const float inv = 1.0f / (float)numFrames;
        const float dDry = (dryT - dryMix_) * inv, dWet = (wetT - wetMix_) * inv, dDr = (drT - driftDepth_) * inv, dMon = (monT - monMix_) * inv;

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
            // PLAY/STOP: applied now (Immediate, transport edges, master restart, no loop to time it
            // by), else at the next bar line / loop wrap (onDownbeat, the per-sample loop).
            k.wantOn = bp[b].play && master;
            if (k.on != k.wantOn && (timing_ == PlayTiming::Immediate || startNow || stopNow || !k.valid || k.clearing))
                setOn(k, k.wantOn, startNow || !k.valid);
            else if (startNow && k.on && k.arm != Arm::Recording) k.gate.snap(1.0f);   // from the top: the downbeat is not faded in
            k.firstBlock = false;
            k.wantDub = k.valid && !k.clearing && k.on && k.wantOn &&
                (g.playbackMode == PlaybackMode::ContinuousOverdub ||
                 (g.playbackMode == PlaybackMode::OverdubOnDemand && bp[b].overdub));
            k.dub.setTarget(k.wantDub ? 1.0f : 0.0f);
        }
        drift_.renormalise();

        // Metronome: beat boundaries in this block (nearest sample, like downbeats).
        // Runs whether or not the transport plays: stopped, it follows the free-running
        // internal clock at the host tempo, so you can hear it before pressing Play.
        int beatOffsets[64]; int beatIndex[64]; int numBeats = 0;
        if (g.metronome && running)
        {
            const int num = clampv(transport.timeSigNumerator, 1, 32);
            const double beatLen = cb.barLen / num;
            const double ph = std::fmod(cb.phase0, beatLen);
            for (int k = 0; numBeats < 64; ++k)
            {
                const double off = std::floor(k * beatLen - ph + 0.5);
                if (off >= numFrames) break;
                if (off < 0.0) continue;
                const int64_t at = now_ + (int64_t)off;
                if (lastClick_ >= 0 && (double)(at - lastClick_) < beatLen * 0.5) continue;   // duplicate
                lastClick_ = at;
                beatOffsets[numBeats] = (int)off;
                beatIndex[numBeats] = (int)std::lround((cb.phase0 + off) / beatLen) % num;
                ++numBeats;
            }
        }
        const float clickGain = clampv(g.clickLevel, 0.0f, 1.0f) * clampv(g.clickLevel, 0.0f, 1.0f) * 0.5f;
        int bti = 0;
        float loopPeak = 0.0f;
        int dbi = 0;
        for (int i = 0; i < numFrames; ++i)
        {
            while (dbi < cb.numDownbeats && cb.downbeats[dbi] == i) { onDownbeat(cb.barLen); ++dbi; }

            const float xl = inL[i], xr = inR[i];   // read input before any output write
            hist_.write(now_, xl, xr);

            dryMix_ += dDry; wetMix_ += dWet; driftDepth_ += dDr; monMix_ += dMon;
            // Latency glides at up to 2 % speed so turning the knob never clicks.
            if (latency_ != latT) latency_ += clampv(latT - latency_, -kLatencyGlide, kLatencyGlide);
            const float d = driftDepth_ * drift_.next() - latency_;   // read position = idx - d

            float wl = 0.0f, wr = 0.0f, presMax = 0.0f;
            for (Bank& k : banks_)
            {
                const float pres = k.presence.next();
                const float gate = k.gate.next();
                const float gain = k.gain.next();
                const float dub = k.dub.next();
                k.fb += k.dFb;
                if (k.copyPending && k.fromRec)
                {
                    // A Next Bar recording just ended. Copying it into its buffer takes a few blocks,
                    // so play its start straight from the history meanwhile (the same audio), at full
                    // level from its first sample: the new loop's downbeat is never lost or faded in.
                    // A loop it replaces stays silent (it stopped when recording began).
                    if (pres > presMax) presMax = pres;
                    const int64_t p = wrap(now_ - k.copyStart + (int64_t)std::lround(latency_), k.copyLen);
                    const float ll = k.outLp[0].process(hist_.left(k.copyStart + p));
                    const float lr = k.outLp[1].process(hist_.right(k.copyStart + p));
                    wl += ll * gain * gate;
                    wr += lr * gain * gate;
                    continue;
                }
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
                if (k.idx == 0 && timing_ == PlayTiming::LoopEnd && k.on != k.wantOn) setOn(k, k.wantOn, true);

                const float gp = gain * pres * gate;
                wl += ll * gp;
                wr += lr * gp;
            }

            const float aw = std::fabs(wl) > std::fabs(wr) ? std::fabs(wl) : std::fabs(wr);
            if (aw > loopPeak) loopPeak = aw;
            const float dryG = (1.0f - presMax * (1.0f - dryMix_)) * monMix_;
            float click = 0.0f;
            if (bti < numBeats && beatOffsets[bti] == i) { clickPos_ = 0; clickAccent_ = beatIndex[bti] == 0 ? 1 : 0; ++bti; }
            if (clickPos_ >= 0)
            {
                click = clickTable_[clickAccent_][(size_t)clickPos_] * clickGain;
                if (++clickPos_ >= (int)clickTable_[clickAccent_].size()) clickPos_ = -1;
            }
            outL[i] = xl * dryG + wl * wetMix_ + click;
            outR[i] = xr * dryG + wr * wetMix_ + click;

            ++now_;
            for (Bank& k : banks_)
                if (k.arm == Arm::Recording && now_ >= k.recStart + k.recLen)
                {
                    k.arm = Arm::None;
                    queueCopy(k, k.recStart, k.recLen, k.recBars);
                    k.fromRec = true;                       // plays from history until the copy is done
                    if (k.on) k.gate.snap(1.0f);            // the new loop's first sample at full level
                    k.presence.setTarget(1.0f);
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
        bool fromRec = false;   // the pending copy is a Next Bar recording that just ended: play it from history meanwhile
        int copyBuf = -1, copyBars = 1;
        int64_t copyStart = 0, copyLen = 0, copyDone = 0;
        // status
        Status status = Status::NoHistory, holdStatus = Status::NoHistory;
        int64_t holdRemain = 0;
        // control
        std::atomic<bool> captureReq{false}, clearReq{false};
        Ramp presence, gain, dub, gate;
        bool on = true, wantOn = true;   // PLAY/STOP: in effect / asked for
        OnePole fbLp[2], outLp[2];
        float fb = 0.7f, dFb = 0.0f;
        bool wantDub = false, firstBlock = true;
    };

    static int clampBars(int n) { return n <= 1 ? 1 : (n <= 2 ? 2 : (n <= 4 ? 4 : 8)); }

    void readLoop(const Buffer& B, int64_t len, int64_t idx, float d, float& outLeft, float& outRight) const
    {
        if (d == 0.0f) { outLeft = B.l[(size_t)idx]; outRight = B.r[(size_t)idx]; return; }
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

    // PLAY/STOP takes effect. `crisp`: a start lands on a boundary (or from the top), so it
    // comes in at full level on its first sample; otherwise a 5 ms gate fade.
    static void setOn(Bank& k, bool on, bool crisp)
    {
        k.on = on;
        if (on && crisp && k.arm != Arm::Recording) k.gate.snap(1.0f); else refreshGate(k);
    }
    // The gate is open when the bank plays, except while a Next Bar capture records into it:
    // the loop being replaced plays through the count-in and stops when recording begins.
    static void refreshGate(Bank& k) { k.gate.setTarget(k.on && k.arm != Arm::Recording ? 1.0f : 0.0f); }

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
            if (timing_ == PlayTiming::BarEnd && k.on != k.wantOn) setOn(k, k.wantOn, true);
            if (k.arm == Arm::Waiting)
            {
                const int64_t len = (int64_t)std::llround(k.recBars * barLen);
                if (len > maxLen_) { k.arm = Arm::None; refuse(k, Status::TooLong); }
                else { k.arm = Arm::Recording; k.recStart = now_; k.recLen = len; refreshGate(k); }   // a loop it replaces stops now
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
        refreshGate(k);
        k.holdRemain = 0;
        if (k.copyPending) { k.copyPending = false; k.fromRec = false; k.copyBuf = -1; if (copyBank_ == b) copyBank_ = -1; }
        if (k.valid) { k.clearing = true; k.presence.setTarget(0.0f); }
    }

    void doCapture(int b, CaptureMode mode)
    {
        Bank& k = banks_[b];
        if (k.arm != Arm::None) { k.arm = Arm::None; refreshGate(k); noteResult(b, CaptureResult::Cancelled); return; }   // second press cancels Next Bar
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
        k.fromRec = false;
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

        if (k.fromRec)
        {
            // Already playing from history (see the per-sample loop): hand over in place. The
            // replaced loop went silent when recording began, so its buffer is simply released.
            k.swapRemain = 0;
        }
        else if (k.valid && !k.clearing)
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
        k.fromRec = false;
        copyBank_ = -1;
        ++diag_.loopsCompleted;
    }

    Status computeStatus(const Bank& k) const
    {
        if (k.holdRemain > 0) return k.holdStatus;
        if (k.copyPending && k.fromRec) return Status::Playing;   // the new loop already plays from history
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

    // transport detection
    bool    havePrevPpq_ = false;
    double  prevPpq_ = 0.0;
    int     prevFrames_ = 0, advanceRun_ = 0, stillRun_ = 0;
    bool    prevFlag_ = false;

    // metronome (added to the output only; never recorded into history or loops)
    std::vector<float> clickTable_[2];
    int     clickPos_ = -1, clickAccent_ = 0;
    int64_t lastClick_ = -1;

    // shared control
    DriftSource drift_;
    float dryMix_ = 1.0f, wetMix_ = 0.0f, driftDepth_ = 0.0f, monMix_ = 1.0f;
    bool  restartReq_ = false, wasRunning_ = false;
    int   masterForced_ = -1;
    bool  masterWas_ = true;
    PlayTiming timing_ = PlayTiming::Immediate;   // -1: follow playAll; 0/1: transport edge, until playAll matches
    float latency_ = 0.0f;   // samples the loop read runs ahead of idx (overdub still writes at idx)
    static constexpr float kLatencyGlide = 0.02f;
    bool  mixFirst_ = true;
};

} // namespace slowops::looper
