#pragma once
#include "PluginProcessor.h"

//==============================================================================
class DrumRow : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    DrumRow(BeatboxProcessor& p, int s) : proc(p), slot(s)
    {
        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        const juce::String pre = "d" + juce::String(s) + "_";

        name.setMultiLine(false);
        name.setText(proc.getSlotName(slot), false);
        name.onTextChange = [this] { proc.setSlotName(slot, name.getText()); };
        addAndMakeVisible(name);

        type.addItemList(juce::StringArray { "Kick", "Snare", "Hat", "Open Hat", "Tom", "Ignore" }, 1);
        typeAtt = std::make_unique<CA>(p.apvts, pre + "syn", type);
        addAndMakeVisible(type);

        choke.addItemList(BeatboxProcessor::chokeNames(), 1);
        chokeAtt = std::make_unique<CA>(p.apvts, pre + "choke", choke);
        choke.setTooltip("Choke group: when this drum plays it cuts off the other drums in the same group. "
                         "Auto = hats and open hats choke each other; No choke = plays independently.");
        addAndMakeVisible(choke);

        for (auto* sl : { &lvl, &lo, &hi })
        {
            sl->setSliderStyle(juce::Slider::LinearBar);
            addAndMakeVisible(*sl);
        }
        lvlAtt = std::make_unique<SA>(p.apvts, pre + "lvl", lvl);
        loAtt = std::make_unique<SA>(p.apvts, pre + "lo", lo);
        hiAtt = std::make_unique<SA>(p.apvts, pre + "hi", hi);
        lvl.textFromValueFunction = [](double v) { return "Level " + juce::String(v, 1) + " dB"; };
        lo.textFromValueFunction = [](double v) { return "Low " + juce::String((int) v) + " Hz"; };
        hi.textFromValueFunction = [](double v) { return "High " + juce::String((int) v) + " Hz"; };
        lvl.updateText(); lo.updateText(); hi.updateText();

        train.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb3261e));
        train.onClick = [this] { proc.setTrainSlot(proc.getTrainSlot() == slot ? -1 : slot); };
        clear.onClick = [this] { proc.clearTemplate(slot); };
        synthBtn.onClick = [this] { proc.useSynth(slot); };
        remove.onClick = [this] { proc.removeSlot(slot); };
        load.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser>("Choose a drum sample", juce::File(),
                                                          "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
            juce::Component::SafePointer<DrumRow> safe(this);
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [safe](const juce::FileChooser& fc)
                                 {
                                     if (safe == nullptr) return;
                                     const auto f = fc.getResult();
                                     if (f.existsAsFile()) safe->proc.loadSample(safe->slot, f);
                                 });
        };
        for (auto* c : std::initializer_list<juce::Component*> { &train, &count, &clear, &load, &synthBtn, &remove })
            addAndMakeVisible(*c);
        count.setJustificationType(juce::Justification::centred);
    }

    // drag & drop of samples from the FL browser / Explorer
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (auto& f : files) if (isAudioFile(juce::File(f))) return true;
        return false;
    }
    void fileDragEnter(const juce::StringArray&, int, int) override { hover = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { hover = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        hover = false;
        for (auto& f : files)
            if (isAudioFile(juce::File(f))) { proc.loadSample(slot, juce::File(f)); break; }
        repaint();
    }

    void syncName() { name.setText(proc.getSlotName(slot), false); }

    void refresh()
    {
        const int n = proc.getExampleCount(slot);
        count.setText(n > 0 ? juce::String(n) + " ex" : (proc.hasPreset(slot) ? "preset" : "untrained"),
                      juce::dontSendNotification);
        const bool tr = proc.getTrainSlot() == slot;
        train.setButtonText(tr ? "Stop" : "Train");
        train.setToggleState(tr, juce::dontSendNotification);
        const auto sn = proc.getSampleName(slot);
        load.setButtonText(sn.isEmpty() ? "Load sample" : sn);
        flash *= 0.85f;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colour(hover ? 0xff2d4a7a : 0xff23272e));
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1), 5.0f);
        g.setColour(slotColour(slot).withAlpha(0.2f + 0.8f * flash));
        g.fillEllipse(8.0f, (float) getHeight() * 0.5f - 7.0f, 14.0f, 14.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(4, 4);
        r.removeFromLeft(24);
        name.setBounds(r.removeFromLeft(90));       r.removeFromLeft(4);
        type.setBounds(r.removeFromLeft(90));       r.removeFromLeft(4);
        choke.setBounds(r.removeFromLeft(84));      r.removeFromLeft(4);
        load.setBounds(r.removeFromLeft(100));      r.removeFromLeft(4);
        synthBtn.setBounds(r.removeFromLeft(55));   r.removeFromLeft(4);
        train.setBounds(r.removeFromLeft(60));      r.removeFromLeft(4);
        count.setBounds(r.removeFromLeft(62));      r.removeFromLeft(4);
        clear.setBounds(r.removeFromLeft(50));      r.removeFromLeft(4);
        lvl.setBounds(r.removeFromLeft(96));        r.removeFromLeft(4);
        lo.setBounds(r.removeFromLeft(96));         r.removeFromLeft(4);
        hi.setBounds(r.removeFromLeft(96));         r.removeFromLeft(4);
        remove.setBounds(r.removeFromLeft(30));
    }

    float flash = 0;

private:
    BeatboxProcessor& proc;
    int slot;
    bool hover = false;
    juce::TextEditor name;
    juce::ComboBox type, choke;
    juce::Slider lvl, lo, hi;
    juce::TextButton train { "Train" }, clear { "Clear" }, load { "Load sample" }, synthBtn { "Synth" }, remove { "X" };
    juce::Label count;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAtt, chokeAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lvlAtt, loAtt, hiAtt;
};
