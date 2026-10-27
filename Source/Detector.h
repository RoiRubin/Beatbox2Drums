#pragma once
#include "Common.h"

//==============================================================================
// Onset detector + spectral feature extractor. Used live and for analysing recordings.
struct Detector
{
    struct Event
    {
        float f[NF] = {};
        float vel = 0;
        bool rising = false;   // envelope still rising at the end of the window (breath-like)
    };

    void prepare(double sampleRate)
    {
        sr = sampleRate;
        win = (int) (0.020 * sr);
        hold = (int) (0.060 * sr);
        fastC = 1.0f - std::exp(-1.0f / (0.001f * (float) sr));
        slowC = 1.0f - std::exp(-1.0f / (0.050f * (float) sr));
        floorFallC = 1.0f - std::exp(-1.0f / (0.2f * (float) sr));
        floorRiseC = 1.0f - std::exp(-1.0f / (8.0f * (float) sr));
        const float lo0 = 40.0f, hi0 = 14000.0f;
        const float ratio = std::pow(hi0 / lo0, 1.0f / (float) NB);
        for (int b = 0; b < NB; ++b)
        {
            const float l = lo0 * std::pow(ratio, (float) b), h = l * ratio;
            bank[b].set(l, h, (float) sr);
        }
        det.set(lo0, hi0, (float) sr);
        reset();
    }

    void reset()
    {
        fast = slow = autoFloor = 0;
        pending = false; holdLeft = 0; lastNoiseThr = 0;
        for (auto& b : bank) b.clear();
        det.clear();
    }

    // returns true when an analysis window has just completed (ev filled in)
    bool push(float x, bool canTrigger, float userThr, bool autoNoise, float marginLin,
              float learnedThr, float sens, Event& ev)
    {
        const float d0 = det.process(x);
        float by[NB];
        for (int b = 0; b < NB; ++b) by[b] = bank[b].process(x);

        const float a = std::abs(d0);
        fast += (a - fast) * fastC;
        slow += (a - slow) * slowC;
        autoFloor += (fast - autoFloor) * (fast < autoFloor ? floorFallC : floorRiseC);

        lastNoiseThr = juce::jmax(autoNoise ? autoFloor * marginLin : 0.0f, learnedThr);
        const float thr = juce::jmax(userThr, lastNoiseThr);

        if (holdLeft > 0) --holdLeft;
        if (canTrigger && !pending && holdLeft == 0 && fast > thr && fast > slow * sens)
            startWindow();
        if (pending)
        {
            for (int b = 0; b < NB; ++b) e[b] += by[b] * by[b];
            if (left > win / 2) e1 += d0 * d0; else e2 += d0 * d0;
            peak = juce::jmax(peak, a);
            if (fast > peakFast) { peakFast = fast; peakIdx = win - left; }
            if (--left == 0)
            {
                pending = false;
                finish(ev);
                return true;
            }
        }
        return false;
    }

    // begin an analysis window at the next sample (also used to measure hand-placed hits)
    void startWindow()
    {
        pending = true; left = win; holdLeft = hold;
        peak = 0; peakFast = 0; peakIdx = 0; e1 = e2 = 0;
        for (auto& v : e) v = 0;
    }

    double sr = 44100;
    int win = 882, hold = 2646;
    float fast = 0, slow = 0, autoFloor = 0, lastNoiseThr = 0;

private:
    void finish(Event& ev)
    {
        float db[NB], mx = -200.0f, mean = 0;
        for (int b = 0; b < NB; ++b)
        {
            db[b] = 10.0f * std::log10(e[b] / (float) win + 1e-12f);
            mx = juce::jmax(mx, db[b]);
        }
        for (int b = 0; b < NB; ++b) { db[b] = juce::jmax(db[b], mx - 50.0f); mean += db[b]; }
        mean /= (float) NB;
        for (int b = 0; b < NB; ++b) ev.f[b] = db[b] - mean;
        ev.f[NB] = juce::jlimit(-40.0f, 10.0f, 10.0f * std::log10((e2 + 1e-12f) / (e1 + 1e-12f)));
        ev.vel = juce::jlimit(0.3f, 1.0f, peak * 2.0f);
        ev.rising = peakIdx > (int) (0.85f * (float) win);
    }

    float fastC = 0, slowC = 0, floorFallC = 0, floorRiseC = 0;
    Band bank[NB], det;
    bool pending = false;
    int left = 0, holdLeft = 0, peakIdx = 0;
    float peak = 0, peakFast = 0, e1 = 0, e2 = 0;
    float e[NB] = {};
};
