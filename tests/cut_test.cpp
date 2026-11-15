// Stand-alone test of the cut-and-place engine (no JUCE needed).
// g++ -O2 -std=c++17 -I../Source cut_test.cpp ../Source/WarpEngine.cpp -o cut_test
#define main song_main
#include "song_test.cpp"
#undef main

static double slowDrift (double t) { return 128.0 * (1.0 + 0.02 * std::sin (2 * M_PI * t / 20.0)); }

static void runCut (const char* name, double (*tempo)(double), double outBpm, int cutEvery, CutSettings cs = {})
{
    const double sr = 44100.0;
    std::printf ("\n[%s]\n", name);
    std::vector<double> truth;
    const AudioData a = makeSong (sr, 0.31, tempo, truth);
    const auto aw = autoWarp (a, 0.5);

    WarpSettings ws; ws.loopBeats = aw.loopBeats; ws.strength = 1.0; ws.gridBeats = 0.25; ws.cutEvery = cutEvery;
    ws.startSample = aw.startSample; ws.endSample = aw.endSample; ws.sourceRate = sr;
    const auto plan = computePlan (aw.markers, (double) a.numSamples(), ws);
    std::printf ("  %zu cuts (every %d beat%s), %d beats, render at %.0f bpm\n", plan.points.size() - 2, cutEvery, cutEvery > 1 ? "s" : "", aw.loopBeats, outBpm);

    RenderResult rr;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = renderCut (a, plan, sr, outBpm, cs, rr);
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("  render %.0f ms\n", ms);
    CHECK (ok, "render ok");
    const double spb = 60.0 / outBpm * sr;
    CHECK (std::abs ((double) rr.numSamples() - aw.loopBeats * spb) < 3.0, "length = loopBeats at the new tempo");

    // kicks that sit on a cut must be exactly on the grid
    const auto on = kickOnsets (rr.ch[0], sr);
    double worstCut = 0, worstAll = 0; int nCut = 0;
    for (double p : on)
    {
        const double beat = std::round (p / spb);
        const double err = std::abs (p / spb - beat) * spb / sr;
        if (err > 0.025) continue;                                    // not a kick (detector noise)
        worstAll = std::max (worstAll, err);
        if (cutEvery == 1 || ((long) beat % cutEvery) == 0) { worstCut = std::max (worstCut, err); ++nCut; }
    }
    std::printf ("  %d kicks on cuts, worst %.2f ms (all kicks: worst %.2f ms)\n", nCut, worstCut * 1000, worstAll * 1000);
    CHECK (nCut >= (cutEvery == 1 ? 40 : 10), "kicks found on the cuts");
    CHECK (worstCut < 0.005, "kicks on cuts are on the grid (< 5 ms)");

    // clicks: the pad is smooth (max step ~0.009). Look right where every join is: 25 ms before each beat.
    double worstStep = 0; long where = 0;
    for (int beat = 4; beat < aw.loopBeats - 4; ++beat)
    {
        if (beat % cutEvery != 0) continue;                      // only the cuts are joins
        const long c = (long) std::llround (beat * spb);
        for (long i = c - (long) (0.025 * sr); i < c - (long) (0.001 * sr); ++i)
        {
            const double d = std::abs ((double) rr.ch[0][(size_t) i] - rr.ch[0][(size_t) i - 1]);
            if (d > worstStep) { worstStep = d; where = beat; }
        }
    }
    std::printf ("  biggest sample-to-sample step at the joins: %.4f (beat %ld; the pad alone reaches 0.009)\n", worstStep, where);
    CHECK (worstStep < 0.02, "no clicks at the joins");

    bool finite = true; float pk = 0;
    for (float v : rr.ch[0]) { if (! std::isfinite (v)) finite = false; pk = std::max (pk, std::abs (v)); }
    CHECK (finite && pk < 1.5f, "no NaN / clipping");
}

int main()
{
    runCut ("tempo drifts +-2%, cut every beat, same tempo",   slowDrift,  128.0, 1);
    runCut ("tempo drifts +-2%, cut every bar (4 beats)",      slowDrift,  128.0, 4);
    runCut ("pieces too long (project is faster: 140 bpm)",    constTempo, 140.0, 1);
    runCut ("pieces too short, gaps filled (project 120 bpm)", constTempo, 120.0, 1);
    CutSettings sil; sil.fillRepeat = false;
    runCut ("gaps left silent (project 120 bpm)",              constTempo, 120.0, 1, sil);
    std::printf ("\n%s\n", failures == 0 ? "ALL CUT TESTS PASSED" : "SOME CUT TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
