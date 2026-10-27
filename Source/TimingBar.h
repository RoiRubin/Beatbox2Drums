#pragma once
#include "PluginProcessor.h"

// Metronome and quantizer controls
class TimingBar : public juce::Component, private juce::Timer
{
public:
    TimingBar(BeatboxProcessor& p, std::function<void()> onQuantize) : proc(p)
    {
        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

        metro.setButtonText("Metronome");
        countIn.setButtonText("Count-in");
        hostSync.setButtonText("Host tempo");
        autoQ.setButtonText("Auto-quantize");
        metroAtt = std::make_unique<BA>(p.apvts, "metro", metro);
        countAtt = std::make_unique<BA>(p.apvts, "countin", countIn);
        hostAtt = std::make_unique<BA>(p.apvts, "hostsync", hostSync);
        autoAtt = std::make_unique<BA>(p.apvts, "qauto", autoQ);

        for (auto* sl : { &click, &bpm, &strength })
            sl->setSliderStyle(juce::Slider::LinearBar);
        clickAtt = std::make_unique<SA>(p.apvts, "metrovol", click);
        bpmAtt = std::make_unique<SA>(p.apvts, "bpm", bpm);
        strAtt = std::make_unique<SA>(p.apvts, "qstr", strength);
        click.textFromValueFunction = [](double v) { return "Click " + juce::String(v, 0) + " dB"; };
        strength.textFromValueFunction = [](double v) { return "Strength " + juce::String((int) v) + "%"; };
        bpm.textFromValueFunction = [this](double v)
        {
            return proc.tempoFromHost() ? "Host " + juce::String(proc.getTempo(), 1) + " BPM"
                                        : "Tempo " + juce::String(v, 1) + " BPM";
        };
        grid.addItemList(Quantizer::divisionNames(), 1);
        gridAtt = std::make_unique<CA>(p.apvts, "qdiv", grid);
        grid.setTooltip("Quantize grid");

        quantizeBtn.onClick = std::move(onQuantize);
        quantizeBtn.setTooltip("Snap the selected hits (or all) to the grid");

        for (auto* c : std::initializer_list<juce::Component*> { &metro, &countIn, &click, &hostSync, &bpm, &grid,
                                                                  &strength, &autoQ, &quantizeBtn })
            addAndMakeVisible(*c);
        click.updateText(); bpm.updateText(); strength.updateText();
        startTimerHz(10);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto put = [&r](juce::Component& c, int w, int gap = 6) { c.setBounds(r.removeFromLeft(w)); r.removeFromLeft(gap); };
        put(metro, 92, 2);
        put(countIn, 84, 6);
        put(click, 112);
        put(hostSync, 96, 2);
        put(bpm, 150, 22);
        put(grid, 100);
        put(strength, 130);
        put(autoQ, 118, 2);
        quantizeBtn.setBounds(r.removeFromLeft(90));
    }

private:
    void timerCallback() override
    {
        bpm.setEnabled(!proc.tempoFromHost());   // follows the host while it reports a tempo
        bpm.updateText();
    }

    BeatboxProcessor& proc;
    juce::ToggleButton metro, countIn, hostSync, autoQ;
    juce::Slider click, bpm, strength;
    juce::ComboBox grid;
    juce::TextButton quantizeBtn { "Quantize" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> metroAtt, countAtt, hostAtt, autoAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> clickAtt, bpmAtt, strAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> gridAtt;
};
