// Test rdzenia DSP na syntetycznym sygnale podobnym do mowy.
// Kompilacja: g++ -O2 -std=c++17 -I../Source test_dynamics.cpp -o test_dynamics
//
// UWAGA: sygnal jest syntetyczny (modulowany szum), wiec wartosci "typowych pikow"
// sa tylko orientacyjne. Finalne strojenie robi sie na prawdziwej mowie.

#include "Dynamics.h"

#include <algorithm>
#include <cstdio>
#include <random>

static float dbToLin (float db) { return std::pow (10.0f, db / 20.0f); }
static float linToDb (float x)  { return 20.0f * std::log10 (std::max (x, 1.0e-9f)); }

// "mowa": szum z obwiednia sylab (4 Hz) i fraz (0.3 Hz); opcjonalnie trzaski blisko pelnej skali
static std::vector<float> makeSpeechLike (double sr, float seconds, float baseDb, unsigned seed, bool clicks)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> noise (0.0f, 1.0f);
    const size_t n = (size_t) (sr * seconds);
    std::vector<float> x (n);
    float lp = 0.0f;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const float syl    = (float) std::max (0.0, std::sin (2.0 * M_PI * 4.0 * t));
        const float phrase = 0.4f + 0.6f * (float) (0.5 + 0.5 * std::sin (2.0 * M_PI * 0.3 * t));
        lp += 0.15f * (noise (rng) - lp);
        x[i] = lp * 3.0f * syl * phrase * dbToLin (baseDb);
    }
    if (clicks)
        for (size_t k = (size_t) (sr * 0.7); k < n; k += (size_t) (sr * 1.3))
            for (size_t j = 0; j < 20 && k + j < n; ++j) x[k + j] = (j % 2 ? -1.0f : 1.0f) * 0.95f;
    return x;
}

static float rmsDb (const std::vector<float>& x, size_t from)
{
    double s = 0.0; for (size_t i = from; i < x.size(); ++i) s += (double) x[i] * x[i];
    return linToDb ((float) std::sqrt (s / (double) (x.size() - from)));
}

static float peakDb (const std::vector<float>& x, size_t from = 0)
{
    float p = 0.0f; for (size_t i = from; i < x.size(); ++i) p = std::max (p, std::abs (x[i]));
    return linToDb (p);
}

// mediana pikow w blokach po 100 ms (tylko bloki z sygnalem) - "typowy pik"
static float typicalPeakDb (const std::vector<float>& x, size_t from, double sr)
{
    std::vector<float> peaks;
    const size_t blk = (size_t) (sr * 0.1);
    for (size_t i = from; i + blk <= x.size(); i += blk)
    {
        float p = 0.0f; for (size_t j = i; j < i + blk; ++j) p = std::max (p, std::abs (x[j]));
        if (linToDb (p) > -70.0f) peaks.push_back (linToDb (p));
    }
    if (peaks.empty()) return -200.0f;
    std::sort (peaks.begin(), peaks.end());
    return peaks[peaks.size() / 2];
}

static void processAll (SpeechDynamics& dyn, std::vector<std::vector<float>>& bufs)
{
    const int channels = (int) bufs.size();
    std::vector<float*> ptrs ((size_t) channels);
    const size_t total = bufs[0].size();
    size_t pos = 0; int blk = 17;
    while (pos < total)                                   // bloki o zmiennej dlugosci jak w hoscie
    {
        const size_t len = std::min ((size_t) blk, total - pos);
        for (int c = 0; c < channels; ++c) ptrs[(size_t) c] = bufs[(size_t) c].data() + pos;
        dyn.process (ptrs.data(), (int) len);
        pos += len;
        blk = (blk * 7 + 13) % 1024 + 1;
    }
}

int main()
{
    bool ok = true;
    const double sr = 48000.0;

    // ---------- A) wyrownywanie poziomu: rozne poziomy wejsciowe, bez trzaskow ----------
    std::printf ("A) Wyrownywanie poziomu (48 kHz stereo, sufit -6 dB, po 8 s ustalania)\n");
    float minRms = 1000.0f, maxRms = -1000.0f;
    const DynamicsParams defs;
    for (float base : { -40.0f, -30.0f, -20.0f, -10.0f })
    {
        SpeechDynamics dyn; dyn.prepare (sr, 2); dyn.setParams (DynamicsParams{});
        std::vector<std::vector<float>> b { makeSpeechLike (sr, 20.0f, base, 1u, false),
                                            makeSpeechLike (sr, 20.0f, base, 2u, false) };
        const size_t from = (size_t) (sr * 8.0);
        const float inRms = rmsDb (b[0], from), inTyp = typicalPeakDb (b[0], from, sr);
        processAll (dyn, b);
        const float outRms = rmsDb (b[0], from), outTyp = typicalPeakDb (b[0], from, sr);
        const float outPk = std::max (peakDb (b[0]), peakDb (b[1]));
        std::printf ("   wej: RMS %6.1f, typ. pik %6.1f | wyj: RMS %6.1f, typ. pik %6.1f, max pik %6.2f dB\n",
                     inRms, inTyp, outRms, outTyp, outPk);
        const bool inReach = inRms + defs.maxBoostDb >= defs.targetRmsDb;   // AGC jest w stanie dojsc do celu
        if (inReach) { minRms = std::min (minRms, outRms); maxRms = std::max (maxRms, outRms); }
        else std::printf ("      ^ wymaga podbicia o %.0f dB, a Max Boost to %.0f dB: poza zasiegiem, pomijam w rozrzucie\n",
                          defs.targetRmsDb - inRms, defs.maxBoostDb);
        ok &= outPk <= -6.0f + 0.001f;
    }
    const bool spreadOk = (maxRms - minRms) <= 3.0f;
    std::printf ("   rozrzut RMS na wyjsciu (przypadki w zasiegu Max Boost): %.1f dB -> %s\n\n",
                 maxRms - minRms, spreadOk ? "OK" : "ZA DUZY");
    ok &= spreadOk;

    // ---------- B) twardy sufit: trzaski blisko pelnej skali, rozne formaty ----------
    std::printf ("B) Twardy sufit przy trzaskach (wej. pik -0.4 dB)\n");
    struct Case { const char* name; double sr; int ch; float ceiling; };
    for (const Case& c : { Case { "48k stereo, sufit -6 dB",  48000.0, 2, -6.0f },
                           Case { "44.1k mono, sufit -9 dB",  44100.0, 1, -9.0f },
                           Case { "96k stereo, sufit -9 dB",  96000.0, 2, -9.0f },
                           Case { "48k 6 kanalow, sufit -6 dB", 48000.0, 6, -6.0f } })
    {
        DynamicsParams p; p.ceilingDb = c.ceiling;
        SpeechDynamics dyn; dyn.prepare (c.sr, c.ch); dyn.setParams (p);
        std::vector<std::vector<float>> b;
        for (int k = 0; k < c.ch; ++k) b.push_back (makeSpeechLike (c.sr, 12.0f, -20.0f, 10u + (unsigned) k, true));
        processAll (dyn, b);
        float worst = -200.0f; for (auto& v : b) worst = std::max (worst, peakDb (v));
        const bool pass = worst <= c.ceiling + 0.001f;
        std::printf ("   %-28s max pik na wyjsciu %6.2f dB -> %s\n", c.name, worst, pass ? "OK" : "PRZEKROCZONY");
        ok &= pass;
    }

    // ---------- C) opoznienie lookahead zgodne z deklarowanym ----------
    {
        SpeechDynamics d; d.prepare (48000.0, 1);
        DynamicsParams p; p.denoiseEnabled = false; p.agcEnabled = false;
        p.makeupDb = 0.0f; p.thresholdDb = 0.0f; p.ceilingDb = 0.0f;
        d.setParams (p);
        std::vector<float> x (4000, 0.0f); x[100] = 0.5f;
        float* ptr = x.data(); d.process (&ptr, (int) x.size());
        int idx = -1; for (int i = 0; i < (int) x.size(); ++i) if (std::abs (x[(size_t) i]) > 0.1f) { idx = i; break; }
        const bool lat = idx == 100 + d.getLatencySamples();
        std::printf ("\nC) Opoznienie: zglaszane %d, zmierzone %d -> %s\n", d.getLatencySamples(), idx - 100, lat ? "OK" : "BLAD");
        ok &= lat;
    }

    // ---------- D) odszumianie ----------
    {
        std::printf ("\nD) Odszumianie (48 kHz mono; AGC i kompresja wylaczone, zeby mierzyc sam odszumiacz)\n");
        DynamicsParams p;
        p.agcEnabled = false; p.makeupDb = 0.0f; p.thresholdDb = 0.0f; p.ceilingDb = 0.0f;
        p.denoiseEnabled = true; p.denoiseReductionDb = 12.0f;

        std::mt19937 rng (7);
        std::normal_distribution<float> gauss (0.0f, 1.0f);
        const float noiseLin = dbToLin (-50.0f);

        // D1: sam stacjonarny szum bialy -50 dB RMS
        {
            std::vector<float> x ((size_t) (sr * 10.0));
            for (auto& s : x) s = gauss (rng) * noiseLin;
            const size_t from = (size_t) (sr * 5.0);
            const float inR = rmsDb (x, from);
            SpeechDynamics d; d.prepare (sr, 1); d.setParams (p);
            std::vector<std::vector<float>> b { x }; processAll (d, b);
            const float red = inR - rmsDb (b[0], from);
            const bool pass = red >= 8.0f && red <= 16.0f;
            std::printf ("   sam szum: tlumienie %.1f dB (ustawione 12) -> %s\n", red, pass ? "OK" : "POZA ZAKRESEM");
            ok &= pass;
        }

        // D2: mowa-podobny sygnal + ten sam szum; pomiar osobno w frazach i w przerwach
        {
            auto speech = makeSpeechLike (sr, 20.0f, -20.0f, 3u, false);
            std::vector<float> x (speech.size());
            for (size_t i = 0; i < x.size(); ++i) x[i] = speech[i] + gauss (rng) * noiseLin;

            SpeechDynamics d; d.prepare (sr, 1); d.setParams (p);
            const size_t L = (size_t) d.getLatencySamples();
            std::vector<std::vector<float>> b { x }; processAll (d, b);

            auto region = [&] (const std::vector<float>& v, size_t shift, bool active)
            {
                double s = 0.0; size_t cnt = 0;
                for (size_t i = (size_t) (sr * 6.0); i + shift < v.size(); ++i)
                {
                    const double sn = std::sin (2.0 * M_PI * 4.0 * (double) i / sr);
                    if (active ? sn > 0.5 : sn < -0.5) { const double w = v[i + shift]; s += w * w; ++cnt; }
                }
                return linToDb ((float) std::sqrt (s / (double) cnt));
            };

            const float gapIn = region (x, 0, false),     gapOut = region (b[0], L, false);
            const float actIn = region (x, 0, true),      actOut = region (b[0], L, true);
            const float gapRed = gapIn - gapOut, actChange = actOut - actIn;
            const bool pass = gapRed >= 8.0f && std::abs (actChange) <= 2.0f;
            std::printf ("   przerwy:  %6.1f -> %6.1f dB (tlumienie %.1f dB)\n", gapIn, gapOut, gapRed);
            std::printf ("   mowa:     %6.1f -> %6.1f dB (zmiana %+.1f dB) -> %s\n",
                         actIn, actOut, actChange, pass ? "OK" : "POZA ZAKRESEM");
            ok &= pass;
        }
    }

    std::printf ("\nWYNIK: %s\n", ok ? "wszystkie testy zaliczone" : "SA BLEDY");
    return ok ? 0 : 1;
}
