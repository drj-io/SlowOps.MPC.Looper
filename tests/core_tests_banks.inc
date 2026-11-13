// core_tests_banks.inc - multi-bank tests (included by core_tests.cpp).

// Loop content check: bank `b` must equal bars [firstBar, firstBar + n) of a barCoded input
// (left channel = 0.1 * (bar % 8 + 1)), outside each bar's seam zone at the loop end.
static double barsContentError(const Sim& s, int b, double bar, int firstBar)
{
    const int64_t seam = std::llround(LooperEngine::kSeamSeconds * s.sr);
    const float* L = s.e.loopLeft(b);
    if (!L) return 1e9;
    double err = 0.0;
    const int64_t len = s.e.loopLength(b);
    for (int64_t i = 0; i < len - seam; ++i)
    {
        const double barIdx = std::floor((firstBar * bar + i) / bar + 1e-9);
        const double want = 0.1 * (std::fmod(barIdx, 8.0) + 1.0);
        const double posInBar = std::fmod(firstBar * bar + i, bar);
        if (posInBar < 2.0 || posInBar > bar - 2.0) continue;   // bar boundary rounding
        err = std::fmax(err, std::fabs(L[i] - want));
    }
    return err;
}

static void testMultiBarCaptures()
{
    for (int n : {2, 4, 8})
    {
        std::printf("[%d-bar loops: Last Bar / Rolling / Next Bar]\n", n);
        {
            Sim s; s.bp[0].lengthBars = n;
            const double bar = s.barSamples();
            s.input = barCoded(bar);
            s.runUntil((int64_t)((n + 1.5) * bar));   // one bar of lead-in gives the seam its pre-roll
            CHECK(s.e.status(0) == Status::Ready, "LCB %d: status %s mid bar %d", n, statusName(s.e.status(0)), n + 2);
            s.e.requestCapture(0);
            CHECK(s.runUntilStatus(Status::Playing), "LCB %d never Playing", n);
            CHECK(s.e.loopBars(0) == n && std::llabs(s.e.loopLength(0) - std::llround(n * bar)) <= 1, "LCB %d: %d bars, len %lld", n, s.e.loopBars(0), (long long)s.e.loopLength(0));
            CHECK(barsContentError(s, 0, bar, 1) < 1e-6, "LCB %d: loop is not bars 2..%d", n, n + 1);
            // Continues seamlessly and in time: the loop restarts where the captured window ended
            // (downbeat n+1), so one bar later (downbeat n+2) it is playing its 2nd bar (input bar 3, level 0.3).
            const int64_t next = (int64_t)std::llround((n + 2) * bar);
            s.runUntil(next + 1000);
            CHECK(std::fabs(s.outL[(size_t)next + 500] - 0.3f) < 1e-5, "LCB %d: loop not in its 2nd bar one bar after the window (%g)", n, s.outL[(size_t)next + 500]);
            CHECK(std::fabs(s.outR[(size_t)next + 500] - s.inR[(size_t)next + 500]) < 1e-4, "LCB %d: loop out of bar phase", n);
        }
        {
            Sim s; s.bp[0].lengthBars = n; s.g.captureMode = CaptureMode::RollingBar;
            s.input = [](int64_t t, int) { return (float)((t % 3000000) / 3000000.0); };
            const int64_t len = std::llround(n * s.barSamples());
            const int64_t T = len + 12345;
            s.runUntil(T);
            s.e.requestCapture(0);
            CHECK(s.runUntilStatus(Status::Playing), "Rolling %d never Playing", n);
            s.runUntil(T + len);
            double err = 0.0;
            for (int64_t t = T + 20000; t < T + len - 500; t += 7) err = std::fmax(err, std::fabs(s.outL[(size_t)t] - s.inL[(size_t)(t - len)]));
            CHECK(err < 1e-5, "Rolling %d: playback is not the input %d bars ago (err %g)", n, n, err);
        }
        {
            Sim s; s.bp[0].lengthBars = n; s.g.captureMode = CaptureMode::NextBar;
            const double bar = s.barSamples();
            s.input = barCoded(bar);
            s.runUntil((int64_t)(1.4 * bar));
            s.e.requestCapture(0);
            s.runUntil((int64_t)(2.0 * bar) - 256);
            CHECK(s.e.status(0) == Status::Armed, "Next %d: %s before downbeat", n, statusName(s.e.status(0)));
            s.runUntil((int64_t)((2.0 + n) * bar) - 256);
            CHECK(s.e.status(0) == Status::Recording, "Next %d: %s during recording", n, statusName(s.e.status(0)));
            CHECK(s.runUntilStatus(Status::Playing), "Next %d never Playing", n);
            CHECK(barsContentError(s, 0, bar, 2) < 1e-6, "Next %d: loop is not bars 3..%d", n, n + 2);
        }
    }
    std::printf("[8 bars at 50 BPM is Too Long, 4 bars fits]\n");
    Sim s; s.bpm = 50.0; s.bp[0].lengthBars = 8; s.bp[1].lengthBars = 4; s.g.captureMode = CaptureMode::NextBar;
    s.runFor(0.3);
    CHECK(s.e.status(0) == Status::TooLong && s.e.status(1) == Status::Ready, "statuses %s / %s", statusName(s.e.status(0)), statusName(s.e.status(1)));
}

static void testBanksMix()
{
    std::printf("[Three banks: independent loops, volume, solo, mute, progress]\n");
    Sim s;   // Last Bar captures, so every loop sits on the host's bar lines
    s.bp[0].lengthBars = 1; s.bp[1].lengthBars = 2; s.bp[2].lengthBars = 4;
    const double bar = s.barSamples();
    // Each bank gets a different DC level: capture bank b while the input is level L_b.
    float level = 0.0f;
    s.input = [&](int64_t, int) { return level; };
    const float L[3] = {0.1f, 0.2f, 0.4f};
    for (int b = 0; b < 3; ++b)
    {
        level = L[b];
        s.runFor(5.2 * bar / s.sr);
        s.e.requestCapture(b);
        s.runUntilStatus(Status::Playing);
        // runUntilStatus watches bank 0; wait for this bank too.
        for (int k = 0; k < 400 && s.e.status(b) != Status::Playing; ++k) s.runUntil(s.t + 128);
        CHECK(s.e.status(b) == Status::Playing, "bank %d: %s", b + 1, statusName(s.e.status(b)));
    }
    level = 0.0f;
    s.runFor(0.1);
    auto outNow = [&]() { s.runFor(0.05); return s.outL.back(); };
    CHECK(std::fabs(outNow() - 0.7f) < 1e-4, "sum of 3 banks: %g, want 0.7", s.outL.back());
    s.bp[1].mute = true;
    CHECK(std::fabs(outNow() - 0.5f) < 1e-4, "bank 2 muted: %g, want 0.5", s.outL.back());
    s.bp[2].solo = true;
    CHECK(std::fabs(outNow() - 0.4f) < 1e-4, "bank 3 soloed: %g, want 0.4", s.outL.back());
    s.bp[1].solo = true;   // a muted bank stays muted even when soloed
    CHECK(std::fabs(outNow() - 0.4f) < 1e-4, "bank 2 solo+mute: %g, want 0.4", s.outL.back());
    s.bp[1].solo = s.bp[1].mute = s.bp[2].solo = false;
    s.bp[0].volume = 0.5f;   // squared law: 0.25
    CHECK(std::fabs(outNow() - (0.025f + 0.6f)) < 1e-4, "bank 1 at 50 %%: %g, want 0.625", s.outL.back());

    // Progress: one sample before the next downbeat, the 1-bar loop is at ~100 %, the
    // 4-bar loop at the end of one of its bars.
    const int64_t nextDb = (int64_t)(std::ceil((double)s.t / bar + 1e-9) * bar);
    s.runUntil(nextDb - 1);
    const float p0 = s.e.progress(0), p2 = s.e.progress(2);
    const double q2 = p2 * 4.0;
    CHECK(p0 > 0.999f, "bank 1 progress before downbeat %g", p0);
    CHECK(std::fabs(q2 - std::round(q2)) < 1e-3, "bank 3 progress %g is not on a bar line", p2);
    s.runUntil(nextDb + 1);
    CHECK(s.e.progress(0) < 1e-3, "bank 1 progress after downbeat %g", s.e.progress(0));

    // Clear one bank: the others keep playing.
    s.bp[0].volume = 1.0f;
    s.e.requestClear(1);
    s.runFor(0.1);
    CHECK(!s.e.hasLoop(1) && s.e.hasLoop(0) && s.e.hasLoop(2), "clear bank 2 affected other banks");
    CHECK(std::fabs(outNow() - 0.5f) < 1e-4, "after clearing bank 2: %g, want 0.5", s.outL.back());
}

static void testBufferPool()
{
    std::printf("[Re-capture with all banks full uses the spare buffer, others keep playing]\n");
    Sim s; s.g.captureMode = CaptureMode::RollingBar;
    float level = 0.1f;
    s.input = [&](int64_t, int) { return level; };
    s.runFor(2.5);
    for (int b = 0; b < 3; ++b) { s.e.requestCapture(b); s.runFor(0.2); }
    CHECK(s.e.hasLoop(0) && s.e.hasLoop(1) && s.e.hasLoop(2), "not all banks captured");
    level = 0.3f;
    s.runFor(2.5);
    s.e.requestCapture(1);
    s.e.requestCapture(2);   // queued behind bank 2; waits for the swap fade to release a buffer
    s.runFor(0.5);
    level = 0.0f;
    s.runFor(0.1);
    CHECK(std::fabs(s.e.loopLeft(1)[1000] - 0.3f) < 1e-6 && std::fabs(s.e.loopLeft(2)[1000] - 0.3f) < 1e-6,
          "re-captures did not land (%g, %g)", s.e.loopLeft(1)[1000], s.e.loopLeft(2)[1000]);
    CHECK(std::fabs(s.e.loopLeft(0)[1000] - 0.1f) < 1e-6, "bank 1 was disturbed (%g)", s.e.loopLeft(0)[1000]);
    s.runFor(0.05);
    CHECK(std::fabs(s.outL.back() - 0.7f) < 1e-4, "mixed output %g, want 0.7", s.outL.back());
    CHECK(finiteAndNoDenormals(s.outL), "NaN/denormal");
}

static void testPerBankFeedback()
{
    std::printf("[Per-bank feedback]\n");
    Sim s; s.g.captureMode = CaptureMode::RollingBar; s.g.playbackMode = PlaybackMode::ContinuousOverdub;
    s.bp[0].feedback = 0.5f; s.bp[1].feedback = 0.9f; s.bp[1].mute = true;
    const int64_t bar = std::llround(s.barSamples());
    int64_t burst = -1;
    s.input = [&](int64_t t, int) { return (burst >= 0 && t >= burst && t < burst + 2000) ? 0.25f : 0.0f; };
    s.runUntil(bar + 3000);
    s.e.requestCapture(0); s.e.requestCapture(1);
    s.runFor(0.3);
    burst = s.t + 100;
    s.runUntil(burst + 4 * bar + 2000);
    // After 4 passes (3 feedback multiplications after the first write): 0.25 * fb^3.
    const double b0 = s.e.loopLeft(0)[(size_t)wrap(burst + 4 * bar + 500 - (burst - burst), bar)];
    (void)b0;
    double m0 = 0.0, m1 = 0.0;
    for (int64_t i = 0; i < bar; ++i) { m0 = std::fmax(m0, std::fabs(s.e.loopLeft(0)[i])); m1 = std::fmax(m1, std::fabs(s.e.loopLeft(1)[i])); }
    std::printf("  bank 1 (fb .5) peak %.4f, bank 2 (fb .9) peak %.4f\n", m0, m1);
    CHECK(std::fabs(m0 / m1 - std::pow(0.5 / 0.9, 4)) < 0.05 * std::pow(0.5 / 0.9, 4), "feedback ratio %g, want %g", m0 / m1, std::pow(0.5 / 0.9, 4));
}

static void testMetronome()
{
    std::printf("[Metronome]\n");
    // Off: still bit-exact pass-through.
    {
        Sim s; s.g.metronome = false;
        s.input = [](int64_t t, int) { return (float)(0.2 * std::sin(0.01 * t)); };
        s.runFor(2.0);
        double err = 0.0;
        for (size_t i = 0; i < s.outL.size(); ++i) err = std::fmax(err, std::fabs(s.outL[i] - s.inL[i]));
        CHECK(err == 0.0, "metronome off changes the signal (%g)", err);
    }
    // On, silent input, 120 BPM 4/4: a blip starts on every beat (22050 samples), louder on the 1.
    for (int blk : {32, 128, 509})
    {
        Sim s(blk); s.g.metronome = true; s.g.clickLevel = 0.5f;
        s.runFor(4.0);
        int clicks = 0, accents = 0, misplaced = 0;
        double peakAll = 0.0;
        for (size_t i = 0; i < s.outL.size(); ++i) peakAll = std::fmax(peakAll, std::fabs(s.outL[i]));
        for (int64_t b = 0; b * 22050 < (int64_t)s.outL.size(); ++b)
        {
            const int64_t at = b * 22050;
            double pk = 0.0;
            for (int64_t i = at; i < at + 1323 && i < (int64_t)s.outL.size(); ++i) pk = std::fmax(pk, std::fabs(s.outL[(size_t)i]));
            if (pk > 1e-3) ++clicks;
            if ((b % 4 == 0) == (pk > 0.1)) ++accents;   // accent ~0.11 on the 1, ~0.08 otherwise
            if (at > 0 && std::fabs(s.outL[(size_t)at - 1]) > 1e-6) ++misplaced;   // silent just before each beat
        }
        if (blk == 128) std::printf("  click peak %.3f (%.1f dBFS)\n", peakAll, 20 * std::log10(peakAll));
        CHECK(clicks == 8 && accents == 8 && misplaced == 0, "block %d: %d clicks, %d accents, %d misplaced", blk, clicks, accents, misplaced);
        CHECK(peakAll < 0.25 && peakAll > 0.05, "block %d: click peak %g out of the quiet range", blk, peakAll);
    }
    // Host that never sets the playing flag but advances its position: still clicks, and loops still lock.
    {
        Sim s; s.g.metronome = true; s.reportPlaying = false;
        s.runFor(2.0);
        int clicks = 0;
        for (int64_t b = 1; b * 22050 < (int64_t)s.outL.size(); ++b)
        {
            double pk = 0.0;
            for (int64_t i = b * 22050; i < b * 22050 + 1323 && i < (int64_t)s.outL.size(); ++i) pk = std::fmax(pk, std::fabs(s.outL[(size_t)i]));
            if (pk > 1e-3) ++clicks;
        }
        CHECK(clicks >= 3 && s.e.diagnostics().positionMoving && s.e.diagnostics().hostLocked,
              "no-flag host: %d clicks, moving %d, locked %d", clicks, s.e.diagnostics().positionMoving, s.e.diagnostics().hostLocked);
    }
    // Transport stopped: still clicks on every beat, from the free-running internal clock.
    {
        Sim s; s.g.metronome = true; s.playing = false;
        s.runFor(2.0);
        int clicks = 0;
        for (int64_t b = 0; b * 22050 < (int64_t)s.outL.size(); ++b)
        {
            double pk = 0.0;
            for (int64_t i = b * 22050; i < b * 22050 + 1323 && i < (int64_t)s.outL.size(); ++i) pk = std::fmax(pk, std::fabs(s.outL[(size_t)i]));
            if (pk > 1e-3) ++clicks;
        }
        CHECK(clicks == 4, "transport stopped: %d clicks in 2 s at 120 BPM, want 4", clicks);
    }
    // Stopped, but the host keeps the playing flag set over a frozen position: still clicks on every beat.
    {
        Sim s; s.g.metronome = true; s.playing = false; s.flagWhenStopped = true;
        s.runFor(2.0);
        int clicks = 0;
        for (int64_t b = 1; b * 22050 < (int64_t)s.outL.size(); ++b)
        {
            double pk = 0.0;
            for (int64_t i = b * 22050 - 600; i < b * 22050 + 1323 && i < (int64_t)s.outL.size(); ++i) pk = std::fmax(pk, std::fabs(s.outL[(size_t)i]));
            if (pk > 1e-3) ++clicks;
        }
        CHECK(clicks >= 3 && !s.e.diagnostics().hostLocked, "flag set, position frozen: %d clicks, locked %d", clicks, s.e.diagnostics().hostLocked);
    }
    // Not captured: a loop recorded with the metronome on contains only the input.
    {
        Sim s; s.g.metronome = true; s.g.captureMode = CaptureMode::RollingBar;
        s.input = [](int64_t, int) { return 0.0f; };
        s.runFor(2.5);
        s.e.requestCapture(0);
        s.runFor(0.2);
        double pk = 0.0;
        for (int64_t i = 0; i < s.e.loopLength(0); ++i) pk = std::fmax(pk, std::fabs(s.e.loopLeft(0)[i]));
        CHECK(pk == 0.0, "metronome leaked into a captured loop (%g)", pk);
    }
}

static void testBanks()
{
    testMetronome();
    testMultiBarCaptures();
    testBanksMix();
    testBufferPool();
    testPerBankFeedback();
}
