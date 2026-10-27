#pragma once
#include "PluginProcessor.h"

//==============================================================================
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView(BeatboxProcessor& p) : proc(p)
    {
        smooth.fill(-100.0f);
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.fillAll(juce::Colour(0xff14161a));

        auto fx = [&](float f) { return r.getX() + r.getWidth() * std::log(f / 20.0f) / std::log(1000.0f); };
        auto dy = [&](float db) { return r.getBottom() - r.getHeight() * juce::jlimit(0.0f, 1.0f, (db + 90.0f) / 90.0f); };

        g.setFont(12.0f);
        for (int s = 0; s < MAXD; ++s)
        {
            if (!proc.slotOn(s)) continue;
            const float lo = proc.getParam(BeatboxProcessor::key(s, "lo"));
            const float hi = proc.getParam(BeatboxProcessor::key(s, "hi"));
            if (lo <= 25.0f && hi >= 19000.0f) continue;
            const float x1 = fx(lo), x2 = fx(hi);
            g.setColour(slotColour(s).withAlpha(0.16f));
            g.fillRect(x1, r.getY(), x2 - x1, r.getHeight());
            g.setColour(slotColour(s).withAlpha(0.7f));
            g.drawVerticalLine((int) x1, r.getY(), r.getBottom());
            g.drawVerticalLine((int) x2, r.getY(), r.getBottom());
            g.drawText(proc.getSlotName(s), juce::Rectangle<float>(x1, r.getY() + 2, x2 - x1, 16),
                       juce::Justification::centred);
        }

        for (float f : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
        {
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.drawVerticalLine((int) fx(f), r.getY(), r.getBottom());
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawText(f >= 1000 ? juce::String((int) (f / 1000)) + "k" : juce::String((int) f),
                       juce::Rectangle<float>(fx(f) + 2, r.getBottom() - 16, 40, 14),
                       juce::Justification::left);
        }

        const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 44100.0;
        juce::Path p;
        p.startNewSubPath(r.getX(), r.getBottom());
        for (int k = 1; k <= N / 2; ++k)
        {
            const float f = (float) (k * sr / N);
            if (f < 20.0f) continue;
            if (f > 20000.0f) break;
            p.lineTo(fx(f), dy(smooth[(size_t) k]));
        }
        p.lineTo(r.getRight(), r.getBottom());
        p.closeSubPath();
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.fillPath(p);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.strokePath(p, juce::PathStrokeType(1.2f));
    }

private:
    void timerCallback() override
    {
        proc.getScope(data.data(), N);
        window.multiplyWithWindowingTable(data.data(), (size_t) N);
        std::fill(data.begin() + N, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform(data.data());
        for (int k = 0; k <= N / 2; ++k)
        {
            const float db = juce::Decibels::gainToDecibels(data[(size_t) k] / ((float) N / 4.0f), -100.0f);
            smooth[(size_t) k] = juce::jmax(db, smooth[(size_t) k] - 2.0f);
        }
        repaint();
    }

    static constexpr int N = 2048;
    BeatboxProcessor& proc;
    juce::dsp::FFT fft { 11 };
    juce::dsp::WindowingFunction<float> window { (size_t) N, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, 2 * N> data {};
    std::array<float, N / 2 + 1> smooth {};
};
