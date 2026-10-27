#pragma once
#include "Common.h"

//==============================================================================
// Onset detector + feature extractor. Used live, for analysing recordings and for building presets.
//
// Onsets: a fast (1 ms) envelope must beat the gate (user threshold / auto noise floor / learned noise)
// and jump above the slow (50 ms) envelope. After a hit the detector is disarmed until the sound has
// died down, and for MASK_MS a new hit must be nearly as loud as the last one -- so the tail of a
// "b-oom" kick or a "tsss" doesn't fire again.
//
// Features (NF values) over the first 2 x 15 ms after the onset: for each sub-window 12 MFCCs
// (tone colour, loudness removed), brightness and noisiness; plus how much the level changes
// from the first to the second sub-window. Tested offline on real beatbox recordings.
struct Detector
{
    struct Event
    {
        float f[NF] = {};
        float vel = 0;
        float peak = 0;         // loudest sample in the window (linear); drum volume is judged from this
        float level = 0;        // peak of the fast envelope during the window (linear)
        float centroidHz = 0;   // brightness of the whole window, for the per-drum Low/High limits
        bool rising = false;    // envelope still rising at the end of the window (breath-like)
    };

    static constexpr float MASK_MS = 200.0f, MASK_DB = -6.0f, REARM = 0.25f, SUB_MS = 15.0f;

    void prepare(double sampleRate)
    {
        sr = sampleRate;
        sub = juce::jmax(64, (int) (SUB_MS * 0.001 * sr));
        win = 2 * sub;
        maskLen = (int) (MASK_MS * 0.001 * sr);
        maskGain = juce::Decibels::decibelsToGain(MASK_DB);
        fastC = 1.0f - std::exp(-1.0f / (0.001f * (float) sr));
        slowC = 1.0f - std::exp(-1.0f / (0.050f * (float) sr));
        floorFallC = 1.0f - std::exp(-1.0f / (0.2f * (float) sr));
        floorRiseC = 1.0f - std::exp(-1.0f / (8.0f * (float) sr));
        det.set(40.0f, 14000.0f, (float) sr);

        order = 1;
        while ((1 << order) < sub) ++order;
        nfft = 1 << order;
        fft = std::make_unique<juce::dsp::FFT>(order);
        fftBuf.assign((size_t) nfft * 2, 0.0f);
        winBuf.assign((size_t) win, 0.0f);
        hann.resize((size_t) sub);
        for (int i = 0; i < sub; ++i)
            hann[(size_t) i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float) i / (float) (sub - 1));
        buildMel();
        reset();
    }

    void reset()
    {
        fast = slow = autoFloor = 0;
        pending = false; armed = true; lastNoiseThr = 0;
        since = 1 << 30; peakEnv = lastPeak = 0;
        det.clear();
    }

    // returns true when an analysis window has just completed (ev filled in)
    bool push(float x, bool canTrigger, float userThr, bool autoNoise, float marginLin,
              float learnedThr, float sens, Event& ev)
    {
        const float d0 = det.process(x);
        const float a = std::abs(d0);
        fast += (a - fast) * fastC;
        slow += (a - slow) * slowC;
        autoFloor += (fast - autoFloor) * (fast < autoFloor ? floorFallC : floorRiseC);

        lastNoiseThr = juce::jmax(autoNoise ? autoFloor * marginLin : 0.0f, learnedThr);
        const float gate = juce::jmax(userThr, lastNoiseThr);
        if (since < (1 << 30)) ++since;

        if (pending)
        {
            winBuf[(size_t) (win - left)] = x;
            if (fast > peakFast) { peakFast = fast; peakIdx = win - left; }
            peakEnv = juce::jmax(peakEnv, fast); lastPeak = peakEnv;
            if (--left == 0)
            {
                pending = false;
                finish(ev);
                return true;
            }
            return false;
        }

        if (!armed)
        {
            peakEnv = juce::jmax(peakEnv, fast); lastPeak = peakEnv;
            if (fast < peakEnv * REARM || fast < gate) armed = true;
            return false;
        }

        const float maskThr = since < maskLen ? lastPeak * maskGain * (1.0f - (float) since / (float) maskLen) : 0.0f;
        if (canTrigger && fast > juce::jmax(gate, maskThr) && fast > slow * sens)
            startWindow();
        return false;
    }

    // begin an analysis window at the next sample (also used to measure hand-placed hits)
    void startWindow()
    {
        pending = true; armed = false; left = win; since = 0;
        peakFast = 0; peakIdx = 0; peakEnv = fast;
    }

    // features of a buffer that starts at an onset (used for presets); returns false if too short
    bool measure(const float* data, int n, Event& ev)
    {
        reset();
        startWindow();
        for (int i = 0; i < win; ++i)
            if (push(i < n ? data[i] : 0.0f, false, 1.0f, false, 1.0f, 0.0f, 100.0f, ev)) return true;
        return false;
    }

    double sr = 44100;
    int win = 1322, sub = 661;
    float fast = 0, slow = 0, autoFloor = 0, lastNoiseThr = 0;

private:
    static constexpr int NMEL = 26, NCEP = 12;

    void buildMel()
    {
        const float fmin = 40.0f, fmax = juce::jmin(14000.0f, 0.45f * (float) sr);
        auto mel = [](float f) { return 2595.0f * std::log10(1.0f + f / 700.0f); };
        auto imel = [](float m) { return 700.0f * (std::pow(10.0f, m / 2595.0f) - 1.0f); };
        const int nb = nfft / 2 + 1;
        melW.assign((size_t) NMEL * (size_t) nb, 0.0f);
        float pts[NMEL + 2];
        for (int i = 0; i < NMEL + 2; ++i)
            pts[i] = imel(mel(fmin) + (mel(fmax) - mel(fmin)) * (float) i / (float) (NMEL + 1));
        for (int m = 0; m < NMEL; ++m)
            for (int k = 0; k < nb; ++k)
            {
                const float f = (float) k * (float) sr / (float) nfft;
                const float l = pts[m], c = pts[m + 1], r = pts[m + 2];
                melW[(size_t) (m * nb + k)] = juce::jmax(0.0f, juce::jmin((f - l) / (c - l), (r - f) / (r - c)));
            }
    }

    // power spectrum of sub-window `w` -> MFCCs, brightness (log2 Hz), noisiness; returns ln(energy)
    float analyseSub(int w, float* out, double& pSum, double& pfSum)
    {
        std::fill(fftBuf.begin(), fftBuf.end(), 0.0f);
        for (int i = 0; i < sub; ++i) fftBuf[(size_t) i] = winBuf[(size_t) (w * sub + i)] * hann[(size_t) i];
        fft->performFrequencyOnlyForwardTransform(fftBuf.data(), true);
        const int nb = nfft / 2 + 1;
        double sum = 0, fsum = 0, logSum = 0;
        for (int k = 0; k < nb; ++k)
        {
            const double p = (double) fftBuf[(size_t) k] * fftBuf[(size_t) k] + 1e-12;
            fftBuf[(size_t) k] = (float) p;
            sum += p; fsum += p * (double) k * sr / nfft; logSum += std::log(p);
        }
        float lm[NMEL];
        for (int m = 0; m < NMEL; ++m)
        {
            double acc = 0;
            const float* row = melW.data() + (size_t) (m * nb);
            for (int k = 0; k < nb; ++k) acc += (double) row[k] * fftBuf[(size_t) k];
            lm[m] = (float) std::log(acc + 1e-10);
        }
        const float sc = std::sqrt(2.0f / (float) NMEL);
        for (int c = 1; c <= NCEP; ++c)
        {
            float acc = 0;
            for (int m = 0; m < NMEL; ++m)
                acc += lm[m] * std::cos(juce::MathConstants<float>::pi * (float) c * (2.0f * (float) m + 1.0f) / (2.0f * (float) NMEL));
            out[c - 1] = acc * sc;
        }
        out[NCEP] = (float) std::log2(fsum / sum + 1.0);                                // brightness
        out[NCEP + 1] = (float) (std::exp(logSum / nb) / (sum / nb));                   // noisiness
        pSum += sum; pfSum += fsum;
        return (float) std::log(sum);
    }

    void finish(Event& ev)
    {
        double pSum = 0, pfSum = 0;
        const float e0 = analyseSub(0, ev.f, pSum, pfSum);
        const float e1 = analyseSub(1, ev.f + (NCEP + 2), pSum, pfSum);
        ev.f[NF - 1] = e1 - e0;                                                         // decay shape
        ev.centroidHz = (float) (pfSum / pSum);
        float pk = 0;
        for (auto v : winBuf) pk = juce::jmax(pk, std::abs(v));
        ev.peak = pk;
        ev.vel = juce::jlimit(0.3f, 1.0f, pk * 2.0f);   // absolute guess; the processor uses peak relative to recent hits
        ev.level = peakFast;
        ev.rising = peakIdx > (int) (0.85f * (float) win);
    }

    float fastC = 0, slowC = 0, floorFallC = 0, floorRiseC = 0, maskGain = 0.5f;
    Band det;
    bool pending = false, armed = true;
    int left = 0, peakIdx = 0, since = 1 << 30, maskLen = 8820, order = 10, nfft = 1024;
    float peakFast = 0, peakEnv = 0, lastPeak = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> fftBuf, winBuf, hann, melW;
};
