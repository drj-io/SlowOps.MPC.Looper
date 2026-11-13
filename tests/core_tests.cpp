// core_tests.cpp - unit tests for the host-agnostic looper engine.
// Build: clang++ -std=c++17 -O2 -I core/include tests/core_tests.cpp -o build/core_tests
#include "slowops/looper/LooperEngine.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace slowops::looper;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const char* statusName(Status s)
{
    static const char* n[] = {"No Hist", "Ready", "Armed", "Rec Bar", "Playing", "Overdub", "Too Long"};
    return n[(int)s];
}

// A scripted host: transport, input generator, full output capture.
struct Sim
{
    double sr = 44100.0, bpm = 120.0;
    int num = 4, den = 4;
    bool playing = true;
    bool reportPlaying = true;   // false: host advances its position but never sets the playing flag
    bool flagWhenStopped = false;   // true: host keeps the playing flag set while stopped (position frozen)
    double ppq = 0.0;          // host song position (advances only while playing)
    int block = 128;
    bool inPlace = false;
    GlobalParams g;
    BankParams bp[LooperEngine::kNumBanks];
    LooperEngine e;
    std::function<float(int64_t, int)> input = [](int64_t, int) { return 0.0f; };
    std::vector<float> inL, inR, outL, outR;   // whole-run recordings
    std::vector<Status> statusAt;              // per block end, indexed by sample (sparse)
    int64_t t = 0;

    explicit Sim(int blk = 128, bool inplace = false) : block(blk), inPlace(inplace)
    {
        g.drift = 0.0f; g.mix = 1.0f; g.tone = 1.0f;
        e.prepare(sr, 512);
    }
    double barSamples() const { return num * 4.0 / den * 60.0 / bpm * sr; }

    void runUntil(int64_t end)
    {
        std::vector<float> bl(4096), br(4096), ol(4096), orr(4096);
        while (t < end)
        {
            const int n = (int)std::min<int64_t>(block, end - t);
            for (int i = 0; i < n; ++i) { bl[i] = input(t + i, 0); br[i] = input(t + i, 1); }
            TransportInfo ti;
            ti.playing = (playing && reportPlaying) || flagWhenStopped; ti.ppqValid = true; ti.barsValid = true; ti.bpm = bpm;
            ti.timeSigNumerator = num; ti.timeSigDenominator = den; ti.ppqPos = ppq;
            const double barQ = num * 4.0 / den;
            ti.barStartPpq = std::floor(ppq / barQ + 1e-9) * barQ;
            for (int i = 0; i < n; ++i) { inL.push_back(bl[i]); inR.push_back(br[i]); }
            if (inPlace) { e.process(bl.data(), br.data(), bl.data(), br.data(), n, ti, g, bp); }
            else { e.process(bl.data(), br.data(), ol.data(), orr.data(), n, ti, g, bp); std::copy(ol.begin(), ol.begin() + n, bl.begin()); std::copy(orr.begin(), orr.begin() + n, br.begin()); }
            for (int i = 0; i < n; ++i) { outL.push_back(bl[i]); outR.push_back(br[i]); }
            if (playing) ppq += n / sr * bpm / 60.0;
            t += n;
        }
    }
    void runFor(double seconds) { runUntil(t + (int64_t)std::llround(seconds * sr)); }
    // Run until the engine reports `s` (or give up after `maxSeconds`).
    bool runUntilStatus(Status s, double maxSeconds = 1.0)
    {
        const int64_t limit = t + (int64_t)(maxSeconds * sr);
        while (t < limit) { if (e.status(0) == s) return true; runUntil(t + block); }
        return e.status(0) == s;
    }
};

static bool finiteAndNoDenormals(const std::vector<float>& v)
{
    for (float x : v) { if (!std::isfinite(x)) return false; if (x != 0.0f && std::fabs(x) < 1e-30f) return false; }
    return true;
}

// Left: DC level per bar (0.1, 0.2, ...); right: bar-phase ramp 0..1.
static std::function<float(int64_t, int)> barCoded(double bar)
{
    return [bar](int64_t t, int ch) {
        const double b = std::floor(t / bar);
        if (ch == 0) return (float)(0.1 * (std::fmod(b, 8.0) + 1.0));
        return (float)((t - b * bar) / bar);
    };
}

// --------------------------------------------------------------------------------------

static void testPassThroughBeforeCapture()
{
    std::printf("[pass-through before first capture, any Mix]\n");
    for (float mix : {0.0f, 0.5f, 1.0f})
    {
        Sim s; s.g.mix = mix; s.g.drift = 0.3f;
        s.input = [](int64_t t, int ch) { return (float)(0.4 * std::sin(0.01 * t + ch)); };
        s.runFor(3.0);
        double maxErr = 0.0;
        for (size_t i = 0; i < s.outL.size(); ++i) maxErr = std::fmax(maxErr, std::fabs(s.outL[i] - s.inL[i]) + std::fabs(s.outR[i] - s.inR[i]));
        CHECK(maxErr == 0.0, "mix %.1f: output differs from input by %g with no loop", mix, maxErr);
    }
}

static void testLastCompleteBar(double bpm, int num, int den)
{
    std::printf("[Last Complete Bar @ %.0f BPM %d/%d]\n", bpm, num, den);
    Sim s; s.bpm = bpm; s.num = num; s.den = den;
    const double bar = s.barSamples();
    s.input = barCoded(bar);
    s.runUntil((int64_t)(0.5 * bar));
    CHECK(s.e.status(0) == Status::NoHistory, "status before one full bar is %s, want No Hist", statusName(s.e.status(0)));
    s.runUntil((int64_t)(2.5 * bar));   // mid bar index 2
    CHECK(s.e.status(0) == Status::Ready, "status mid bar 3 is %s, want Ready", statusName(s.e.status(0)));
    s.e.requestCapture(0);
    CHECK(s.runUntilStatus(Status::Playing), "capture never reached Playing (status %s)", statusName(s.e.status(0)));
    const int64_t len = s.e.loopLength(0);
    CHECK(std::llabs(len - std::llround(bar)) <= 1, "loop length %lld, bar %.1f", (long long)len, bar);
    const int64_t seam = std::llround(LooperEngine::kSeamSeconds * s.sr);
    double maxErr = 0.0;
    for (int64_t i = 0; i < len - seam; ++i) maxErr = std::fmax(maxErr, std::fabs(s.e.loopLeft(0)[i] - 0.2f));
    CHECK(maxErr < 1e-6, "loop content is not bar 2 (DC 0.2): max err %g", maxErr);

    // Phase lock: output right channel (loop of a bar-phase ramp) should equal the live bar phase.
    s.runUntil((int64_t)(5.0 * bar));
    double phaseErr = 0.0;
    for (int64_t t = (int64_t)(3.2 * bar); t < (int64_t)(5.0 * bar); ++t)
    {
        const double ph = std::fmod((double)t, bar);
        if (ph > bar - seam - 2 || ph < 2) continue;   // seam zone
        phaseErr = std::fmax(phaseErr, std::fabs(s.outR[(size_t)t] - s.inR[(size_t)t]));
    }
    CHECK(phaseErr < 2.0 / bar + 1e-5, "loop is not aligned to host bars: max err %g", phaseErr);
}

static void testRollingBar()
{
    std::printf("[Rolling Bar]\n");
    Sim s;
    s.input = [](int64_t t, int ch) { return (float)((t % 1000000) / 1000000.0) * (ch ? -1.0f : 1.0f); };
    const int64_t bar = std::llround(s.barSamples());
    s.runUntil((int64_t)(0.4 * s.sr));
    s.g.captureMode = CaptureMode::RollingBar;
    s.e.requestCapture(0);
    s.runUntil(s.t + s.block);
    CHECK(s.e.status(0) == Status::NoHistory, "rolling capture with 0.4 s history: status %s, want No Hist", statusName(s.e.status(0)));
    CHECK(!s.e.hasLoop(0), "a loop was created without enough history");
    const int64_t T = 200000;
    s.runUntil(T);
    s.e.requestCapture(0);
    CHECK(s.runUntilStatus(Status::Playing), "rolling capture never reached Playing");
    const int64_t seam = std::llround(LooperEngine::kSeamSeconds * s.sr);
    double maxErr = 0.0;
    for (int64_t i = 0; i < bar - seam; ++i) maxErr = std::fmax(maxErr, std::fabs(s.e.loopLeft(0)[i] - s.inL[(size_t)(T - bar + i)]));
    CHECK(maxErr < 1e-6, "rolling loop != [T-bar, T): max err %g", maxErr);
    // Playback is the live input delayed by exactly one bar.
    s.runUntil(T + bar);
    double delayErr = 0.0;
    for (int64_t t = T + 8000; t < T + bar - seam - 10; ++t) delayErr = std::fmax(delayErr, std::fabs(s.outL[(size_t)t] - s.inL[(size_t)(t - bar)]));
    CHECK(delayErr < 1e-5, "rolling playback is not one bar behind: max err %g", delayErr);
}

static void testNextBar()
{
    std::printf("[Next Bar]\n");
    Sim s;
    const double bar = s.barSamples();
    s.input = barCoded(bar);
    s.g.captureMode = CaptureMode::NextBar;
    s.runUntil((int64_t)(2.0 * bar + 10000));
    s.e.requestCapture(0);
    s.runUntil(s.t + s.block);
    CHECK(s.e.status(0) == Status::Armed, "after press: %s, want Armed", statusName(s.e.status(0)));
    s.runUntil((int64_t)(3.0 * bar) - 256);
    CHECK(s.e.status(0) == Status::Armed, "just before downbeat: %s, want Armed", statusName(s.e.status(0)));
    s.runUntil((int64_t)(3.0 * bar) + 1000);
    CHECK(s.e.status(0) == Status::Recording, "after downbeat: %s, want Rec Bar", statusName(s.e.status(0)));
    s.runUntil((int64_t)(4.0 * bar) - 256);
    CHECK(s.e.status(0) == Status::Recording, "late in the bar: %s, want Rec Bar", statusName(s.e.status(0)));
    CHECK(s.runUntilStatus(Status::Playing), "Next Bar never reached Playing");
    const int64_t seam = std::llround(LooperEngine::kSeamSeconds * s.sr);
    double maxErr = 0.0;
    for (int64_t i = 0; i < s.e.loopLength(0) - seam; ++i) maxErr = std::fmax(maxErr, std::fabs(s.e.loopLeft(0)[i] - 0.4f));
    CHECK(maxErr < 1e-6, "Next Bar loop is not bar 4 (DC 0.4): max err %g", maxErr);

    // A second press while armed cancels.
    Sim c; c.g.captureMode = CaptureMode::NextBar;
    c.runUntil(10000); c.e.requestCapture(0); c.runUntil(c.t + 128); c.e.requestCapture(0); c.runUntil(c.t + 128);
    CHECK(c.e.status(0) == Status::Ready, "second press did not cancel Next Bar (status %s)", statusName(c.e.status(0)));
}

static void testNoHistoryHoldAndDry()
{
    std::printf("[No Hist refusal keeps track dry]\n");
    Sim s;
    s.input = [](int64_t t, int ch) { return (float)(0.3 * std::sin(0.02 * t + ch)); };
    s.runFor(0.5);
    s.e.requestCapture(0);
    s.runFor(0.5);
    CHECK(s.e.status(0) == Status::NoHistory, "status %s, want No Hist", statusName(s.e.status(0)));
    CHECK(!s.e.hasLoop(0), "loop created");
    double err = 0.0;
    for (size_t i = 0; i < s.outL.size(); ++i) err = std::fmax(err, std::fabs(s.outL[i] - s.inL[i]));
    CHECK(err == 0.0, "output not dry after refused capture (err %g)", err);
}

static void testTooLong()
{
    std::printf("[Too Long]\n");
    Sim s; s.bpm = 30.0; s.bp[0].lengthBars = 8;   // 8 bars of 4/4 = 64 s > 32 s loop memory
    s.runFor(0.2);
    CHECK(s.e.status(0) == Status::TooLong, "status %s, want Too Long", statusName(s.e.status(0)));
}

// Captures a silent rolling bar, injects a burst during the first pass, then measures each pass.
static std::vector<double> passPeaks(PlaybackMode mode, float fb, std::function<void(Sim&, int)> perPass = {})
{
    Sim s; s.g.playbackMode = mode; s.bp[0].feedback = fb; s.g.captureMode = CaptureMode::RollingBar;
    const int64_t bar = std::llround(s.barSamples());
    int64_t burstStart = -1;
    s.input = [&](int64_t t, int) {
        if (burstStart >= 0 && t >= burstStart && t < burstStart + 4410) return (float)(0.25 * std::sin(2.0 * kPi * 1000.0 * t / 44100.0));
        return 0.0f;
    };
    s.runUntil(bar + 2000);
    s.e.requestCapture(0);
    s.runUntilStatus(Status::Playing);
    if (mode == PlaybackMode::OverdubOnDemand) s.bp[0].overdub = true;
    burstStart = s.t + 1000;
    const int64_t p0 = burstStart - 500;   // pass boundaries are measured from here
    std::vector<double> peaks;
    for (int k = 0; k < 6; ++k)
    {
        if (perPass) perPass(s, k);
        s.runUntil(p0 + (k + 1) * bar);
        double pk = 0.0;
        for (int64_t t = p0 + k * bar; t < p0 + (k + 1) * bar; ++t) pk = std::fmax(pk, std::fabs(s.outL[(size_t)t]));
        peaks.push_back(pk);
    }
    CHECK(finiteAndNoDenormals(s.outL) && finiteAndNoDenormals(s.outR), "NaN/Inf/denormal in output");
    return peaks;
}

static void testFeedbackDecay()
{
    std::printf("[Feedback decay]\n");
    for (float fb : {0.5f, 0.7f})
    {
        auto pk = passPeaks(PlaybackMode::ContinuousOverdub, fb);
        std::printf("  Continuous fb=%.1f pass peaks:", fb);
        for (double v : pk) std::printf(" %.4f", v);
        std::printf("\n");
        CHECK(pk[0] < 1e-6, "burst audible from the loop during the pass it was recorded (%g)", pk[0]);
        for (int k = 2; k < 6; ++k)
        {
            const double db = 20.0 * std::log10(pk[(size_t)k] / pk[(size_t)k - 1]);
            const double want = 20.0 * std::log10((double)fb);
            CHECK(std::fabs(db - want) < 0.05, "fb %.1f pass %d: %.2f dB per pass, want %.2f", fb, k, db, want);
        }
    }
    // Feedback 100 %: overdubbing with nothing played keeps the loop exactly as it was.
    {
        auto pk = passPeaks(PlaybackMode::ContinuousOverdub, 1.0f);
        std::printf("  Continuous fb=1.0 pass peaks:");
        for (double v : pk) std::printf(" %.4f", v);
        std::printf("\n");
        CHECK(pk[1] > 0.2, "fb 1.0: burst not recorded (%g)", pk[1]);
        for (int k = 2; k < 6; ++k)
            CHECK(std::fabs(20.0 * std::log10(pk[(size_t)k] / pk[1])) < 0.01, "fb 1.0 pass %d: %.3f dB vs pass 1, want 0", k,
                  20.0 * std::log10(pk[(size_t)k] / pk[1]));
    }
    // The overdub limiter: linear up to the knee, smooth above it, never past full scale.
    {
        float worst = 0.0f, prev = 0.0f; bool mono = true;
        for (int i = 0; i <= 4000; ++i)
        {
            const float x = i * 0.001f, y = softClip(x);
            if (x <= kClipKnee) worst = std::fmax(worst, std::fabs(y - x));
            if (y < prev || y > 1.0f || std::fabs(softClip(-x) + y) > 0.0f) mono = false;
            prev = y;
        }
        CHECK(worst == 0.0f && mono && softClip(4.0f) > 0.99f, "softClip: off by %g below the knee, monotonic/bounded %d", worst, mono);
    }
    auto fr = passPeaks(PlaybackMode::Freeze, 0.5f);
    CHECK(fr[1] < 1e-6 && fr[5] < 1e-6, "Freeze recorded the burst (%g)", fr[5]);

    // On Demand: Overdub on for the burst pass only, then off -> layer is kept unchanged.
    auto od = passPeaks(PlaybackMode::OverdubOnDemand, 0.5f, [](Sim& s, int k) { if (k == 1) s.bp[0].overdub = false; });
    std::printf("  On Demand (overdub only first pass) peaks: %.4f %.4f %.4f %.4f\n", od[1], od[2], od[3], od[4]);
    CHECK(od[1] > 0.2 && std::fabs(od[4] - od[1]) < 1e-3, "On Demand did not hold the layer once Overdub was off");
}

static void testSeams()
{
    std::printf("[Loop seam crossfade]\n");
    Sim s; s.g.captureMode = CaptureMode::RollingBar;
    const double f = 233.0, amp = 0.5;
    s.input = [&](int64_t t, int) { return (float)(amp * std::sin(2.0 * kPi * f * t / s.sr)); };
    s.runFor(2.6);
    s.e.requestCapture(0);
    s.runUntilStatus(Status::Playing);
    const int64_t from = s.t + 2000;
    s.runFor(4.5);
    double maxStep = 0.0;
    for (int64_t t = from; t < s.t; ++t) maxStep = std::fmax(maxStep, std::fabs(s.outL[(size_t)t] - s.outL[(size_t)t - 1]));
    const double sineStep = 2.0 * kPi * f / s.sr * amp;
    std::printf("  max step %.5f, pure sine step %.5f\n", maxStep, sineStep);
    CHECK(maxStep <= 1.5 * sineStep, "click at loop seam: step %.5f > 1.5 x %.5f", maxStep, sineStep);
}

static std::vector<float> scenario(int block, bool inPlace)
{
    Sim s(block, inPlace); s.g.drift = 0.25f; s.g.tone = 0.6f; s.g.mix = 0.6f; s.g.playbackMode = PlaybackMode::Freeze;
    s.input = [](int64_t t, int ch) { return (float)(0.3 * std::sin(0.013 * t + ch) * (1.0 + 0.5 * std::sin(0.0001 * t))); };
    s.runUntil(3 * 88200 + 1000);
    s.e.requestCapture(0);
    s.runUntil(4 * 88200 + 512 * 3);   // block-aligned for every size tested
    s.g.playbackMode = PlaybackMode::ContinuousOverdub;   // overdub from a fixed time
    s.runUntil(6 * 88200);
    std::vector<float> o(s.outL.begin() + 3 * 88200 + 1000 + 8820, s.outL.end());
    o.insert(o.end(), s.outR.begin() + 3 * 88200 + 1000 + 8820, s.outR.end());
    CHECK(finiteAndNoDenormals(s.outL) && finiteAndNoDenormals(s.outR), "block %d: NaN/Inf/denormal", block);
    return o;
}

static void testBlockSizesAndInPlace()
{
    std::printf("[Block sizes 32/77/128/512 and in-place]\n");
    const auto ref = scenario(128, false);
    for (int b : {32, 77, 512})
    {
        const auto o = scenario(b, false);
        double err = 0.0;
        for (size_t i = 0; i < ref.size() && i < o.size(); ++i) err = std::fmax(err, std::fabs(ref[i] - o[i]));
        CHECK(o.size() == ref.size() && err < 1e-4, "block %d differs from 128 by %g", b, err);
    }
    const auto ip = scenario(128, true);
    double err = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) err = std::fmax(err, std::fabs(ref[i] - ip[i]));
    CHECK(err == 0.0, "in-place differs from out-of-place by %g", err);
}

static void testTransportResync()
{
    std::printf("[Transport stop / relocate]\n");
    Sim s;
    const double bar = s.barSamples();
    s.input = barCoded(bar);
    s.runUntil((int64_t)(2.5 * bar));
    s.e.requestCapture(0);
    s.runUntilStatus(Status::Playing);
    // Stop: the loop keeps playing on the internal clock.
    s.runUntil((int64_t)(3.5 * bar));
    s.playing = false;
    const int64_t stopAt = s.t;
    s.runUntil(stopAt + (int64_t)bar);
    CHECK(s.e.status(0) == Status::Playing, "loop stopped with the transport (status %s)", statusName(s.e.status(0)));
    double rms = 0.0;
    for (int64_t t = stopAt; t < s.t; ++t) rms += s.outL[(size_t)t] * s.outL[(size_t)t];
    CHECK(rms > 0.0, "silent while stopped");
    // Restart from a different position (relocate to bar 10 + 0.3): loop must realign to bar phase.
    s.ppq = 10 * 4.0 + 1.2;
    s.playing = true;
    const int64_t relocAt = s.t;
    s.input = [&, bar, relocAt](int64_t t, int ch) {
        const double ph = std::fmod((t - relocAt) + 0.3 * bar, bar);
        return ch == 0 ? 0.0f : (float)(ph / bar);
    };
    s.runUntil(relocAt + (int64_t)(1.5 * bar));
    const int64_t seam = std::llround(LooperEngine::kSeamSeconds * s.sr);
    double err = 0.0;
    for (int64_t t = relocAt + 10; t < s.t; ++t)
    {
        const double ph = std::fmod((t - relocAt) + 0.3 * bar, bar);
        if (ph > bar - seam - 2 || ph < 2) continue;
        err = std::fmax(err, std::fabs(s.outR[(size_t)t] - s.inR[(size_t)t]));
    }
    CHECK(err < 1e-3, "loop not realigned after relocate: max phase err %g", err);
}

static void testClear()
{
    std::printf("[Clear]\n");
    Sim s; s.g.mix = 0.5f;
    s.input = [](int64_t t, int ch) { return (float)(0.3 * std::sin(0.02 * t + ch)); };
    s.runUntil(3 * 88200);
    s.e.requestCapture(0);
    s.runUntilStatus(Status::Playing);
    s.runFor(0.5);
    s.e.requestClear(0);
    s.runFor(0.1);
    CHECK(!s.e.hasLoop(0) && s.e.status(0) == Status::Ready, "after Clear: status %s", statusName(s.e.status(0)));
    const int64_t from = s.t;
    s.runFor(0.5);
    double err = 0.0;
    for (int64_t t = from; t < s.t; ++t) err = std::fmax(err, std::fabs(s.outL[(size_t)t] - s.inL[(size_t)t]));
    CHECK(err == 0.0, "not dry after Clear (err %g)", err);
}

static void testRecapture()
{
    std::printf("[Re-capture swaps loops with a crossfade]\n");
    Sim s;
    const double bar = s.barSamples();
    s.input = barCoded(bar);
    s.runUntil((int64_t)(2.5 * bar));
    s.e.requestCapture(0);
    s.runUntilStatus(Status::Playing);
    s.runUntil((int64_t)(4.5 * bar));
    s.e.requestCapture(0);
    s.runUntilStatus(Status::Playing);
    s.runFor(0.1);
    const int64_t seam = std::llround(LooperEngine::kSeamSeconds * s.sr);
    double maxErr = 0.0;
    for (int64_t i = 0; i < s.e.loopLength(0) - seam; ++i) maxErr = std::fmax(maxErr, std::fabs(s.e.loopLeft(0)[i] - 0.4f));
    CHECK(maxErr < 1e-6, "second capture is not bar 4: err %g", maxErr);
}

#include "core_tests_banks.inc"

int main()
{
    testPassThroughBeforeCapture();
    testLastCompleteBar(120.0, 4, 4);
    testLastCompleteBar(93.0, 3, 4);
    testRollingBar();
    testNextBar();
    testNoHistoryHoldAndDry();
    testTooLong();
    testFeedbackDecay();
    testSeams();
    testBlockSizesAndInPlace();
    testTransportResync();
    testClear();
    testRecapture();
    testBanks();
    std::printf("\ncore_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
