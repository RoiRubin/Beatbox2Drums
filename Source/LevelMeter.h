#pragma once
#include "PluginProcessor.h"

//==============================================================================
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    explicit LevelMeter(BeatboxProcessor& p) : proc(p) { startTimerHz(30); }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(0, 6);
        g.fillAll(juce::Colour(0xff14161a));
        auto y = [&](float db) { return r.getBottom() - r.getHeight() * juce::jlimit(0.0f, 1.0f, (db + 70.0f) / 70.0f); };
        auto bar = r.withTrimmedLeft(r.getWidth() * 0.45f).withTrimmedRight(4);

        g.setColour(juce::Colours::white.withAlpha(0.4f));
        g.setFont(11.0f);
        for (int db = 0; db >= -60; db -= 10)
            g.drawText(juce::String(db), juce::Rectangle<float>(0, y((float) db) - 6, bar.getX() - 14, 12),
                       juce::Justification::right);
        g.setColour(juce::Colour(0xff2a2e35));
        g.fillRect(bar);
        g.setColour(juce::Colour(0xff3ddc84));
        g.fillRect(bar.withTop(y(shown)));

        const float gy = y(proc.gateDb.load());
        g.setColour(juce::Colour(0xfff5a524));
        g.fillRect(bar.getX() - 6, gy - 1, bar.getWidth() + 6, 2.0f);

        const float ty = y(proc.getParam("thresh"));
        g.setColour(juce::Colours::white);
        g.fillRect(bar.getX() - 6, ty - 1.5f, bar.getWidth() + 6, 3.0f);
        juce::Path tri;
        tri.addTriangle(bar.getX() - 6, ty - 5, bar.getX() - 6, ty + 5, bar.getX() - 13, ty);
        g.fillPath(tri);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (auto* p = proc.apvts.getParameter("thresh")) p->beginChangeGesture();
        setFromY(e.position.y);
    }
    void mouseDrag(const juce::MouseEvent& e) override { setFromY(e.position.y); }
    void mouseUp(const juce::MouseEvent&) override
    {
        if (auto* p = proc.apvts.getParameter("thresh")) p->endChangeGesture();
    }

private:
    void setFromY(float py)
    {
        auto r = getLocalBounds().toFloat().reduced(0, 6);
        const float db = juce::jmap(py, r.getBottom(), r.getY(), -70.0f, 0.0f);
        if (auto* p = proc.apvts.getParameter("thresh"))
            p->setValueNotifyingHost(p->convertTo0to1(juce::jlimit(-70.0f, 0.0f, db)));
    }
    void timerCallback() override
    {
        shown = juce::jmax(proc.levelDb.load(), shown - 1.5f);
        repaint();
    }

    BeatboxProcessor& proc;
    float shown = -100.0f;
};
