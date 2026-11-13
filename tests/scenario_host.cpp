// scenario_host.cpp - drives the real native plugin build (VST2 ABI, like the MPC)
// with a scripted transport and scripted button presses, then checks behaviour.
//
// Complements the SDK's host_sim (which cannot press buttons at chosen times or
// script tempo/bar position). Build:
//   clang++ -std=c++17 -O2 -I third_party/mpc-plugin-builder-sdk/framework \
//       tests/scenario_host.cpp -o build/scenario_host
// Run:  build/scenario_host plugin/build/native/slow-ops-looper.dylib [out_dir]
#include "mpc_vst2_abi.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// Parameter indices (plugin/src/SlowOpsLooper.cpp). Bank params: B(bank, kX), bank 0 = the bare names.
enum P { kMix = 0, kTone, kDrift, kCapMode, kPlayMode,
         kLength, kVolume, kFeedback, kSolo, kMute, kOverdub, kCapture, kClear, kProgress, kStatus };
static constexpr int kPerBank = 10, kNum = 5 + 3 * kPerBank + 7;
enum Extra { kBeat = 35, kDbgTransport, kDbgPosition, kDbgBars, kDbgTaps, kDbgLast, kDbgLevel };
static int B(int bank, int p) { return p + bank * kPerBank; }
static bool isReadOnly(int i) { return i >= kBeat || (i >= 5 && ((i - 5) % kPerBank == 8 || (i - 5) % kPerBank == 9)); }
static float choice(int i, int n) { return (float)i / (float)(n - 1); }

// ---- host side ----------------------------------------------------------------------
static VstTimeInfo g_time{};
static int   g_automateCount[64] = {};
static float g_automateValue[64] = {};
static VstIntPtr hostCallback(AEffect*, VstInt32 opcode, VstInt32 index, VstIntPtr, void* ptr, float opt)
{
    switch (opcode)
    {
        case audioMasterAutomate:   // the plugin telling the host a value changed
            if (index >= 0 && index < 64) { ++g_automateCount[index]; g_automateValue[index] = opt; }
            return 0;
        case audioMasterVersion: return 2400;
        case audioMasterGetTime: return (VstIntPtr)&g_time;
        case audioMasterGetVendorString: if (ptr) std::strcpy((char*)ptr, "MPC 3"); return 1;
        case audioMasterGetProductString: if (ptr) std::strcpy((char*)ptr, "MPC 3"); return 1;
        default: return 0;
    }
}

typedef AEffect* (*MainProc)(audioMasterCallback);
static MainProc g_main = nullptr;

struct Plug
{
    AEffect* e = nullptr;
    double sr = 44100.0;
    // transport
    double bpm = 120.0, ppq = 0.0;
    int num = 4, den = 4;
    bool playing = true;
    int64_t t = 0;
    std::function<float(int64_t, int)> input = [](int64_t, int) { return 0.0f; };
    std::vector<float> inL, inR, outL, outR;

    VstIntPtr disp(VstInt32 op, VstInt32 idx = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0.0f) { return e->dispatcher(e, op, idx, v, p, o); }

    explicit Plug(int maxBlock = 512)
    {
        e = g_main(hostCallback);
        // MPC-like load + prepare sequence (see host-contract.md).
        disp(effSetSampleRate, 0, 0, nullptr, (float)sr);
        disp(effSetBlockSize, 0, maxBlock);
        disp(effOpen);
        disp(effOpen);
        disp(effSetProgram, 0, 0);
        disp(effMainsChanged, 0, 1);
        const float p0 = e->getParameter(e, 0);   // parameter-0 nudge quirk
        e->setParameter(e, 0, p0 >= 0.5f ? 0.0f : 1.0f);
        e->setParameter(e, 0, p0);
        disp(effStartProcess);
        set(kDrift, 0.0f);
    }
    ~Plug() { disp(effStopProcess); disp(effMainsChanged, 0, 0); disp(effClose); }

    void set(int i, float v) { e->setParameter(e, i, v); }
    float get(int i) { return e->getParameter(e, i); }
    std::string text(int i) { char buf[256] = {0}; disp(effGetParamDisplay, i, 0, buf); return buf; }
    double barSamples() const { return num * 4.0 / den * 60.0 / bpm * sr; }

    void block(int n)
    {
        std::vector<float> l((size_t)n), r((size_t)n);
        for (int i = 0; i < n; ++i) { l[(size_t)i] = input(t + i, 0); r[(size_t)i] = input(t + i, 1); inL.push_back(l[(size_t)i]); inR.push_back(r[(size_t)i]); }
        std::memset(&g_time, 0, sizeof(g_time));
        g_time.sampleRate = sr; g_time.tempo = bpm; g_time.ppqPos = ppq;
        const double barQ = num * 4.0 / den;
        g_time.barStartPos = std::floor(ppq / barQ + 1e-9) * barQ;
        g_time.timeSigNumerator = num; g_time.timeSigDenominator = den;
        g_time.flags = kVstTempoValid | kVstTimeSigValid | kVstPpqPosValid | kVstBarsValid;
        if (playing) g_time.flags |= kVstTransportPlaying;
        float* io[2] = {l.data(), r.data()};
        e->processReplacing(e, io, io, n);   // in place, like the MPC
        for (int i = 0; i < n; ++i) { outL.push_back(l[(size_t)i]); outR.push_back(r[(size_t)i]); }
        if (playing) ppq += n / sr * bpm / 60.0;
        t += n;
    }
    void runUntil(int64_t end, int blk = 128) { while (t < end) block((int)std::min<int64_t>(blk, end - t)); }
    void runFor(double s, int blk = 128) { runUntil(t + (int64_t)std::llround(s * sr), blk); }
    bool runUntilStatus(const char* s, double maxSec = 1.0)
    {
        const int64_t lim = t + (int64_t)(maxSec * sr);
        while (t < lim) { if (text(kStatus) == s) return true; block(128); }
        return text(kStatus) == s;
    }
};

static std::function<float(int64_t, int)> barCoded(double bar)
{
    return [bar](int64_t t, int ch) {
        const double b = std::floor(t / bar);
        return ch == 0 ? (float)(0.1 * (std::fmod(b, 8.0) + 1.0)) : (float)((t - b * bar) / bar);
    };
}

// ---- scenarios ------------------------------------------------------------------------

static void passThrough()
{
    std::printf("[pass-through, in place, stopped and playing]\n");
    for (bool playing : {false, true})
    {
        Plug p; p.playing = playing; p.set(kMix, 1.0f);
        p.input = [](int64_t t, int ch) { return (float)(0.4 * std::sin(0.01 * t + 2 * ch)); };
        p.runFor(3.0, 96);
        double err = 0.0;
        for (size_t i = 0; i < p.outL.size(); ++i) err = std::fmax(err, std::fabs(p.outL[i] - p.inL[i]) + std::fabs(p.outR[i] - p.inR[i]));
        CHECK(err == 0.0, "playing=%d: not bit-exact pass-through (err %g)", playing, err);
    }
}

static void lastCompleteBar()
{
    std::printf("[Capture: Last Complete Bar via the plugin]\n");
    Plug p; p.set(kMix, 1.0f);
    const double bar = p.barSamples();
    p.input = barCoded(bar);
    p.runFor(0.5);
    CHECK(p.text(kStatus) == "No Hist", "status at 0.5 s = %s", p.text(kStatus).c_str());
    p.set(kCapture, 1.0f);   // touchscreen tap
    p.runFor(0.1);
    CHECK(p.text(kStatus) == "No Hist" && p.get(kCapture) == 0.0f, "early capture: status %s capture=%g", p.text(kStatus).c_str(), p.get(kCapture));
    p.runUntil((int64_t)(2.5 * bar));
    CHECK(p.text(kStatus) == "Ready", "status mid bar 3 = %s", p.text(kStatus).c_str());
    const int before = g_automateCount[kCapture];
    const int statusBefore = g_automateCount[kStatus];
    p.set(kCapture, 1.0f);
    p.block(128);
    CHECK(p.get(kCapture) == 0.0f && p.text(kCapture) == "Off", "Capture did not self-reset (%g)", p.get(kCapture));
    CHECK(g_automateCount[kCapture] == before + 1 && g_automateValue[kCapture] == 0.0f,
          "host not told that Capture went back to Off (count %d, value %g)", g_automateCount[kCapture] - before, g_automateValue[kCapture]);
    CHECK(p.runUntilStatus("Playing"), "never Playing (status %s)", p.text(kStatus).c_str());
    CHECK(g_automateCount[kStatus] > statusBefore && std::fabs(g_automateValue[kStatus] - 4.0f / 6.0f) < 1e-6,
          "host not told about the Status change (value %g)", g_automateValue[kStatus]);
    p.runUntil((int64_t)(5.0 * bar));
    double err = 0.0;
    const int64_t seam = 441;
    for (int64_t t = (int64_t)(3.0 * bar); t < (int64_t)(5.0 * bar); ++t)
    {
        const double ph = std::fmod((double)t, bar);
        if (ph > bar - seam - 2 || ph < 2) continue;
        err = std::fmax(err, std::fabs(p.outL[(size_t)t] - 0.2f) + std::fabs(p.outR[(size_t)t] - p.inR[(size_t)t]));
    }
    CHECK(err < 1e-4, "loop is not bar 2, phase-locked to the host (err %g)", err);
}

static void rollingBar()
{
    std::printf("[Capture: Rolling Bar via the plugin]\n");
    Plug p; p.set(kMix, 1.0f); p.set(kCapMode, choice(1, 3));
    CHECK(p.text(kCapMode) == "Rolling", "mode text %s", p.text(kCapMode).c_str());
    p.input = [](int64_t t, int) { return (float)((t % 1000000) / 1000000.0); };
    const int64_t bar = std::llround(p.barSamples());
    const int64_t T = 150000 + 37;
    p.runUntil(T, 64);
    p.set(kCapture, 1.0f);
    CHECK(p.runUntilStatus("Playing"), "never Playing");
    p.runUntil(T + bar);
    double err = 0.0;
    for (int64_t t = T + 8000; t < T + bar - 460; ++t) err = std::fmax(err, std::fabs(p.outL[(size_t)t] - p.inL[(size_t)(t - bar)]));
    CHECK(err < 1e-5, "rolling loop is not [press - bar, press) (err %g)", err);
}

static void nextBar()
{
    std::printf("[Capture: Next Bar via the plugin]\n");
    Plug p; p.set(kMix, 1.0f); p.set(kCapMode, choice(2, 3));
    const double bar = p.barSamples();
    p.input = barCoded(bar);
    p.runUntil((int64_t)(1.3 * bar));
    CHECK(p.text(kStatus) == "Ready", "Next Bar ready status = %s", p.text(kStatus).c_str());
    p.set(kCapture, 1.0f);
    p.block(128);
    std::string s1 = p.text(kStatus);
    p.runUntil((int64_t)(2.0 * bar) + 2000);
    std::string s2 = p.text(kStatus);
    p.runUntil((int64_t)(3.0 * bar) + 2000);
    std::string s3 = p.text(kStatus);
    std::printf("  status: after press %s, after downbeat %s, after the bar %s\n", s1.c_str(), s2.c_str(), s3.c_str());
    CHECK(s1 == "Armed" && s2 == "Rec Bar" && s3 == "Playing", "status sequence wrong");
    p.runUntil((int64_t)(5.0 * bar));
    double err = 0.0;
    for (int64_t t = (int64_t)(4.0 * bar) + 10; t < (int64_t)(5.0 * bar) - 460; ++t) err = std::fmax(err, std::fabs(p.outL[(size_t)t] - 0.3f));
    CHECK(err < 1e-4, "Next Bar loop is not bar 3 (err %g)", err);
}

static void feedbackDecay()
{
    std::printf("[Feedback decay via the plugin (Continuous, Feedback 50 %%)]\n");
    Plug p; p.set(kMix, 1.0f); p.set(kCapMode, choice(1, 3)); p.set(kPlayMode, choice(2, 3)); p.set(kFeedback, 0.5f);
    const int64_t bar = std::llround(p.barSamples());
    int64_t burst = -1;
    p.input = [&](int64_t t, int) { return (burst >= 0 && t >= burst && t < burst + 4410) ? (float)(0.25 * std::sin(0.1425 * t)) : 0.0f; };
    p.runUntil(bar + 3000);
    p.set(kCapture, 1.0f);
    p.runUntilStatus("Overdub");
    burst = p.t + 1000;
    const int64_t p0 = burst - 500;
    std::vector<double> pk;
    for (int k = 0; k < 6; ++k)
    {
        p.runUntil(p0 + (k + 1) * bar);
        double m = 0.0;
        for (int64_t t = p0 + k * bar; t < p0 + (k + 1) * bar; ++t) m = std::fmax(m, std::fabs(p.outL[(size_t)t]));
        pk.push_back(m);
    }
    std::printf("  pass peaks:");
    for (double v : pk) std::printf(" %.4f", v);
    std::printf("\n");
    for (int k = 2; k < 6; ++k)
    {
        const double db = 20.0 * std::log10(pk[(size_t)k] / pk[(size_t)k - 1]);
        CHECK(std::fabs(db + 6.02) < 0.5, "pass %d: %.2f dB, want -6.02 dB", k, db);
    }
}

static void stateRecall()
{
    std::printf("[State recall: every writable parameter, all banks]\n");
    std::vector<std::pair<int, float>> want = {
        {kMix, 0.8f}, {kTone, 0.37f}, {kDrift, 0.0f}, {kCapMode, choice(2, 3)}, {kPlayMode, choice(2, 3)}};
    for (int b = 0; b < 3; ++b)
    {
        want.push_back({B(b, kLength), choice((b + 1) % 4, 4)});
        want.push_back({B(b, kVolume), 0.3f + 0.2f * b});
        want.push_back({B(b, kFeedback), 0.42f + 0.1f * b});
        want.push_back({B(b, kSolo), b == 1 ? 1.0f : 0.0f});
        want.push_back({B(b, kMute), b == 2 ? 1.0f : 0.0f});
        want.push_back({B(b, kOverdub), b == 0 ? 1.0f : 0.0f});
        want.push_back({B(b, kCapture), 0.0f});
        want.push_back({B(b, kClear), 0.0f});
    }
    CHECK((int)want.size() + 13 == kNum, "test does not cover every writable parameter");
    std::vector<uint8_t> chunk;
    std::vector<std::string> texts;
    {
        Plug a;
        a.input = [](int64_t t, int ch) { return (float)(0.3 * std::sin(0.02 * t + ch)); };
        a.set(kCapMode, choice(1, 3));
        a.runFor(2.5);
        a.set(kCapture, 1.0f);
        a.runUntilStatus("Playing");
        CHECK(a.text(kStatus) == "Playing", "setup: no loop captured");
        for (auto& w : want) a.set(w.first, w.second);
        a.runFor(0.05);
        void* ptr = nullptr;
        const VstIntPtr n = a.disp(effGetChunk, 0, 0, &ptr);
        CHECK(n > 0 && ptr != nullptr, "effGetChunk returned %ld", (long)n);
        chunk.assign((const uint8_t*)ptr, (const uint8_t*)ptr + n);
        for (auto& w : want) texts.push_back(a.text(w.first));
        std::printf("  saved %zu values (%zu bytes): ", want.size(), chunk.size());
        for (size_t i = 0; i < 9; ++i) std::printf("%s ", texts[i].c_str());
        std::printf("...\n");
    }
    // Project reload: a fresh instance, state restored, then audio starts.
    Plug b;
    b.disp(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data());
    b.input = [](int64_t t, int ch) { return (float)(0.3 * std::sin(0.02 * t + ch)); };
    b.runFor(0.5);
    for (size_t k = 0; k < want.size(); ++k)
    {
        const int i = want[k].first;
        CHECK(std::fabs(b.get(i) - want[k].second) < 1e-4, "param %d: %g after reload, want %g", i, b.get(i), want[k].second);
        CHECK(b.text(i) == texts[k], "param %d text: %s after reload, want %s", i, b.text(i).c_str(), texts[k].c_str());
    }
    double err = 0.0;
    for (size_t i = 0; i < b.outL.size(); ++i) err = std::fmax(err, std::fabs(b.outL[i] - b.inL[i]));
    CHECK(err == 0.0, "captured audio came back after reload (output not dry, err %g)", err);
    CHECK(b.text(kStatus) != "Playing" && b.text(kStatus) != "Overdub", "status after reload: %s", b.text(kStatus).c_str());

    // A chunk saved while Capture was On must not fire on load.
    Plug c;
    c.input = [](int64_t t, int) { return (float)(0.2 * std::sin(0.03 * t)); };
    c.set(kCapMode, choice(2, 3));
    c.runFor(1.0);
    c.set(kCapture, 1.0f);
    void* ptr = nullptr;
    const VstIntPtr n = c.disp(effGetChunk, 0, 0, &ptr);
    std::vector<uint8_t> armedChunk((const uint8_t*)ptr, (const uint8_t*)ptr + n);
    Plug d;
    d.disp(effSetChunk, 0, (VstIntPtr)armedChunk.size(), armedChunk.data());
    d.runFor(1.0);
    CHECK(d.get(kCapture) == 0.0f && d.text(kStatus) == "Ready", "state load fired Capture (status %s, capture %g)", d.text(kStatus).c_str(), d.get(kCapture));
}

static void banksScenario()
{
    std::printf("[Three banks via the plugin: 1/2/4 bars, progress dials, solo, mute, volume]\n");
    Plug p; p.set(kMix, 1.0f);
    const double bar = p.barSamples();
    float level = 0.0f;
    p.input = [&](int64_t, int) { return level; };
    // Defaults: Length 1 = 1 bar, Length 2 = 2 bars, Length 3 = 4 bars.
    CHECK(p.text(B(0, kLength)) == "1 Bar" && p.text(B(1, kLength)) == "2 Bars" && p.text(B(2, kLength)) == "4 Bars",
          "default lengths %s/%s/%s", p.text(B(0, kLength)).c_str(), p.text(B(1, kLength)).c_str(), p.text(B(2, kLength)).c_str());
    const float L[3] = {0.1f, 0.2f, 0.4f};
    for (int b = 0; b < 3; ++b)
    {
        level = L[b];
        p.runUntil(p.t + (int64_t)(5.3 * bar));
        p.set(B(b, kCapture), 1.0f);
        p.runFor(0.2);
        CHECK(p.text(B(b, kStatus)) == "Playing", "bank %d status %s", b + 1, p.text(B(b, kStatus)).c_str());
    }
    level = 0.0f;
    p.runFor(0.3);
    CHECK(std::fabs(p.outL.back() - 0.7f) < 1e-4, "3 banks summed: %g, want 0.7", p.outL.back());

    // Progress dials: move, are quantised to 16ths, and the MPC is told.
    const int before = g_automateCount[B(2, kProgress)];
    const float p0 = p.get(B(2, kProgress));
    p.runFor(1.0);
    const float p1 = p.get(B(2, kProgress));
    std::printf("  bank 3 progress %.3f -> %.3f (\"%s\"), %d host updates in 1 s\n", p0, p1, p.text(B(2, kProgress)).c_str(),
                g_automateCount[B(2, kProgress)] - before);
    CHECK(p1 != p0 && std::fabs(p1 * 64.0f - std::round(p1 * 64.0f)) < 1e-4, "bank 3 progress not moving in 1/64 steps (%g -> %g)", p0, p1);
    const int updates = g_automateCount[B(2, kProgress)] - before;
    CHECK(updates >= 5 && updates <= 26, "progress host updates per second: %d", updates);
    const std::string t3 = p.text(B(2, kProgress));
    CHECK(t3.size() == 3 && t3[1] == '/' && t3[2] == '4', "bank 3 progress text \"%s\", want n/4", t3.c_str());
    CHECK(p.text(B(0, kProgress)) == "1/1", "bank 1 progress text \"%s\"", p.text(B(0, kProgress)).c_str());

    p.set(B(1, kMute), 1.0f);  p.runFor(0.05);
    CHECK(std::fabs(p.outL.back() - 0.5f) < 1e-4, "mute bank 2: %g, want 0.5", p.outL.back());
    p.set(B(0, kSolo), 1.0f);  p.runFor(0.05);
    CHECK(std::fabs(p.outL.back() - 0.1f) < 1e-4, "solo bank 1: %g, want 0.1", p.outL.back());
    p.set(B(0, kSolo), 0.0f); p.set(B(1, kMute), 0.0f); p.set(B(2, kVolume), 0.5f); p.runFor(0.05);
    CHECK(std::fabs(p.outL.back() - 0.4f) < 1e-4, "bank 3 at 50 %%: %g, want 0.3 + 0.1", p.outL.back());

    p.set(B(1, kClear), 1.0f); p.runFor(0.1);
    CHECK(p.text(B(1, kStatus)) == "Ready" && p.text(B(1, kProgress)) == "--" && p.get(B(1, kClear)) == 0.0f,
          "clear bank 2: status %s, progress %s", p.text(B(1, kStatus)).c_str(), p.text(B(1, kProgress)).c_str());
    CHECK(p.text(B(0, kStatus)) == "Playing" && p.text(B(2, kStatus)) == "Playing", "clearing bank 2 stopped another bank");

    // Automation: every control can be automated except the read-only displays.
    int bad = 0;
    for (int i = 0; i < kNum; ++i)
        if ((p.disp(effCanBeAutomated, i) != 0) == isReadOnly(i)) ++bad;
    CHECK(bad == 0, "%d parameters have the wrong automatable flag", bad);
}

static void beatAndDebug()
{
    std::printf("[Beat boxes and DEBUG readouts]\n");
    Plug p; p.set(kMix, 1.0f);
    p.input = [](int64_t t, int) { return (float)(0.3 * std::sin(0.02 * t)); };
    // Beat: 120 BPM, a beat is 22050 samples. Sample the Beat text mid-beat for 8 beats.
    std::string seq;
    const int before = g_automateCount[kBeat];
    for (int k = 0; k < 8; ++k) { p.runUntil(22050 * k + 11025); seq += p.text(kBeat); }
    std::printf("  beats: %s, %d host updates\n", seq.c_str(), g_automateCount[kBeat] - before);
    CHECK(seq == "12341234", "beat sequence %s, want 12341234", seq.c_str());
    CHECK(g_automateCount[kBeat] - before >= 7, "Beat changes not sent to the host");
    std::printf("  transport \"%s\", position \"%s\", bars \"%s\"\n", p.text(kDbgTransport).c_str(), p.text(kDbgPosition).c_str(), p.text(kDbgBars).c_str());
    CHECK(p.text(kDbgTransport) == "Play 120.0 4/4", "transport text %s", p.text(kDbgTransport).c_str());
    CHECK(p.text(kDbgBars) == "2 j1", "bars text %s", p.text(kDbgBars).c_str());
    CHECK(p.text(kDbgLast) == "none" && p.text(kDbgLevel) == "silent", "before capture: %s / %s", p.text(kDbgLast).c_str(), p.text(kDbgLevel).c_str());
    p.runUntil(5 * 88200 / 2);
    p.set(kCapture, 1.0f);
    p.runFor(0.3);
    std::printf("  after Capture 1: taps \"%s\", last \"%s\", level \"%s\"\n", p.text(kDbgTaps).c_str(), p.text(kDbgLast).c_str(), p.text(kDbgLevel).c_str());
    CHECK(p.text(kDbgTaps) == "1 0 0", "taps text %s", p.text(kDbgTaps).c_str());
    CHECK(p.text(kDbgLast) == "B1 OK 2.0s", "last capture text %s", p.text(kDbgLast).c_str());
    const int lvl = std::atoi(p.text(kDbgLevel).c_str());   // the 0.3 sine at Mix 100 %: about -10 dB, slow release
    CHECK(lvl <= -9 && lvl >= -12, "loop level text %s", p.text(kDbgLevel).c_str());
    // Stopped transport: the beat keeps counting on the internal clock.
    p.playing = false;
    p.runFor(0.6);
    CHECK(p.text(kDbgTransport) == "Stop 120.0 4/4 int", "stopped transport text %s", p.text(kDbgTransport).c_str());
}

static void automationThreadTriggers()
{
    std::printf("[Clear and Overdub as automation writes]\n");
    Plug p; p.set(kMix, 1.0f); p.set(kCapMode, choice(1, 3));
    p.input = [](int64_t t, int) { return (float)(0.3 * std::sin(0.02 * t)); };
    p.runFor(2.5);
    p.set(kCapture, 1.0f);
    p.runUntilStatus("Playing");
    p.set(kOverdub, 1.0f);
    p.runFor(0.05);
    CHECK(p.text(kStatus) == "Overdub", "Overdub on: status %s", p.text(kStatus).c_str());
    p.set(kOverdub, 0.0f);
    p.runFor(0.05);
    CHECK(p.text(kStatus) == "Playing", "Overdub off: status %s", p.text(kStatus).c_str());
    p.set(kClear, 1.0f);
    p.runFor(0.1);
    CHECK(p.text(kStatus) == "Ready" && p.get(kClear) == 0.0f, "Clear: status %s clear=%g", p.text(kStatus).c_str(), p.get(kClear));
    const int64_t from = p.t;
    p.runFor(0.3);
    double err = 0.0;
    for (int64_t t = from; t < p.t; ++t) err = std::fmax(err, std::fabs(p.outL[(size_t)t] - p.inL[(size_t)t]));
    CHECK(err == 0.0, "not dry after Clear (err %g)", err);
}

// ---- musical render ----------------------------------------------------------------

static void writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r, int sr)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("  cannot write %s\n", path.c_str()); return; }
    const uint32_t n = (uint32_t)l.size(), data = n * 4, riff = 36 + data;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(riff); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32((uint32_t)sr); u32((uint32_t)sr * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    for (uint32_t i = 0; i < n; ++i)
    {
        int16_t a = (int16_t)std::lround(std::fmax(-1.0, std::fmin(1.0, (double)l[i])) * 32767.0);
        int16_t b = (int16_t)std::lround(std::fmax(-1.0, std::fmin(1.0, (double)r[i])) * 32767.0);
        std::fwrite(&a, 2, 1, f); std::fwrite(&b, 2, 1, f);
    }
    std::fclose(f);
}

// A pluck with a few harmonics and an exponential decay.
static float pluck(double tSec, double hz)
{
    if (tSec < 0.0 || tSec > 1.2) return 0.0f;
    const double env = std::exp(-tSec * 4.0) * std::fmin(1.0, tSec * 400.0);
    const double w = 2.0 * 3.14159265358979 * hz * tSec;
    return (float)(env * (std::sin(w) + 0.35 * std::sin(2 * w) + 0.12 * std::sin(3 * w)));
}

static void ichigoRender(const std::string& outDir)
{
    std::printf("[Musical render: motif -> capture -> overdub -> fading layers]\n");
    Plug p;
    p.bpm = 92.0;
    const double sr = p.sr, beat = 60.0 / p.bpm, bar = 4 * beat;
    // Stand-in for the Ichigo motif (A minor pentatonic, 8th notes) and a second phrase an octave up.
    const double motif[8] = {440.0, 523.25, 587.33, 659.25, 587.33, 523.25, 440.0, 392.0};
    const double phrase[6] = {880.0, 783.99, 659.25, 783.99, 1046.5, 880.0};
    auto motifAt = [&](double s) {
        float x = 0.0f;
        for (int k = 0; k < 8; ++k) x += pluck(std::fmod(s, bar) - k * beat * 0.5, motif[k]);
        return x;
    };
    auto phraseAt = [&](double s) {
        float x = 0.0f;
        for (int k = 0; k < 6; ++k) x += pluck(std::fmod(s, bar) - (0.25 + k * 0.62) * beat, phrase[k]);
        return 0.8f * x;
    };
    // Timeline (bars, 0-based): 0-1 motif plays live; Capture (Last Bar) in bar 2;
    // live motif stops; bars 4-5 a new phrase with Overdub on; Overdub stays on so
    // older layers fade by Feedback each pass; Overdub off at bar 12.
    p.input = [&](int64_t t, int ch) {
        const double s = t / sr;
        const int b = (int)(s / bar);
        float x = 0.0f;
        if (b < 3) x += motifAt(s);
        if (b == 4 || b == 5) x += phraseAt(s);
        return 0.22f * x * (ch ? 0.95f : 1.0f);
    };
    p.set(kFeedback, 0.6f);
    p.set(kMix, 0.5f);
    p.set(kDrift, 0.15f);
    p.set(kTone, 0.85f);
    std::mt19937 rng(7);
    auto runTo = [&](double bars) {
        const int64_t end = (int64_t)(bars * bar * sr);
        while (p.t < end) p.block((int)std::min<int64_t>(32 + (int)(rng() % 481), end - p.t));
    };
    runTo(2.3);
    p.set(kCapture, 1.0f);
    runTo(3.0);
    std::printf("  after capture: %s\n", p.text(kStatus).c_str());
    runTo(4.0);
    p.set(kOverdub, 1.0f);
    runTo(12.0);
    p.set(kOverdub, 0.0f);
    runTo(14.0);

    // Drums on another track (not through the looper), summed for listening.
    std::vector<float> mixL = p.outL, mixR = p.outR;
    for (size_t i = 0; i < mixL.size(); ++i)
    {
        const double s = i / sr, pb = std::fmod(s, beat), pbar = std::fmod(s, bar);
        float d = (float)(0.5 * std::exp(-pb * 18.0) * std::sin(2 * 3.14159 * 55.0 * pb * (1.0 + 2.0 * std::exp(-pb * 30.0))));
        const double sn = std::fmod(pbar - beat, 2 * beat);
        if (sn >= 0.0 && sn < 0.2) d += (float)(0.18 * std::exp(-sn * 25.0) * (((i * 1103515245u + 12345u) >> 16) % 2000 / 1000.0 - 1.0));
        mixL[i] += d; mixR[i] += d;
    }
    // Per-pass loop energy (wet only) shows the older layers fading.
    std::printf("  output RMS per bar:");
    for (int b = 0; b < 14; ++b)
    {
        double e = 0.0; int64_t a = (int64_t)(b * bar * sr), z = (int64_t)((b + 1) * bar * sr);
        for (int64_t t = a; t < z && t < (int64_t)p.outL.size(); ++t) e += p.outL[(size_t)t] * p.outL[(size_t)t];
        std::printf(" %.3f", std::sqrt(e / (double)(z - a)));
    }
    std::printf("\n");
    bool finite = true;
    for (float x : p.outL) finite = finite && std::isfinite(x);
    CHECK(finite, "render has NaN/Inf");
    writeWav(outDir + "/ichigo-sim.wav", mixL, mixR, (int)sr);
    writeWav(outDir + "/ichigo-sim-looper-only.wav", p.outL, p.outR, (int)sr);
    std::printf("  wrote %s/ichigo-sim.wav (with drums) and ichigo-sim-looper-only.wav\n", outDir.c_str());
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin.dylib|.so> [out_dir]\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 2; }
    g_main = (MainProc)dlsym(lib, "VSTPluginMain");
    if (!g_main) { std::fprintf(stderr, "VSTPluginMain not exported\n"); return 2; }
    const std::string outDir = argc > 2 ? argv[2] : ".";

    passThrough();
    lastCompleteBar();
    rollingBar();
    nextBar();
    feedbackDecay();
    stateRecall();
    automationThreadTriggers();
    banksScenario();
    beatAndDebug();
    ichigoRender(outDir);

    std::printf("\nscenario_host: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
