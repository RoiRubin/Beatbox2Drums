// Shared constants and small value types used by every part of the plugin
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <algorithm>
#include <memory>
#include <array>
#include <vector>

static constexpr const char* BB_VERSION = "1.5";   // shown in the editor's bottom-left corner
static constexpr int MAXD = 8;     // max drums
static constexpr int NF = 2 * (12 + 2) + 1;   // features per hit: 2 sub-windows x (12 MFCC + brightness + noisiness) + decay
static constexpr int MAXEX = 256;             // training examples kept per drum (oldest dropped)
static constexpr int SYN_KICK = 0, SYN_SNARE = 1, SYN_HAT = 2, SYN_OPEN = 3, SYN_TOM = 4, SYN_IGNORE = 5;

// Choke groups: a drum that plays cuts off every other drum in its group. Choice index 0 = Auto (closed and
// open hats choke each other, everything else plays independently), 1 = Off, 2.. = explicit group 1, 2, ...
static constexpr int CHOKE_AUTO = 0, CHOKE_OFF = 1;
static constexpr int AUTO_HAT_GROUP = 1000;
static inline int chokeGroupOf(int choice, int syn)
{
    if (choice == CHOKE_AUTO) return (syn == SYN_HAT || syn == SYN_OPEN) ? AUTO_HAT_GROUP : -1;
    return choice == CHOKE_OFF ? -1 : choice;
}

static juce::Colour slotColour(int i)
{
    static const juce::uint32 c[MAXD] = { 0xffe5484d, 0xfff5a524, 0xff3dd6c6, 0xff8e7cff,
                                          0xff4aa3ff, 0xffa3d64a, 0xffff7ac6, 0xffc9b037 };
    return juce::Colour(c[i % MAXD]);
}

static bool isAudioFile(const juce::File& f) { return f.hasFileExtension("wav;aif;aiff;flac;ogg;mp3"); }

//==============================================================================
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void set(bool highpass, float f, float sr)
    {
        f = juce::jlimit(10.0f, 0.45f * sr, f);
        const float w = juce::MathConstants<float>::twoPi * f / sr;
        const float cs = std::cos(w), alpha = std::sin(w) / (2.0f * 0.7071f);
        const float a0 = 1.0f + alpha;
        if (highpass) { b0 = (1 + cs) * 0.5f / a0; b1 = -(1 + cs) / a0; b2 = b0; }
        else          { b0 = (1 - cs) * 0.5f / a0; b1 =  (1 - cs) / a0; b2 = b0; }
        a1 = -2.0f * cs / a0;
        a2 = (1.0f - alpha) / a0;
    }
    float process(float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void clear() { z1 = z2 = 0; }
};

struct Band
{
    Biquad hp[2], lp[2];
    void set(float lo, float hi, float sr)
    {
        for (auto& f : hp) f.set(true, lo, sr);
        for (auto& f : lp) f.set(false, hi, sr);
    }
    float process(float x)
    {
        x = hp[0].process(x); x = hp[1].process(x);
        x = lp[0].process(x); x = lp[1].process(x);
        return x;
    }
    void clear() { for (auto& f : hp) f.clear(); for (auto& f : lp) f.clear(); }
};

struct Sample
{
    std::vector<float> data;
    double sr = 44100.0;
};

using Feat = std::array<float, NF>;

struct Voice
{
    bool on = false;
    float vel = 0, t = 0, ph = 0, prev = 0;
    double pos = 0;
    std::shared_ptr<const Sample> smp;
};

struct RecHit
{
    juce::int64 sample;
    int slot;
    float vel;            // loudness of the beatbox hit, 0.3..1
    float vol = -1.0f;    // manual volume 0..1 set by the user, or -1 = automatic (loudness, or 100% in Full level)
};
