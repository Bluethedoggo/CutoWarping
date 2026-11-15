// Stand-alone test of song mode (beat tracking + warp). No JUCE needed.
// g++ -O2 -std=c++17 -I../Source song_test.cpp ../Source/WarpEngine.cpp -o song_test
#include "WarpEngine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
using namespace dogside;

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf ("  FAIL: %s\n", m); ++failures; } else std::printf ("  ok:   %s\n", m); } while (0)

// builds a ~60 s "song"; tempo(t) in bpm, returns the true beat times
static AudioData makeSong (double sr, double lead, double (*tempo)(double), std::vector<double>& beatTimes)
{
    const double dur = 60.0;
    AudioData a; a.sampleRate = sr;
    a.ch.assign (2, std::vector<float> ((size_t) (dur * sr), 0.0f));
    auto& L = a.ch[0];
    std::mt19937 rng (3); std::uniform_real_distribution<float> nd (-1.f, 1.f);

    // pad: 3 sustained sines (constant)
    for (size_t i = 0; i < L.size(); ++i)
    {
        const double t = (double) i / sr;
        L[i] += (float) (0.10 * std::sin (2*M_PI*220.0*t) + 0.07 * std::sin (2*M_PI*277.18*t) + 0.07 * std::sin (2*M_PI*329.63*t));
    }
    // beats
    double t = lead;
    while (t < dur - 0.6)
    {
        beatTimes.push_back (t);
        const size_t p = (size_t) (t * sr);
        for (size_t i = 0; i < (size_t) (0.25 * sr) && p + i < L.size(); ++i)        // kick
        {
            const double u = (double) i / sr;
            const double f = 45.0 + 110.0 * std::exp (-u * 35.0);
            L[p + i] += (float) (0.7 * std::exp (-u * 9.0) * std::sin (2*M_PI*f*u) + (u < 0.004 ? 0.3 * nd (rng) : 0.0));
        }
        const double period = 60.0 / tempo (t);
        const size_t h = (size_t) ((t + 0.5 * period) * sr);                          // off-beat hat
        for (size_t i = 0; i < (size_t) (0.05 * sr) && h + i < L.size(); ++i)
            L[h + i] += 0.25f * nd (rng) * (float) std::exp (-(double) i / sr * 90.0);
        t += period;
    }
    a.ch[1] = a.ch[0];
    return a;
}

// a "kick" onset = an onset followed by a loud peak (pad max 0.24, hats 0.25, kick >= 0.7)
static std::vector<double> kickOnsets (const std::vector<float>& x, double sr)
{
    std::vector<double> r;
    for (double p : detectTransients (x, sr, 0.5, 0, x.size()))
    {
        float pk = 0.0f;
        for (size_t i = (size_t) p; i < (size_t) p + (size_t) (0.007 * sr) && i < x.size(); ++i) pk = std::max (pk, std::abs (x[i]));
        if (pk > 0.45f) r.push_back (p);
    }
    return r;
}

static double constTempo (double) { return 128.0; }
static double driftTempo (double t) { return 128.0 * (1.0 + 0.04 * std::sin (2*M_PI*t/20.0)); }

[[maybe_unused]] static double goertzel (const std::vector<float>& x, size_t a, size_t b, double f, double sr)
{
    const double w = 2*M_PI*f/sr, c = 2*std::cos (w); double s1 = 0, s2 = 0;
    for (size_t i = a; i < b; ++i) { const double s = x[i] + c*s1 - s2; s2 = s1; s1 = s; }
    return std::sqrt (s1*s1 + s2*s2 - c*s1*s2) / (double) (b - a);
}

static void run (const char* name, double (*tempo)(double), double expectBpm, double bpmTol, double outBpm)
{
    const double sr = 44100.0;
    std::printf ("\n[%s]\n", name);
    std::vector<double> truth;
    const AudioData a = makeSong (sr, 0.31, tempo, truth);

    auto t0 = std::chrono::steady_clock::now();
    const auto aw = autoWarp (a, 0.5);
    const double msA = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("  analysis %.0f ms: bpm=%.2f loopBeats=%d markers=%zu start=%.3fs end=%.3fs\n",
                 msA, aw.bpm, aw.loopBeats, aw.markers.size(), aw.startSample / sr, aw.endSample / sr);
    CHECK (std::abs (aw.bpm - expectBpm) < bpmTol, "average tempo found");
    CHECK ((int) aw.markers.size() == aw.loopBeats - 1, "one cut per beat");

    // every detected beat must sit on a real kick (< 8 ms)
    int hit = 0;
    for (const auto& m : aw.markers)
        for (double tt : truth) if (std::abs (m.sample / sr - tt) < 0.008) { ++hit; break; }
    std::printf ("  %d / %zu beats within 8 ms of a real kick\n", hit, aw.markers.size());
    {
        double mn = 1e9, mx = -1e9, sum = 0; int cnt = 0; double worstT = 0;
        for (const auto& m : aw.markers)
        {
            double best = 1e9;
            for (double tt : truth) if (std::abs (m.sample / sr - tt) < std::abs (best)) best = m.sample / sr - tt;
            if (std::abs (best) > 0.05) continue;
            mn = std::min (mn, best); mx = std::max (mx, best); sum += best; ++cnt;
            if (std::abs (best) >= std::max (std::abs (mn), std::abs (mx))) worstT = m.beatTarget;
        }
        std::printf ("  marker error vs real kick: min %.2f ms, max %.2f ms, mean %.2f ms (worst at beat %.0f)\n",
                     mn * 1000, mx * 1000, sum / cnt * 1000, worstT);
    }
    CHECK ((double) hit >= 0.95 * (double) aw.markers.size(), "beats land on the kicks");

    WarpSettings ws; ws.loopBeats = aw.loopBeats; ws.strength = 1.0; ws.gridBeats = 0.25;
    ws.startSample = aw.startSample; ws.endSample = aw.endSample; ws.sourceRate = sr;
    const auto plan = computePlan (aw.markers, (double) a.numSamples(), ws);
    RenderResult rr;
    t0 = std::chrono::steady_clock::now();
    const bool ok = renderWarp (a, plan, sr, outBpm, StretchMode::Stretch, rr);
    const double msR = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("  render %.0f ms (%.1f s of audio)\n", msR, (double) rr.numSamples() / sr);
    CHECK (ok, "render ok");

    const double spb = 60.0 / outBpm * sr;
    CHECK (std::abs ((double) rr.numSamples() - aw.loopBeats * spb) < 3.0, "length = loopBeats at the new tempo");

    // how many kicks can the onset detector find in the UNTOUCHED source? (the warp can't do better than that)
    int srcFound = 0;
    {
        const auto so = kickOnsets (a.ch[0], sr);
        for (double tt : truth)
            for (double p : so) if (std::abs (p / sr - tt) < 0.012) { ++srcFound; break; }
    }

    // kicks in the output must be on the beat grid
    const auto on = kickOnsets (rr.ch[0], sr);
    int onGrid = 0, kicks = 0; double worst = 0.0, worstBeat = 0.0;
    for (double p : on)
    {
        const double beat = p / spb;
        const double frac = beat - std::floor (beat);
        const double d = std::min (frac, 1.0 - frac);                 // distance to nearest beat
        if (d * spb / sr < 0.012) { ++kicks; if (d * spb / sr > worst) { worst = d * spb / sr; worstBeat = beat; } ++onGrid; }
    }
    std::printf ("  %d onsets on a beat in the output (detector finds %d of %zu kicks in the source), worst %.2f ms\n",
                 onGrid, srcFound, truth.size(), worst * 1000.0);
    std::printf ("  (worst one is at beat %.2f)\n", worstBeat);
    CHECK ((double) kicks >= 0.9 * (double) srcFound * (double) aw.loopBeats / (double) truth.size(), "kicks are on the grid after warping");
    CHECK (worst < 0.008, "grid error < 8 ms");

    // The pad is a 3-note chord. Per-note phase may shift a little in any time stretcher (a mild "chorus"),
    // but the chord must never drop out or pump, so we measure the energy of the 200..450 Hz band
    // and compare its spread with the same measurement on the source itself.
    auto bandSpread = [&] (const std::vector<float>& x, size_t from, size_t to)
    {
        std::vector<float> y (x.size());
        const double a1 = std::exp (-2*M_PI*180.0/sr), a2 = std::exp (-2*M_PI*450.0/sr);
        double l1=0,l2=0,p1=0,p2=0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            l1 = a1*l1 + (1-a1)*x[i]; double h = x[i]-l1; l2 = a1*l2 + (1-a1)*h; h -= l2;   // high-pass 2 poles
            p1 = a2*p1 + (1-a2)*h;   p2 = a2*p2 + (1-a2)*p1;                                  // low-pass 2 poles
            y[i] = (float) p2;
        }
        double lo = 1e9, hi = 0.0;
        const size_t blk = (size_t) (0.5 * sr);
        for (size_t b = from; b + blk < to; b += blk)
        {
            double e = 0; for (size_t i = b; i < b + blk; ++i) e += (double) y[i]*y[i];
            const double r = std::sqrt (e / (double) blk);
            lo = std::min (lo, r); hi = std::max (hi, r);
        }
        return hi / lo;
    };
    const double floorRatio = bandSpread (a.ch[0], (size_t) (2 * sr), a.numSamples() - (size_t) (2 * sr));
    const double outRatio   = bandSpread (rr.ch[0], (size_t) (2 * sr), rr.numSamples() - (size_t) (2 * sr));
    std::printf ("  chord energy spread (max/min over 0.5 s blocks): source %.2f, warped %.2f\n", floorRatio, outRatio);
    CHECK (outRatio < 1.25 * floorRatio, "sustained notes don't drop out or pump");

    bool finite = true; float pk = 0;
    for (float v : rr.ch[0]) { if (! std::isfinite (v)) finite = false; pk = std::max (pk, std::abs (v)); }
    CHECK (finite && pk < 1.5f, "no NaN / clipping");
}

int main()
{
    run ("constant 128 bpm, offset start, render @140", constTempo, 128.0, 0.25, 140.0);
    run ("drifting tempo (128 bpm +-4%), render @120",  driftTempo, 128.0, 2.5, 120.0);
    std::printf ("\n%s\n", failures == 0 ? "ALL SONG TESTS PASSED" : "SOME SONG TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
