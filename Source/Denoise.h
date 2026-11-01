#pragma once
// Odszumiacz widmowy: STFT (75% zakladki, okno sqrt-Hann) + filtr Wienera "decision-directed".
// Szum jest szacowany automatycznie przez sledzenie minimum wygladzonego widma, wiec nie
// potrzeba probki szumu. Wzmocnienie liczone jest ze sredniej mocy wszystkich kanalow i stosowane
// jednakowo do kazdego z nich (stabilny obraz stereo).
//
// Opoznienie: getLatencySamples() = fftSize - 1 (stale, rowniez gdy odszumianie jest wylaczone,
// zeby przelaczanie nie powodowalo skokow czasowych).

#include <algorithm>
#include <cmath>
#include <vector>

class SpectralDenoiser
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        ch = std::max (1, numChannels);
        N  = sr <= 50000.0 ? 1024 : (sr <= 100000.0 ? 2048 : 4096);
        hop = N / 4;
        M   = N / 2 + 1;

        // tablice FFT
        cosT.resize ((size_t) N / 2);
        sinT.resize ((size_t) N / 2);
        for (int k = 0; k < N / 2; ++k)
        {
            cosT[(size_t) k] = (float) std::cos (2.0 * kPi * k / N);
            sinT[(size_t) k] = (float) std::sin (2.0 * kPi * k / N);
        }
        int bits = 0; while ((1 << bits) < N) ++bits;
        rev.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }

        window.resize ((size_t) N);
        for (int n = 0; n < N; ++n) window[(size_t) n] = (float) std::sin (kPi * n / N);

        inRing.assign ((size_t) ch, std::vector<float> ((size_t) N, 0.0f));
        outAcc.assign ((size_t) ch, std::vector<float> ((size_t) N, 0.0f));
        re.assign ((size_t) ch, std::vector<float> ((size_t) N, 0.0f));
        im.assign ((size_t) ch, std::vector<float> ((size_t) N, 0.0f));

        pinst.assign ((size_t) M, 0.0f);
        ps.assign ((size_t) M, 0.0f);
        nest.assign ((size_t) M, 0.0f);
        gPrev.assign ((size_t) M, 1.0f);
        gammaPrev.assign ((size_t) M, 1.0f);
        gain.assign ((size_t) M, 1.0f);
        gainS.assign ((size_t) M, 1.0f);

        const double hopSec = (double) hop / sr;
        rise = (float) std::pow (10.0, 0.1 * kRiseDbPerSec * hopSec);

        reset();
    }

    void reset()
    {
        for (auto& v : inRing) std::fill (v.begin(), v.end(), 0.0f);
        for (auto& v : outAcc) std::fill (v.begin(), v.end(), 0.0f);
        std::fill (gPrev.begin(), gPrev.end(), 1.0f);
        std::fill (gammaPrev.begin(), gammaPrev.end(), 1.0f);
        wpos = 0;
        hopCounter = 0;
        initialized = false;
    }

    void setParams (bool enabled, float reductionDb, float sensitivityDb)
    {
        on   = enabled;
        gMin = std::pow (10.0f, -std::max (0.0f, reductionDb) / 20.0f);
        bias = std::pow (10.0f, std::max (0.0f, sensitivityDb) / 10.0f);
    }

    int getLatencySamples() const { return N - 1; }

    // przetwarza w miejscu; data[kanal][probka]
    void process (float* const* data, int numSamples)
    {
        const int mask = N - 1;
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < ch; ++c)
                inRing[(size_t) c][(size_t) wpos] = data[c][i];

            const int start = (wpos + 1) & mask;               // najstarsza probka w oknie

            if (++hopCounter == hop)
            {
                hopCounter = 0;
                processFrame (start);
            }

            for (int c = 0; c < ch; ++c)
            {
                data[c][i] = outAcc[(size_t) c][(size_t) start];
                outAcc[(size_t) c][(size_t) start] = 0.0f;
            }
            wpos = start;
        }
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kRiseDbPerSec = 3.0;     // jak szybko estymata szumu moze rosnac
    static constexpr float  kSmoothA = 0.8f;         // wygladzanie mocy widma w czasie
    static constexpr float  kDdAlpha = 0.95f;        // decision-directed

    void fft (float* r, float* x, bool inverse) const
    {
        for (int i = 0; i < N; ++i)
        {
            const int j = rev[(size_t) i];
            if (j > i) { std::swap (r[i], r[j]); std::swap (x[i], x[j]); }
        }
        for (int len = 2; len <= N; len <<= 1)
        {
            const int half = len >> 1, step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const float wr = cosT[(size_t) (j * step)];
                    const float wi = inverse ? sinT[(size_t) (j * step)] : -sinT[(size_t) (j * step)];
                    const int a = i + j, b = a + half;
                    const float tr = r[b] * wr - x[b] * wi;
                    const float ti = r[b] * wi + x[b] * wr;
                    r[b] = r[a] - tr; x[b] = x[a] - ti;
                    r[a] += tr;       x[a] += ti;
                }
        }
    }

    void processFrame (int start)
    {
        const int mask = N - 1;
        const float invCh = 1.0f / (float) ch;

        for (int c = 0; c < ch; ++c)
        {
            float* r = re[(size_t) c].data();
            float* x = im[(size_t) c].data();
            for (int k = 0; k < N; ++k)
            {
                r[k] = inRing[(size_t) c][(size_t) ((start + k) & mask)] * window[(size_t) k];
                x[k] = 0.0f;
            }
            fft (r, x, false);
        }

        // moc chwilowa (srednia po kanalach)
        for (int k = 0; k < M; ++k)
        {
            float p = 0.0f;
            for (int c = 0; c < ch; ++c)
                p += re[(size_t) c][(size_t) k] * re[(size_t) c][(size_t) k]
                   + im[(size_t) c][(size_t) k] * im[(size_t) c][(size_t) k];
            pinst[(size_t) k] = p * invCh;
        }

        if (! initialized)
        {
            for (int k = 0; k < M; ++k)
            {
                ps[(size_t) k] = nest[(size_t) k] = std::max (pinst[(size_t) k], 1.0e-10f);
            }
            initialized = true;
        }

        // estymata szumu (sledzenie minimum) + filtr Wienera decision-directed
        for (int k = 0; k < M; ++k)
        {
            const size_t u = (size_t) k;
            ps[u] = kSmoothA * ps[u] + (1.0f - kSmoothA) * pinst[u];

            if (ps[u] < nest[u]) nest[u] = ps[u];
            else                 nest[u] *= rise;
            nest[u] = std::max (nest[u], 1.0e-10f);

            const float noise = nest[u] * bias;
            const float gamma = pinst[u] / noise;
            const float xi = kDdAlpha * gPrev[u] * gPrev[u] * gammaPrev[u]
                           + (1.0f - kDdAlpha) * std::max (gamma - 1.0f, 0.0f);
            gain[u] = std::max (xi / (1.0f + xi), gMin);
            gammaPrev[u] = gamma;
        }

        // wygladzanie po czestotliwosci (mniej "muzycznych" artefaktow)
        for (int k = 0; k < M; ++k)
        {
            const float a = gain[(size_t) std::max (k - 1, 0)];
            const float b = gain[(size_t) k];
            const float c = gain[(size_t) std::min (k + 1, M - 1)];
            gainS[(size_t) k] = 0.25f * a + 0.5f * b + 0.25f * c;
            gPrev[(size_t) k] = gainS[(size_t) k];
        }

        const float scale = 0.5f / (float) N;   // 1/N (IFFT) * 1/2 (suma okien sqrt-Hann^2 przy zakladce 75%)

        for (int c = 0; c < ch; ++c)
        {
            float* r = re[(size_t) c].data();
            float* x = im[(size_t) c].data();

            if (on)
            {
                for (int k = 0; k < M; ++k)
                {
                    const float g = gainS[(size_t) k];
                    r[k] *= g; x[k] *= g;
                    if (k > 0 && k < M - 1) { r[N - k] *= g; x[N - k] *= g; }
                }
            }

            fft (r, x, true);

            for (int k = 0; k < N; ++k)
                outAcc[(size_t) c][(size_t) ((start + k) & mask)] += r[k] * window[(size_t) k] * scale;
        }
    }

    double sr = 48000.0;
    int ch = 2, N = 1024, hop = 256, M = 513;
    bool on = true;
    float gMin = 0.25f, bias = 2.0f, rise = 1.0f;
    int wpos = 0, hopCounter = 0;
    bool initialized = false;

    std::vector<float> cosT, sinT, window;
    std::vector<int> rev;
    std::vector<std::vector<float>> inRing, outAcc, re, im;
    std::vector<float> pinst, ps, nest, gPrev, gammaPrev, gain, gainS;
};
