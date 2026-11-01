#pragma once
// Czysty C++ (bez JUCE): kompresor + limiter z lookahead.
// Limiter gwarantuje, ze sygnal wyjsciowy nie przekroczy ceilingDb.

#include <algorithm>
#include <cmath>
#include <vector>

#include "Denoise.h"

struct DynamicsParams
{
    // odszumiacz widmowy, dziala jako pierwszy w lancuchu (przed AGC)
    bool  denoiseEnabled     = true;
    float denoiseReductionDb = 12.0f;  // maksymalne tlumienie szumu
    float denoiseSensitivity = 3.0f;   // przesuniecie estymaty szumu (dB), wyzej = agresywniej
    float thresholdDb = -22.0f;
    float ratio       = 3.0f;
    float attackMs    = 10.0f;
    float releaseMs   = 120.0f;
    float makeupDb    = 3.0f;
    float ceilingDb   = -6.0f;   // twardy sufit limitera
    float kneeDb      = 6.0f;

    // automatyczne wyrownywanie poziomu (AGC), dziala PRZED kompresorem
    bool  agcEnabled   = true;
    float targetRmsDb  = -20.0f;   // docelowy poziom RMS mowy
    float gateDb       = -50.0f;   // ponizej tego poziomu AGC zamraza wzmocnienie (cisza, szum)
    float maxBoostDb   = 24.0f;
    float maxCutDb     = 18.0f;
    float agcAttackMs  = 150.0f;   // jak szybko AGC sciaga glosne fragmenty
    float agcReleaseMs = 1500.0f;  // jak wolno podnosi ciche fragmenty
};

class SpeechDynamics
{
public:
    void prepare (double sampleRate, int numChannels, float lookaheadMs = 5.0f)
    {
        sr = sampleRate;
        ch = std::max (1, numChannels);
        denoise.prepare (sr, ch);
        N  = std::max (2, (int) std::lround (lookaheadMs * 0.001 * sr));
        delay.assign ((size_t) ch, std::vector<float> ((size_t) N, 0.0f));
        tHist.assign ((size_t) N, 1.0f);
        uHist.assign ((size_t) N, 1.0f);
        reset();
        updateCoeffs();
    }

    void reset()
    {
        for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
        std::fill (tHist.begin(), tHist.end(), 1.0f);
        std::fill (uHist.begin(), uHist.end(), 1.0f);
        pos = 0;
        uSum = (double) N;
        grState = 0.0f;
        msPower = 0.0f;
        agcGainDb = 0.0f;
        denoise.reset();
    }

    void setParams (const DynamicsParams& p)
    {
        params = p;
        updateCoeffs();
        denoise.setParams (p.denoiseEnabled, p.denoiseReductionDb, p.denoiseSensitivity);
    }

    // opoznienie lacznie: odszumiacz (stale, takze gdy wylaczony) + lookahead limitera
    int getLatencySamples() const { return (N - 1) + denoise.getLatencySamples(); }

    // przetwarza w miejscu; data[kanal][probka]
    void process (float* const* data, int numSamples)
    {
        denoise.process (data, numSamples);   // 1. odszumianie (reszta lancucha dziala na czystszym sygnale)

        const float ceilLin = std::pow (10.0f, params.ceilingDb / 20.0f);
        const float makeup  = params.makeupDb;
        const float thr     = params.thresholdDb;
        const float knee    = std::max (0.01f, params.kneeDb);
        const float slope   = 1.0f / std::max (1.0f, params.ratio) - 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            // ---------- AGC: powolne wyrownywanie poziomu ----------
            float agcLin = 1.0f;
            if (params.agcEnabled)
            {
                double sq = 0.0;
                for (int c = 0; c < ch; ++c) sq += (double) data[c][i] * (double) data[c][i];
                sq /= (double) ch;
                msPower += (float) ((sq - (double) msPower) * (double) powCoef);

                const float lvl = 10.0f * std::log10 (std::max (msPower, 1.0e-12f));
                if (lvl > params.gateDb)
                {
                    const float want = std::max (-params.maxCutDb,
                                       std::min (params.maxBoostDb, params.targetRmsDb - lvl));
                    const float k = want < agcGainDb ? agcAtt : agcRel;
                    agcGainDb = k * agcGainDb + (1.0f - k) * want;
                }
                agcLin = std::pow (10.0f, agcGainDb / 20.0f);
            }

            // ---------- kompresor (stereo linked) ----------
            float det = 0.0f;
            for (int c = 0; c < ch; ++c) det = std::max (det, std::abs (data[c][i]) * agcLin);
            const float levelDb = 20.0f * std::log10 (std::max (det, 1.0e-9f));

            const float over = levelDb - thr;
            float gainDb = 0.0f;                                  // <= 0
            if (2.0f * over < -knee)       gainDb = 0.0f;
            else if (2.0f * std::abs (over) <= knee)
            {
                const float x = over + knee * 0.5f;
                gainDb = slope * x * x / (2.0f * knee);
            }
            else                           gainDb = slope * over;

            const float reduction = -gainDb;                      // >= 0
            const float coef = reduction > grState ? attCoef : relCoef;
            grState = coef * grState + (1.0f - coef) * reduction;

            const float g = agcLin * std::pow (10.0f, (makeup - grState) / 20.0f);

            float y[2] = { 0.0f, 0.0f };
            float peak = 0.0f;
            for (int c = 0; c < ch; ++c)
            {
                const float v = data[c][i] * g;
                if (c < 2) y[c] = v;
                peak = std::max (peak, std::abs (v));
            }

            // ---------- limiter lookahead ----------
            const float t = peak > ceilLin ? ceilLin / peak : 1.0f;
            tHist[(size_t) pos] = t;

            float u = 1.0f;                                        // min z ostatnich N wartosci t
            for (int k = 0; k < N; ++k) u = std::min (u, tHist[(size_t) k]);

            uSum += (double) u - (double) uHist[(size_t) pos];
            uHist[(size_t) pos] = u;
            const float s = std::min (1.0f, (float) (uSum / (double) N));

            const int readPos = (pos + 1) % N;                     // probka sprzed N-1 krokow
            for (int c = 0; c < ch; ++c)
            {
                auto& d = delay[(size_t) c];
                const float delayed = d[(size_t) readPos];
                d[(size_t) pos] = (c < 2) ? y[c] : data[c][i] * g;
                float out = delayed * s;
                out = std::max (-ceilLin, std::min (ceilLin, out)); // zabezpieczenie
                data[c][i] = out;
            }

            pos = (pos + 1) % N;
        }
    }

private:
    void updateCoeffs()
    {
        auto coeff = [this] (float ms) {
            return (float) std::exp (-1.0 / (std::max (0.01f, ms) * 0.001 * sr));
        };
        attCoef = coeff (params.attackMs);
        relCoef = coeff (params.releaseMs);
        agcAtt  = coeff (params.agcAttackMs);
        agcRel  = coeff (params.agcReleaseMs);
        powCoef = 1.0f - coeff (300.0f);   // okno RMS ok. 300 ms
    }

    DynamicsParams params;
    SpectralDenoiser denoise;
    double sr = 48000.0;
    int ch = 2, N = 240, pos = 0;
    float attCoef = 0.0f, relCoef = 0.0f, grState = 0.0f;
    float msPower = 0.0f, agcGainDb = 0.0f, powCoef = 0.0f, agcAtt = 0.0f, agcRel = 0.0f;
    double uSum = 0.0;
    std::vector<std::vector<float>> delay;
    std::vector<float> tHist, uHist;
};
