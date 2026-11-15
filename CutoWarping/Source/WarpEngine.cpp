// CutoWarping - Warp / Quantize engine
// Created by DogSide Productions & Bluethedoggo
#include "WarpEngine.h"

#include <algorithm>
#include <cmath>

namespace dogside
{
namespace
{
    constexpr size_t kPad = 16384;     // zero padding around every source buffer
    constexpr double kPi  = 3.14159265358979323846;
    constexpr double kCorrHighPassHz = 200.0;

    inline double clampd (double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // 4-point Catmull-Rom interpolation. `p` points at sample 0 of a padded buffer.
    inline float cubicAt (const float* p, double pos)
    {
        const long   i = (long) std::floor (pos);
        const double f = pos - (double) i;
        const double y0 = p[i - 1], y1 = p[i], y2 = p[i + 1], y3 = p[i + 2];
        const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
        const double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
        const double c = -0.5 * y0 + 0.5 * y2;
        return (float) (((a * f + b) * f + c) * f + y1);
    }

    std::vector<float> makePadded (const std::vector<float>& in)
    {
        std::vector<float> out (kPad + in.size() + kPad, 0.0f);
        std::copy (in.begin(), in.end(), out.begin() + (long) kPad);
        return out;
    }
}

//==============================================================================
// Plan (marker positions + quantize)
//==============================================================================
WarpPlan computePlan (const std::vector<Marker>& markers, double sourceLength, const WarpSettings& s)
{
    WarpPlan plan;

    const double start = clampd (s.startSample, 0.0, sourceLength);
    const double end   = (s.endSample > 0.0) ? clampd (s.endSample, 0.0, sourceLength) : sourceLength;

    if (end - start < 64.0 || s.loopBeats <= 0.0)
        return plan;

    const double span = end - start;
    plan.loopBeats = s.loopBeats;

    auto toBeat = [&] (double smp) { return (smp - start) / span * s.loopBeats; };

    // interior markers, sorted by position
    std::vector<int> idx;
    for (int i = 0; i < (int) markers.size(); ++i)
    {
        const Marker& mk = markers[(size_t) i];
        if (mk.sample <= start + 8.0 || mk.sample >= end - 8.0)
            continue;
        // "cut every N beats": detected beats that are not on a multiple of N are not used
        if (s.cutEvery > 1 && mk.beatTarget >= 0.0 && ! mk.manual
            && (std::lround (mk.beatTarget) % (long) s.cutEvery) != 0)
            continue;
        idx.push_back (i);
    }

    std::stable_sort (idx.begin(), idx.end(), [&] (int a, int b)
    {
        return markers[(size_t) a].sample < markers[(size_t) b].sample;
    });

    WarpPoint first;
    first.sample = start; first.origBeat = 0.0; first.targetBeat = 0.0;
    plan.points.push_back (first);

    const double grid = s.gridBeats > 1.0e-6 ? s.gridBeats : 0.25;

    for (int i : idx)
    {
        const Marker& m = markers[(size_t) i];
        WarpPoint p;
        p.sample = m.sample;
        p.origBeat = toBeat (m.sample);
        p.markerIndex = i;
        p.manual = m.manual;

        if (m.manual)
        {
            p.targetBeat = m.manualBeat;
        }
        else if (m.beatTarget >= 0.0)
        {
            // detected beat: strength blends from "where it was" to "exactly on its beat"
            p.targetBeat = p.origBeat + (m.beatTarget - p.origBeat) * s.strength;
        }
        else
        {
            const long slot = std::lround (p.origBeat / grid);
            double q = (double) slot * grid;
            if ((slot & 1L) != 0)
                q += s.swing * grid / 3.0;   // swing delays every second grid slot
            p.targetBeat = p.origBeat + (q - p.origBeat) * s.strength;
        }
        plan.points.push_back (p);
    }

    WarpPoint last;
    last.sample = end; last.origBeat = s.loopBeats; last.targetBeat = s.loopBeats;
    plan.points.push_back (last);

    // ---- keep the targets ordered ------------------------------------------
    const size_t n = plan.points.size();
    const double minSeg = 0.015 * (s.sourceRate > 0.0 ? s.sourceRate : 44100.0);
    const double minGap = std::min (grid * 0.25, s.loopBeats / (4.0 * (double) n));

    // gap[i] = minimum distance between point i-1 and point i
    std::vector<double> gap (n, 0.0);
    for (size_t i = 2; i < n; ++i)   // first segment may collapse (leading silence)
        gap[i] = (plan.points[i].sample - plan.points[i - 1].sample) >= minSeg ? minGap : 0.0;

    for (size_t i = 1; i + 1 < n; ++i)
        plan.points[i].targetBeat = clampd (plan.points[i].targetBeat, 0.0, s.loopBeats);

    for (size_t i = 1; i + 1 < n; ++i)
        plan.points[i].targetBeat = std::max (plan.points[i].targetBeat, plan.points[i - 1].targetBeat + gap[i]);

    for (size_t i = n - 2; i >= 1; --i)
    {
        plan.points[i].targetBeat = std::min (plan.points[i].targetBeat, plan.points[i + 1].targetBeat - gap[i + 1]);
        if (i == 1) break;
    }

    for (size_t i = 1; i < n; ++i)   // final safety net: never go backwards
        plan.points[i].targetBeat = std::max (plan.points[i].targetBeat, plan.points[i - 1].targetBeat);

    return plan;
}

double planBeatToSample (const WarpPlan& plan, double beat)
{
    if (! plan.valid())
        return 0.0;

    const auto& p = plan.points;
    if (beat <= p.front().targetBeat) return p.front().sample;
    if (beat >= p.back().targetBeat)  return p.back().sample;

    for (size_t i = 0; i + 1 < p.size(); ++i)
    {
        const double t0 = p[i].targetBeat, t1 = p[i + 1].targetBeat;
        if (beat >= t0 && beat < t1 && t1 > t0)
        {
            const double f = (beat - t0) / (t1 - t0);
            return p[i].sample + f * (p[i + 1].sample - p[i].sample);
        }
    }
    return p.back().sample;
}

//==============================================================================
// Time stretching (WSOLA, driven by a piecewise time map)
//==============================================================================
namespace
{
    struct StretchContext
    {
        int    F = 1024;       // grain length
        int    Hs = 256;       // synthesis hop
        int    tolMax = 260;   // max search offset (samples)
        int    corrLen = 512;  // correlation length
        double rate = 44100.0;
        std::vector<float> window;
    };

    // `music` = long, tonal material: longer grains keep bass and pads intact.
    StretchContext makeContext (double sampleRate, bool music)
    {
        StretchContext c;
        c.rate = sampleRate;
        int F = 256;
        const double grain = music ? 0.035 : 0.021;
        while (F < grain * sampleRate && F < 4096) F *= 2;
        c.F = F;
        c.Hs = F / 4;
        c.tolMax = std::max (16, (int) std::lround ((music ? 0.012 : 0.006) * sampleRate));
        c.corrLen = F / 2;
        c.window.resize ((size_t) F);
        for (int i = 0; i < F; ++i)
            c.window[(size_t) i] = (float) (0.5 * (1.0 - std::cos (2.0 * kPi * (double) i / (double) F)));
        return c;
    }

    //--------------------------------------------------------------------------
    // The whole loop is stretched in ONE pass through a continuous time map, so
    // the phase of sustained notes never jumps at a cut. Around every cut the
    // map runs at exactly 1:1 (a "protected zone"): the hit is copied untouched
    // and all the stretching happens in between, where the alignment search
    // is free to work.
    struct Knot { double out = 0.0, src = 0.0; };

    // One original hit that is put back untouched at its place on the grid.
    struct Hit { long O = 0, S = 0, L = 0; bool exact = false; };

    inline long hitRamp (const StretchContext& c)  { return std::max (8L, (long) std::lround (0.004 * c.rate)); }
    inline long hitLength (const StretchContext& c) { return std::max (2 * hitRamp (c) + 8, (long) std::lround (0.045 * c.rate)); }

    // Time map (one knot per cut, plus a 1:1 stretch of map right after each cut so that the
    // stretched signal is *identical* to the original under the hit) and the list of hits.
    std::vector<Knot> buildKnots (const WarpPlan& plan, double spb, double ratio, long totalOut,
                                  const StretchContext& c, std::vector<Hit>& hits)
    {
        struct P { double o, s; };
        std::vector<P> pts;
        const size_t np = plan.points.size();

        for (size_t i = 0; i < np; ++i)
        {
            const bool lastPt = (i + 1 == np);
            const double o = lastPt ? (double) totalOut
                                    : std::min ((double) totalOut, std::round (plan.points[i].targetBeat * spb));
            const double sPos = plan.points[i].sample * ratio;

            if (! pts.empty() && (o < pts.back().o + 2.0 || sPos < pts.back().s + 2.0))
            {
                if (! lastPt) continue;      // drop a degenerate interior cut
                pts.pop_back();              // the end anchor always wins
                if (! pts.empty() && (o < pts.back().o + 2.0 || sPos < pts.back().s + 2.0))
                    continue;
            }
            pts.push_back ({ o, sPos });
        }

        std::vector<Knot> k;
        hits.clear();
        if (pts.size() < 2) return k;

        const long R = hitRamp (c), Lmax = hitLength (c);

        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            const double oL = pts[i + 1].o - pts[i].o, sL = pts[i + 1].s - pts[i].s;

            Hit h;
            h.O = (long) std::lround (pts[i].o);
            h.S = (long) std::lround (pts[i].s);
            h.L = std::min (Lmax, (long) (0.8 * oL));

            k.push_back ({ pts[i].o, pts[i].s });

            if (h.L >= 2 * R + 4)
            {
                const double zone = (double) h.L + 0.5 * c.F + (double) c.Hs;
                if (oL - zone >= 2.0 * c.Hs && sL - zone >= 2.0 * c.Hs)
                {
                    k.push_back ({ pts[i].o + zone, pts[i].s + zone });
                    h.exact = true;
                }
            }
            else
                h.L = 0;                      // too short to be worth a hit

            hits.push_back (h);
        }
        k.push_back ({ pts.back().o, pts.back().s });
        return k;
    }

    // Signal used ONLY to choose grain alignment: two cascaded one-pole high-passes.
    // A loud kick or sub bass would otherwise dominate the correlation and the
    // alignment of the (quieter) sustained notes above it would be lost.
    std::vector<float> makeCorrSignal (const std::vector<float>& mono, double sr, double fc)
    {
        std::vector<float> out (mono.size());
        const double a = std::exp (-2.0 * kPi * fc / sr);
        double l1 = 0.0, l2 = 0.0;
        for (size_t i = 0; i < mono.size(); ++i)
        {
            l1 = a * l1 + (1.0 - a) * (double) mono[i];
            const double h1 = (double) mono[i] - l1;
            l2 = a * l2 + (1.0 - a) * h1;
            out[i] = (float) (h1 - l2);
        }
        return out;
    }

    // Puts the untouched original hit back at each cut (kicks / snares stay sharp and land exactly
    // on the grid). A short crossfade hides the join; the hit itself masks it.
    void overlayHits (const StretchContext& c, const std::vector<std::vector<float>>& padCh,
                      const std::vector<Hit>& hits, long totalOut, std::vector<std::vector<float>>& out)
    {
        const long R = hitRamp (c);

        for (const Hit& h : hits)
        {
            if (h.L <= 0) continue;

            for (long j = -R; j < h.L; ++j)
            {
                const long oi = h.O + j;
                if (oi < 0 || oi >= totalOut) continue;

                double w = 1.0;
                if (j < 0)             w = (double) (j + R) / (double) R;
                else if (j > h.L - R)  w = (double) (h.L - j) / (double) R;

                for (size_t ch = 0; ch < out.size(); ++ch)
                {
                    const float orig = padCh[ch][(size_t) ((long) kPad + h.S + j)];
                    out[ch][(size_t) oi] += (float) w * (orig - out[ch][(size_t) oi]);
                }
            }
        }
    }

    bool wsolaGlobal (const StretchContext& c,
                      const std::vector<std::vector<float>>& padCh,
                      const std::vector<float>& padMono,
                      const std::vector<Knot>& kn, const std::vector<Hit>& hits, long totalOut, std::vector<std::vector<float>>& out,
                      const std::atomic<bool>* cancel)
    {
        const int F = c.F, Hs = c.Hs;
        const std::vector<float> corrSig = makeCorrSignal (padMono, c.rate, kCorrHighPassHz);
        const float* mono = corrSig.data() + (long) kPad;
        const size_t nch = out.size();

        std::vector<std::vector<float>> acc (nch, std::vector<float> ((size_t) (totalOut + 2L * F), 0.0f));

        size_t seg = 0;
        auto locate = [&] (double t, double& srcPos, double& slope)
        {
            if (t < kn.front().out || t >= kn.back().out)
            {
                const Knot& e = (t < kn.front().out) ? kn.front() : kn.back();
                srcPos = e.src + (t - e.out);
                slope = 1.0;
                return;
            }
            while (seg + 2 < kn.size() && t >= kn[seg + 1].out) ++seg;
            const Knot& a = kn[seg];
            const Knot& b = kn[seg + 1];
            slope = (b.src - a.src) / (b.out - a.out);
            srcPos = a.src + (t - a.out) * slope;
        };

        long prevStart = 0;
        bool havePrev = false;
        int hops = 0;

        for (long o = -3L * Hs; o < totalOut; o += Hs)
        {
            if ((++hops & 63) == 0 && cancel != nullptr && cancel->load())
                return false;

            double srcC = 0.0, slope = 1.0;
            locate ((double) o + 0.5 * F, srcC, slope);
            const long b0 = std::lround (srcC - 0.5 * F);

            // Grains that start inside a hit are copied exactly (no search) so the stretched signal
            // equals the original there; elsewhere the search is free. Also nothing to search
            // where the map is a plain 1:1 copy.
            bool exact = false;
            {
                auto it = std::upper_bound (hits.begin(), hits.end(), o, [] (long v, const Hit& h) { return v < h.O; });
                if (it != hits.begin())
                {
                    --it;
                    exact = it->exact && o >= it->O && o < it->O + it->L;
                }
            }
            const int tol = (exact || std::abs (slope - 1.0) < 0.001) ? 0 : c.tolMax;

            // The next hit must not be heard before its time (pre-echo): a candidate whose grain
            // contains that hit earlier than `ramp` before its place on the grid is not allowed.
            const Hit* nextHit = nullptr;
            {
                const auto it2 = std::upper_bound (hits.begin(), hits.end(), o, [] (long v, const Hit& h) { return v < h.O; });
                if (it2 != hits.end()) nextHit = &*it2;
            }
            const long ramp = hitRamp (c);
            auto forbidden = [&] (long cand) -> bool
            {
                if (nextHit == nullptr) return false;
                // does this grain contain the hit, and where in the output would it be heard?
                if (cand > nextHit->S || cand + F <= nextHit->S) return false;
                const long heard   = o + nextHit->S - cand;           // with this candidate
                const long mapped  = o + nextHit->S - b0;             // with the plain time map
                const long limit   = std::min (nextHit->O, mapped) - ramp;
                return heard < limit;                                  // earlier than it should be
            };

            long best = b0;
            if (havePrev && tol > 0)
            {
                const float* ref = mono + (prevStart + Hs);
                double bestScore = -1.0e30;

                double refE = 1.0e-9;
                for (int k = 0; k < c.corrLen; k += 2) refE += (double) ref[k] * (double) ref[k];

                // normalised correlation (-1..1) minus a small penalty for straying from the
                // mapped position: periodic sounds still lock on (score ~1), noise-like sounds
                // (hats) stay where the time map puts them instead of jittering
                auto evalAt = [&] (long cand) -> double
                {
                    const float* cp = mono + cand;
                    double dot = 0.0, en = 1.0e-9;
                    for (int k = 0; k < c.corrLen; k += 2)
                    {
                        dot += (double) ref[k] * (double) cp[k];
                        en  += (double) cp[k] * (double) cp[k];
                    }
                    if (forbidden (cand)) return -1.0e30;
                    const double dev = (double) std::abs (cand - b0) / (double) tol;
                    return dot / std::sqrt (en * refE) - 0.10 * dev;
                };

                for (long dl = -tol; dl <= tol; dl += 4)
                {
                    const double sc = evalAt (b0 + dl);
                    if (sc > bestScore) { bestScore = sc; best = b0 + dl; }
                }
                const long centre = best;
                for (long dl = -3; dl <= 3; ++dl)
                {
                    const long cand = centre + dl;
                    if (cand < b0 - tol || cand > b0 + tol) continue;
                    const double sc = evalAt (cand);
                    if (sc > bestScore) { bestScore = sc; best = cand; }
                }
            }

            const long base = o + F;
            for (size_t ch = 0; ch < nch; ++ch)
            {
                const float* sp = padCh[ch].data() + (long) kPad + best;
                float* a = acc[ch].data() + base;
                for (int n = 0; n < F; ++n)
                    a[n] += c.window[(size_t) n] * sp[n];
            }

            prevStart = best;
            havePrev = true;
        }

        for (size_t ch = 0; ch < nch; ++ch)
            for (long j = 0; j < totalOut; ++j)
                out[ch][(size_t) j] = acc[ch][(size_t) (F + j)] * 0.5f;   // Hann @ 75% overlap sums to 2
        return true;
    }
}

//==============================================================================
bool renderWarp (const AudioData& src, const WarpPlan& plan, double outRate, double bpm,
                 StretchMode mode, RenderResult& result, const std::atomic<bool>* cancel)
{
    if (! plan.valid() || src.ch.empty() || src.numSamples() < 64 || outRate <= 0.0 || bpm <= 0.0)
        return false;

    const size_t nch = std::min<size_t> (2, src.ch.size());
    const double ratio = outRate / src.sampleRate;

    // ---- 1. bring the source to the output sample rate, zero padded ---------
    std::vector<std::vector<float>> padCh (nch);
    size_t n = src.numSamples();

    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        for (size_t c = 0; c < nch; ++c) padCh[c] = makePadded (src.ch[c]);
    }
    else
    {
        n = (size_t) std::llround ((double) n * ratio);
        for (size_t c = 0; c < nch; ++c)
        {
            const std::vector<float> in = makePadded (src.ch[c]);
            std::vector<float> out (kPad + n + kPad, 0.0f);
            const float* ip = in.data() + (long) kPad;
            for (size_t i = 0; i < n; ++i)
                out[kPad + i] = cubicAt (ip, (double) i / ratio);
            padCh[c] = std::move (out);
            if (cancel != nullptr && cancel->load()) return false;
        }
    }

    std::vector<float> padMono (kPad + n + kPad, 0.0f);
    for (size_t i = 0; i < padMono.size(); ++i)
    {
        float s = 0.0f;
        for (size_t c = 0; c < nch; ++c) s += padCh[c][i];
        padMono[i] = s / (float) nch;
    }

    // ---- 2. render every segment between two warp points --------------------
    const double spb = 60.0 / bpm * outRate;                 // samples per beat
    const long totalOut = std::max (1L, std::lround (plan.loopBeats * spb));

    result.ch.assign (nch, std::vector<float> ((size_t) totalOut, 0.0f));
    result.sampleRate = outRate;
    result.bpm = bpm;
    result.loopBeats = plan.loopBeats;

    const StretchContext ctx = makeContext (outRate, (double) n / outRate > 30.0);

    if (mode == StretchMode::Stretch)
    {
        std::vector<Hit> hits;
        const auto knots = buildKnots (plan, spb, ratio, totalOut, ctx, hits);
        if (knots.size() < 2)
            return false;
        if (! wsolaGlobal (ctx, padCh, padMono, knots, hits, totalOut, result.ch, cancel))
            return false;
        overlayHits (ctx, padCh, hits, totalOut, result.ch);
        return true;
    }

    // Re-Pitch: plain variable-speed playback of every segment
    for (size_t i = 0; i + 1 < plan.points.size(); ++i)
    {
        if (cancel != nullptr && cancel->load()) return false;

        const long o0 = std::lround (plan.points[i].targetBeat * spb);
        const long o1 = std::min (totalOut, std::lround (plan.points[i + 1].targetBeat * spb));
        const long outLen = o1 - o0;
        if (outLen < 2) continue;

        const double s0 = plan.points[i].sample * ratio;
        const double srcLen = plan.points[i + 1].sample * ratio - s0;
        if (srcLen < 2.0) continue;

        for (size_t c = 0; c < nch; ++c)
        {
            const float* p = padCh[c].data() + (long) kPad;
            for (long j = 0; j < outLen; ++j)
                result.ch[c][(size_t) (o0 + j)] = cubicAt (p, s0 + (double) j * srcLen / (double) outLen);
        }
    }

    return true;
}

//==============================================================================
// Cut-and-place ("CutoWarping")
//==============================================================================
bool renderCut (const AudioData& src, const WarpPlan& plan, double outRate, double bpm,
                const CutSettings& cs, RenderResult& result, const std::atomic<bool>* cancel)
{
    if (! plan.valid() || src.ch.empty() || src.numSamples() < 64 || outRate <= 0.0 || bpm <= 0.0)
        return false;

    const size_t nch = std::min<size_t> (2, src.ch.size());
    const double ratio = outRate / src.sampleRate;

    // ---- source at the output sample rate, zero padded ---------------------------
    std::vector<std::vector<float>> padCh (nch);
    size_t n = src.numSamples();
    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        for (size_t c = 0; c < nch; ++c) padCh[c] = makePadded (src.ch[c]);
    }
    else
    {
        n = (size_t) std::llround ((double) n * ratio);
        for (size_t c = 0; c < nch; ++c)
        {
            const std::vector<float> in = makePadded (src.ch[c]);
            std::vector<float> out (kPad + n + kPad, 0.0f);
            const float* ip = in.data() + (long) kPad;
            for (size_t i = 0; i < n; ++i)
                out[kPad + i] = cubicAt (ip, (double) i / ratio);
            padCh[c] = std::move (out);
            if (cancel != nullptr && cancel->load()) return false;
        }
    }
    auto sample = [&] (size_t c, long i) -> float { return padCh[c][(size_t) ((long) kPad + i)]; };

    const double spb = 60.0 / bpm * outRate;
    const long totalOut = std::max (1L, std::lround (plan.loopBeats * spb));

    result.ch.assign (nch, std::vector<float> ((size_t) totalOut, 0.0f));
    result.sampleRate = outRate;
    result.bpm = bpm;
    result.loopBeats = plan.loopBeats;

    // ---- the cut points: where every piece starts in the source and in the output ---
    const long pre = std::max (0L, (long) std::lround (cs.preRollMs * 0.001 * outRate));
    long X = std::max (2L, (long) std::lround (cs.crossfadeMs * 0.001 * outRate));
    X = std::min (X, std::max (2L, pre));                      // the join stays in the pre-roll

    struct Cut { long cs, co; };                               // source / output start of a piece
    std::vector<Cut> cuts;
    const size_t np = plan.points.size();
    for (size_t i = 0; i < np; ++i)
    {
        Cut c;
        const long S = (long) std::lround (plan.points[i].sample * ratio);
        if (i == 0)            { c.cs = S; c.co = std::lround (plan.points[i].targetBeat * spb); }
        else if (i + 1 == np)  { c.cs = S; c.co = totalOut; }
        else
        {
            c.cs = S - pre;
            c.co = (long) std::lround (plan.points[i].targetBeat * spb) - pre;

            // move the cut (source AND output together, so the hit stays on its beat) to the
            // closest zero crossing: cutting there never clicks
            long best = 0; bool found = false;
            for (long k = 0; k <= 96 && ! found; ++k)
                for (long sgn : { -1L, 1L })
                {
                    const long p = c.cs + sgn * k;
                    const float a = sample (0, p - 1), b = sample (0, p);
                    if ((a <= 0.0f && b > 0.0f) || (a >= 0.0f && b < 0.0f)) { best = sgn * k; found = true; break; }
                }
            c.cs += best; c.co += best;
        }

        if (! cuts.empty() && i + 1 < np && (c.cs < cuts.back().cs + 64 || c.co < cuts.back().co + 64))
            continue;                                          // too close to the previous cut: skip it
        if (! cuts.empty() && i + 1 == np && (c.cs < cuts.back().cs + 64 || c.co < cuts.back().co + 64))
            cuts.pop_back();                                   // the end anchor always wins
        cuts.push_back (c);
    }
    if (cuts.size() < 2) return false;

    auto addTo = [&] (size_t c, long outIndex, float v)
    {
        if (outIndex >= 0 && outIndex < totalOut) result.ch[c][(size_t) outIndex] += v;
    };

    // ---- place every piece ------------------------------------------------------
    for (size_t k = 0; k + 1 < cuts.size(); ++k)
    {
        if (cancel != nullptr && cancel->load()) return false;

        const long a = cuts[k].cs, b = cuts[k].co;
        const long len = cuts[k + 1].cs - a;                   // what the piece has
        const long room = cuts[k + 1].co - b;                  // what the grid gives it
        const bool first = (k == 0);

        // How much of the piece is used. Too long: trimmed, but it keeps playing for X samples
        // under the next piece's fade-in. Too short: it plays to its end.
        const long used = (len >= room) ? std::min (len, room + X) : len;
        const long fadeOutLen = (len >= room) ? std::max (0L, used - room) : std::min (X, len / 2);
        const long fadeOutAt  = (len >= room) ? room : len - fadeOutLen;

        for (long j = 0; j < used; ++j)
        {
            double g = 1.0;
            if (! first && j < X)                       g *= (double) j / (double) X;
            if (fadeOutLen > 0 && j >= fadeOutAt)       g *= 1.0 - (double) (j - fadeOutAt) / (double) fadeOutLen;
            for (size_t c = 0; c < nch; ++c) addTo (c, b + j, (float) g * sample (c, a + j));
        }

        // ---- gap: the piece ended before the next one starts -----------------------
        if (len < room)
        {
            const long gap = room - len;
            const long Xf = std::max (2L, std::min (X, len / 2));
            const long fillStart = len - Xf;                    // relative to b
            const long fillLen = gap + X + Xf;                  // up to the end of the next fade-in

            const long Lr = std::min (len / 2, (long) std::lround (0.015 * outRate));
            if (cs.fillRepeat && Lr >= 4 * Xf)
            {
                // copies of the last Lr samples of the piece, joined with linear crossfades:
                // each copy fades in over Xf and, if another copy follows, fades out over Xf
                const long step = Lr - Xf;
                for (long c0 = 0; c0 < fillLen; c0 += step)
                {
                    const bool nextCopy = (c0 + step < fillLen);
                    for (long j = 0; j < Lr; ++j)
                    {
                        const long t = c0 + j;
                        if (t >= fillLen) break;

                        double g = 1.0;
                        if (j < Xf)                          g *= (double) j / (double) Xf;
                        if (nextCopy && j >= Lr - Xf)        g *= (double) (Lr - j) / (double) Xf;
                        if (t >= fillLen - X)                g *= (double) (fillLen - t) / (double) X;   // out, under the next piece's fade-in

                        for (size_t c = 0; c < nch; ++c)
                            addTo (c, b + fillStart + t, (float) g * sample (c, a + len - Lr + j));
                    }
                }
            }
        }
    }
    return true;
}

//==============================================================================
// Analysis
//==============================================================================
std::vector<float> mixToMono (const AudioData& src)
{
    std::vector<float> mono (src.numSamples(), 0.0f);
    if (src.ch.empty()) return mono;
    for (const auto& c : src.ch)
        for (size_t i = 0; i < mono.size() && i < c.size(); ++i)
            mono[i] += c[i];
    const float g = 1.0f / (float) src.ch.size();
    for (auto& v : mono) v *= g;
    return mono;
}

namespace
{
    // Energy envelope (dB) of the pre-emphasised signal.
    std::vector<double> computeEnvDb (const std::vector<float>& x, size_t from, size_t to, size_t hop, size_t win)
    {
        const size_t nFrames = to > from ? (to - from) / hop : 0;
        std::vector<double> env (nFrames, -100.0);
        for (size_t f = 0; f < nFrames; ++f)
        {
            const size_t s = from + f * hop;
            const size_t e = std::min (s + win, to);
            double sum = 0.0;
            for (size_t i = std::max<size_t> (s, 1); i < e; ++i)
            {
                const double d = (double) x[i] - 0.97 * (double) x[i - 1];
                sum += d * d;
            }
            const double rms = std::sqrt (sum / (double) std::max<size_t> (1, e - s));
            env[f] = std::max (-100.0, 20.0 * std::log10 (rms + 1.0e-7));
        }
        return env;
    }
}

std::vector<double> detectTransients (const std::vector<float>& x, double sr, double sensitivity,
                                      size_t from, size_t to)
{
    std::vector<double> result;
    if (to > x.size()) to = x.size();
    if (to <= from + 2048 || sr <= 0.0) return result;

    sensitivity = clampd (sensitivity, 0.0, 1.0);

    const size_t hop = std::max<size_t> (32, (size_t) (sr * 0.004));
    const size_t win = hop * 2;
    const std::vector<double> env = computeEnvDb (x, from, to, hop, win);
    const size_t nFrames = env.size();
    if (nFrames < 8) return result;

    const double thrDb = 9.0 - 6.0 * sensitivity;                       // needed level rise (dB)
    const double floorDb = -52.0;                                       // ignore very quiet material
    const double minDist = sr * (0.030 + (1.0 - sensitivity) * 0.050);  // min distance between hits

    double lastPos = -1.0e12;
    bool armed = true;

    for (size_t f = 2; f < nFrames; ++f)
    {
        const double flux = (env[f] > floorDb) ? env[f] - env[f - 2] : 0.0;
        const double framePos = (double) (from + f * hop);

        if (flux >= thrDb && armed && framePos - lastPos >= minDist)
        {
            // ---- refine to sample accuracy ----
            const size_t a = from + (f - 1) * hop;
            const size_t b = std::min (to, a + win + 2 * hop);

            double peak = 0.0;
            for (size_t i = std::max<size_t> (a, 1); i < b; ++i)
                peak = std::max (peak, std::abs ((double) x[i] - 0.97 * (double) x[i - 1]));

            size_t pos = a;
            if (peak > 1.0e-6)
            {
                double smooth = 0.0;
                for (size_t i = std::max<size_t> (a, 8); i < b; ++i)
                {
                    smooth = 0.0;
                    for (size_t k = 0; k < 8; ++k)
                        smooth += std::abs ((double) x[i - k] - 0.97 * (double) x[i - k - 1]);
                    smooth /= 8.0;
                    if (smooth >= 0.30 * peak) { pos = i; break; }
                }
            }

            pos = pos > 24 ? pos - 24 : 0;   // small pre-roll
            if (pos < from) pos = from;

            // snap back to a zero crossing (avoids clicks when cutting)
            for (size_t k = 0; k < 96 && pos > from + 1; ++k, --pos)
                if ((x[pos] <= 0.0f && x[pos - 1] > 0.0f) || (x[pos] >= 0.0f && x[pos - 1] < 0.0f))
                    break;

            result.push_back ((double) pos);
            lastPos = (double) pos;
            armed = false;
        }
        else if (flux < thrDb * 0.4)
        {
            armed = true;
        }
    }
    return result;
}

namespace
{
    // Picks how many beats the loop lasts, by checking how well a regular beat
    // grid (anchored at the loop start) lines up with the onsets.
    int estimateLoopBeats (const std::vector<float>& x, double sr, size_t from, size_t to)
    {
        const double dur = (double) (to - from) / sr;
        const int fallback = std::max (1, (int) std::lround (dur * 2.0));   // = 120 bpm
        if (dur < 0.2) return 1;

        const size_t hop = std::max<size_t> (32, (size_t) (sr * 0.005));
        const std::vector<double> env = computeEnvDb (x, from, to, hop, hop * 2);
        const size_t nF = env.size();
        if (nF < 8) return fallback;

        std::vector<double> flux (nF, 0.0);
        double mean = 0.0;
        for (size_t f = 1; f < nF; ++f)
        {
            flux[f] = clampd (env[f] - env[f - 1], 0.0, 24.0);
            mean += flux[f];
        }
        mean /= (double) nF;

        double bestScore = -1.0;
        int bestN = fallback;
        const int maxN = 4096;

        for (int N = 1; N <= maxN; ++N)
        {
            const double bpm = (double) N * 60.0 / dur;
            if (bpm < 60.0) continue;
            if (bpm > 200.0) break;

            const double period = (double) nF / (double) N;
            double sum = 0.0;
            for (int k = 0; k < N; ++k)
            {
                const long c = std::lround ((double) k * period);
                double m = 0.0;
                for (long j = c - 1; j <= c + 2; ++j)
                    if (j >= 0 && j < (long) nF) m = std::max (m, flux[(size_t) j]);
                sum += m;
            }
            const double hit = sum / (double) N;
            const double oct = std::log2 (bpm / 120.0) / 0.6;
            const double prior = std::exp (-0.5 * oct * oct);
            const double score = hit / (mean + 1.0e-6) * prior;
            if (score > bestScore) { bestScore = score; bestN = N; }
        }
        return bestN;
    }
}

namespace
{
    //==========================================================================
    // Song mode: beat tracking
    //==========================================================================

    // Onset strength per frame. Two bands are used so that both a kick drum
    // (low band) and hats / snares / plucks (pre-emphasised full band) count.
    std::vector<double> onsetStrength (const std::vector<float>& x, double sr, size_t from, size_t to, size_t hop)
    {
        const size_t nFrames = to > from ? (to - from) / hop : 0;
        std::vector<double> eLow (nFrames, 0.0), eHi (nFrames, 0.0);

        const double a = std::exp (-2.0 * kPi * 180.0 / sr);
        double y = 0.0;
        for (size_t f = 0; f < nFrames; ++f)
        {
            double sl = 0.0, sh = 0.0;
            for (size_t i = from + f * hop, e = i + hop; i < e; ++i)
            {
                y = a * y + (1.0 - a) * (double) x[i];
                sl += y * y;
                const double d = (double) x[i] - (i > 0 ? 0.97 * (double) x[i - 1] : 0.0);
                sh += d * d;
            }
            eLow[f] = sl; eHi[f] = sh;
        }

        auto toDb = [&] (const std::vector<double>& e)
        {
            std::vector<double> db (nFrames, -100.0);
            for (size_t f = 0; f + 1 < nFrames; ++f)
                db[f] = std::max (-100.0, 10.0 * std::log10 ((e[f] + e[f + 1]) / (double) (2 * hop) + 1.0e-12));
            return db;
        };
        const auto dl = toDb (eLow), dh = toDb (eHi);

        std::vector<double> o (nFrames, 0.0);
        double sum = 0.0;
        for (size_t f = 2; f < nFrames; ++f)
        {
            const double fl = (dl[f] > -60.0) ? std::min (20.0, std::max (0.0, dl[f] - dl[f - 2])) : 0.0;
            const double fh = (dh[f] > -60.0) ? std::min (20.0, std::max (0.0, dh[f] - dh[f - 2])) : 0.0;
            o[f] = fl + fh;
            sum += o[f];
        }
        const double mean = sum / (double) std::max<size_t> (1, nFrames);
        if (mean > 1.0e-9)
            for (auto& v : o) v /= mean;
        return o;
    }

    // Finds the beat positions (in samples). Returns false if the material has no usable pulse.
    bool trackBeats (const std::vector<float>& x, double sr, std::vector<double>& beats)
    {
        const size_t n = x.size();
        const size_t hop = std::max<size_t> (32, (size_t) (sr * 0.0025));
        const double h = (double) hop / sr;

        const std::vector<double> o = onsetStrength (x, sr, 0, n, hop);
        const long nF = (long) o.size();
        if (nF < (long) (8.0 / h)) return false;

        std::vector<double> os (o.size(), 0.0);   // smoothed copy, tolerant to +-1 frame
        for (long f = 0; f < nF; ++f)
            os[(size_t) f] = std::max ({ o[(size_t) f], f > 0 ? o[(size_t) f - 1] : 0.0, f + 1 < nF ? o[(size_t) f + 1] : 0.0 });

        // ---- 1. rough tempo from the autocorrelation ------------------------------
        const long minLag = (long) std::floor (60.0 / (190.0 * h));
        const long maxLag = (long) std::ceil  (60.0 / (65.0 * h));
        double bestAc = -1.0; long bestLag = 0;
        for (long lag = minLag; lag <= maxLag; ++lag)
        {
            double ac = 0.0;
            for (long f = 0; f + lag < nF; ++f)
                ac += o[(size_t) f] * o[(size_t) (f + lag)];
            ac /= (double) (nF - lag);
            const double bpm = 60.0 / ((double) lag * h);
            const double oct = std::log2 (bpm / 125.0) / 0.75;
            const double sc = ac * std::exp (-0.5 * oct * oct);
            if (sc > bestAc) { bestAc = sc; bestLag = lag; }
        }
        if (bestLag <= 0 || bestAc <= 0.0) return false;

        // ---- 2. fine global tempo + phase: pulse train over the whole song ---------
        double P = (double) bestLag, bestScore = -1.0, phase = 0.0;
        const double P0 = (double) bestLag;
        for (double cand = P0 * 0.985; cand <= P0 * 1.015; cand += P0 * 0.0003)
        {
            const long nb = (long) ((double) nF / cand);
            if (nb < 8) continue;
            for (long ph = 0; ph < (long) cand; ++ph)
            {
                double sc = 0.0;
                for (long k = 0; k < nb; ++k)
                {
                    const long f = ph + (long) std::lround ((double) k * cand);
                    if (f >= nF) break;
                    sc += os[(size_t) f];
                }
                sc /= (double) nb;
                if (sc > bestScore) { bestScore = sc; P = cand; phase = (double) ph; }
            }
        }
        (void) phase;

        // ---- 3. dynamic-programming beat tracker (follows tempo drift) -------------
        const double alpha = 100.0, beta = 2.0;
        const long lo = std::max (1L, (long) (0.5 * P));
        const long hi = (long) (4.5 * P);

        std::vector<double> pen ((size_t) hi + 1, -1.0e9);
        for (long d = lo; d <= hi; ++d)
        {
            const double r = (double) d / P;
            const double m = std::max (1.0, std::round (r));
            const double lr = std::log (r / m);
            pen[(size_t) d] = -(alpha * lr * lr + beta * (m - 1.0));
        }

        std::vector<double> C ((size_t) nF, 0.0);
        std::vector<long> prev ((size_t) nF, -1);
        for (long t = 0; t < nF; ++t)
        {
            double best = 0.0; long bp = -1;
            const long t0 = std::max (0L, t - hi), t1 = t - lo;
            for (long tau = t0; tau <= t1; ++tau)
            {
                const double v = C[(size_t) tau] + pen[(size_t) (t - tau)];
                if (v > best) { best = v; bp = tau; }
            }
            C[(size_t) t] = o[(size_t) t] + best;
            prev[(size_t) t] = bp;
        }

        long tEnd = std::max (0L, nF - (long) (1.5 * P));
        for (long t = tEnd; t < nF; ++t)
            if (C[(size_t) t] > C[(size_t) tEnd]) tEnd = t;

        std::vector<double> chain;   // frames, backwards
        for (long t = tEnd; t >= 0; t = prev[(size_t) t])
        {
            chain.push_back ((double) t);
            if (prev[(size_t) t] < 0) break;
        }
        std::reverse (chain.begin(), chain.end());
        if (chain.size() < 8) return false;

        // ---- 4. fill gaps (breakdowns) and extend to both ends of the file ---------
        std::vector<double> f;   // beat frames
        for (size_t i = 0; i < chain.size(); ++i)
        {
            if (i > 0)
            {
                const double g = chain[i] - chain[i - 1];
                const long m = std::max (1L, std::lround (g / P));
                for (long k = 1; k < m; ++k)
                    f.push_back (chain[i - 1] + g * (double) k / (double) m);
            }
            f.push_back (chain[i]);
        }

        auto localPeriod = [&] (bool atStart)
        {
            const size_t k = std::min<size_t> (8, f.size() - 1);
            return atStart ? (f[k] - f[0]) / (double) k : (f.back() - f[f.size() - 1 - k]) / (double) k;
        };
        const double pStart = localPeriod (true), pEnd = localPeriod (false);

        std::vector<double> front;
        for (double t = f.front() - pStart; t >= 0.0; t -= pStart) front.push_back (t);
        std::reverse (front.begin(), front.end());
        f.insert (f.begin(), front.begin(), front.end());
        for (double t = f.back() + pEnd; t * (double) hop + (double) hop < (double) n; t += pEnd) f.push_back (t);

        beats.clear();
        for (double fr : f) beats.push_back (fr * (double) hop);

        // ---- 5. refine every beat to the true start of its attack ------------------
        // (the tracker works in 2.5 ms steps; the same rule is used for every beat so
        //  they all sit at the same place relative to their hit)
        const long W = std::max (8L, (long) (0.002 * sr));
        std::vector<double> refined (beats.size());
        for (size_t i = 0; i < beats.size(); ++i)
        {
            refined[i] = beats[i];
            const long a = std::max (W + 1, (long) beats[i] - (long) (0.012 * sr));
            const long b = std::min ((long) n - 1, (long) beats[i] + (long) (0.020 * sr));
            if (b - a < 4 * W) continue;

            std::vector<double> sm ((size_t) (b - a), 0.0);
            double run = 0.0;
            for (long k = a - W; k < b; ++k)
            {
                run += std::abs ((double) x[(size_t) k]);
                if (k - W >= a - W) run -= std::abs ((double) x[(size_t) (k - W)]);
                if (k >= a) sm[(size_t) (k - a)] = run / (double) W;
            }
            double lo = 1.0e30, hi = 0.0;
            for (double v : sm) { lo = std::min (lo, v); hi = std::max (hi, v); }
            if (hi < 0.01 || hi < 1.5 * lo) continue;        // no clear attack here: keep the tracker's position

            const double thr = lo + 0.25 * (hi - lo);
            long pos = -1;
            for (long k = 0; k < b - a; ++k)
                if (sm[(size_t) k] >= thr) { pos = a + k; break; }
            if (pos < 0) continue;

            pos -= W / 2 + 24;                                // window centre + small pre-roll
            for (int k = 0; k < 96 && pos > 1; ++k, --pos)    // zero crossing: no click when cutting
                if ((x[(size_t) pos] <= 0.0f && x[(size_t) pos - 1] > 0.0f) || (x[(size_t) pos] >= 0.0f && x[(size_t) pos - 1] < 0.0f))
                    break;
            if (pos > 0) refined[i] = (double) pos;
        }
        bool ordered = true;
        for (size_t i = 1; i < refined.size(); ++i)
            if (refined[i] <= refined[i - 1] + 0.02 * sr) ordered = false;
        if (ordered) beats = refined;

        return beats.size() >= 8;
    }

    CutoWarpingResult autoWarpSong (const AudioData& src, bool& ok)
    {
        CutoWarpingResult r;
        const size_t n = src.numSamples();
        const std::vector<float> mono = mixToMono (src);

        std::vector<double> beats;
        ok = trackBeats (mono, src.sampleRate, beats);
        if (! ok) return r;

        const size_t M = beats.size() - 1;     // number of beat intervals
        r.startSample = beats.front();
        r.endSample = std::min ((double) n, beats.back());
        r.loopBeats = (int) M;
        r.bpm = (double) M * 60.0 / ((r.endSample - r.startSample) / src.sampleRate);

        for (size_t k = 1; k < M; ++k)
        {
            Marker m;
            m.sample = beats[k];
            m.beatTarget = (double) k;
            r.markers.push_back (m);
        }
        return r;
    }
}

CutoWarpingResult autoWarp (const AudioData& src, double sensitivity, CutoWarpingMode mode)
{
    const double seconds = src.sampleRate > 0.0 ? (double) src.numSamples() / src.sampleRate : 0.0;
    if (mode == CutoWarpingMode::Song || (mode == CutoWarpingMode::Auto && seconds >= 30.0))
    {
        bool ok = false;
        const auto r = autoWarpSong (src, ok);
        if (ok) return r;
        // no usable pulse: fall back to the per-hit method below
    }

    CutoWarpingResult r;
    const size_t n = src.numSamples();
    r.startSample = 0.0;
    r.endSample = (double) n;
    if (n < 2048) return r;

    const std::vector<float> mono = mixToMono (src);

    // Only skip a *short* silent pre-roll (< 100 ms) so the first hit becomes
    // beat 1. Longer silences are musical (a rest) and are kept. The end of
    // the file is never trimmed: loops normally end in a decayed tail.
    size_t first = 0;
    while (first < n && std::abs (mono[first]) < 0.01f) ++first;

    size_t start = 0;
    const size_t preRoll = (size_t) (src.sampleRate * 0.002);
    if (first < n && first > preRoll && (double) first < 0.100 * src.sampleRate)
        start = first - preRoll;
    const size_t end = n;

    r.startSample = (double) start;
    r.endSample = (double) end;

    const auto onsets = detectTransients (mono, src.sampleRate, sensitivity, start, end);
    const double edge = src.sampleRate * 0.012;
    for (double p : onsets)
    {
        if (p - (double) start < edge) continue;     // first hit = loop start
        if ((double) end - p < edge * 1.5) continue;
        Marker m;
        m.sample = p;
        r.markers.push_back (m);
    }

    r.loopBeats = estimateLoopBeats (mono, src.sampleRate, start, end);
    r.bpm = (double) r.loopBeats * 60.0 / ((double) (end - start) / src.sampleRate);
    return r;
}

} // namespace dogside
