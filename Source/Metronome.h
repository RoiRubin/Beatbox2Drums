#pragma once
#include "Common.h"

// Click generator: call trigger() on each beat, then next() once per sample.
class Metronome
{
public:
    void prepare(double sampleRate)
    {
        sr = (float) sampleRate;
        decay = std::exp(-1.0f / (0.02f * sr));
        env = 0.0f;
    }
    void trigger(bool accent)
    {
        freq = accent ? 1600.0f : 1000.0f;
        ph = 0.0f;
        env = 1.0f;
    }
    float next(float gain)
    {
        if (env < 1.0e-4f) return 0.0f;
        const float o = std::sin(ph) * env * gain;
        ph += juce::MathConstants<float>::twoPi * freq / sr;
        env *= decay;
        return o;
    }

private:
    float sr = 44100.0f, decay = 0.999f, ph = 0.0f, freq = 1000.0f, env = 0.0f;
};
