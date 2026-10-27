#pragma once
#include "Common.h"

// The built-in drum synths and sample playback, one sample of a voice at a time.
// Shared by live playback and the offline audio render.
namespace DrumSynth
{
// one sample of a drum voice (synth or sample); shared by live playback and the offline audio render
inline float render(Voice& v, int syn, double sr, juce::Random& rng)
{
    const float dt = 1.0f / (float) sr, twoPi = juce::MathConstants<float>::twoPi;
    float o = 0;
if (v.smp != nullptr)
{
    const auto& dat = v.smp->data;
    const int idx = (int) v.pos;
    if (idx + 1 >= (int) dat.size()) { v.on = false; return 0.0f; }
    const float fr = (float) (v.pos - idx);
    o = (dat[(size_t) idx] + (dat[(size_t) idx + 1] - dat[(size_t) idx]) * fr) * v.vel;
    v.pos += v.smp->sr / sr;
}
else
{
    switch (syn)
    {
        case SYN_KICK:
        {
            const float f = 50.0f + 120.0f * std::exp(-v.t * 35.0f);
            v.ph += twoPi * f * dt;
            o = std::sin(v.ph) * std::exp(-v.t * 10.0f) * v.vel;
            if (v.t > 0.5f) v.on = false;
            break;
        }
        case SYN_SNARE:
        {
            const float nz = rng.nextFloat() * 2 - 1;
            const float hpn = nz - v.prev; v.prev = nz;
            o = (hpn * 0.5f * std::exp(-v.t * 25.0f)
                 + std::sin(twoPi * 190.0f * v.t) * 0.6f * std::exp(-v.t * 35.0f)) * v.vel;
            if (v.t > 0.35f) v.on = false;
            break;
        }
        case SYN_HAT:
        {
            const float nz = rng.nextFloat() * 2 - 1;
            const float hpn = nz - v.prev; v.prev = nz;
            o = hpn * 0.35f * std::exp(-v.t * 90.0f) * v.vel;
            if (v.t > 0.12f) v.on = false;
            break;
        }
        case SYN_OPEN:
        {
            const float nz = rng.nextFloat() * 2 - 1;
            const float hpn = nz - v.prev; v.prev = nz;
            o = hpn * 0.3f * std::exp(-v.t * 8.0f) * v.vel;
            if (v.t > 0.7f) v.on = false;
            break;
        }
        default: // tom
        {
            const float f = 110.0f + 60.0f * std::exp(-v.t * 20.0f);
            v.ph += twoPi * f * dt;
            o = std::sin(v.ph) * std::exp(-v.t * 8.0f) * 0.9f * v.vel;
            if (v.t > 0.6f) v.on = false;
            break;
        }
    }
    v.t += dt;
}
    return o;
}
}
