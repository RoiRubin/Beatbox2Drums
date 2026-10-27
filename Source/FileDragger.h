#pragma once
#include "PluginProcessor.h"

//==============================================================================
// Drag these into FL Studio to drop the converted MIDI / rendered drum audio
class FileDragger : public juce::Component
{
public:
    FileDragger(BeatboxProcessor& p, juce::String label, std::function<juce::File()> exporter)
        : proc(p), text(std::move(label)), exportFile(std::move(exporter)) {}

    void paint(juce::Graphics& g) override
    {
        const bool ready = proc.getRecHitCount() > 0;
        g.setColour(ready ? juce::Colour(0xff2d6cdf) : juce::Colour(0xff3a3f47));
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1), 5.0f);
        g.setColour(juce::Colours::white.withAlpha(ready ? 1.0f : 0.5f));
        g.drawText(text, getLocalBounds(), juce::Justification::centred);
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 6 || proc.getRecHitCount() == 0) return;
        dragging = true;
        const auto f = exportFile();
        if (f.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles({ f.getFullPathName() }, false, this);
        dragging = false;
    }
    void mouseUp(const juce::MouseEvent&) override { dragging = false; }

private:
    BeatboxProcessor& proc;
    juce::String text;
    std::function<juce::File()> exportFile;
    bool dragging = false;
};
