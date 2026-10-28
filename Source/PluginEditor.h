#pragma once
#include "PluginProcessor.h"
#include "SpectrumView.h"
#include "LevelMeter.h"
#include "FileDragger.h"
#include "ClipView.h"
#include "TimingBar.h"
#include "DrumRow.h"

//==============================================================================
class BeatboxEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit BeatboxEditor(BeatboxProcessor& p)
        : AudioProcessorEditor(&p), proc(p), spectrum(p), meter(p), dragger(p, "Drag MIDI into FL", [&p] { return p.exportTempMidi(); }),
          audioDragger(p, "Drag drums to playlist", [&p] { return p.exportTempAudio(); }), clip(p),
          timing(p, [this] { clip.quantize(); })
    {
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
        addAndMakeVisible(spectrum);
        addAndMakeVisible(meter);
        addAndMakeVisible(clip);
        addAndMakeVisible(timing);

        listening.setButtonText("Listening");
        autoNoise.setButtonText("Auto noise floor");
        guardBtn.setButtonText("Breath guard");
        fullBtn.setButtonText("Full level");
        learn.setButtonText("Learn room noise (2s)");
        for (auto* c : std::initializer_list<juce::Component*> { &listening, &autoNoise, &guardBtn, &fullBtn, &learn, &saveKit, &loadKit })
            addAndMakeVisible(*c);
        listenAtt = std::make_unique<BA>(p.apvts, "active", listening);
        noiseAtt = std::make_unique<BA>(p.apvts, "noiseauto", autoNoise);
        guardAtt = std::make_unique<BA>(p.apvts, "guard", guardBtn);
        fullAtt = std::make_unique<BA>(p.apvts, "fulllvl", fullBtn);
        fullBtn.setTooltip("Every drum hit plays at full volume instead of following how loud you beatboxed. "
                           "Segments whose volume you edited keep their own value.");
        learn.onClick = [this] { proc.startLearning(); };
        saveKit.onClick = [this] { doSaveKit(); };
        loadKit.onClick = [this] { doLoadKit(); };

        status.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(status);

        addKnob(sens, "Onset sensitivity", "sens");
        addKnob(nmargin, "Noise margin dB", "nmargin");
        addKnob(match, "Match tolerance", "match");
        addKnob(mix, "Drums mix", "mix");

        for (auto* c : std::initializer_list<juce::Component*> { &recBtn, &playBtn, &origBtn, &analyseBtn, &saveMidi, &dragger, &saveWav, &audioDragger, &recStatus })
            addAndMakeVisible(*c);
        recBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb3261e));
        playBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2e7d32));
        recBtn.onClick = [this]
        {
            if (proc.isRecording() || proc.isCountingIn()) { proc.stopRecording(); proc.analyseRecording(); }
            else proc.startRecording();
        };
        analyseBtn.onClick = [this] { if (!proc.isRecording() && !proc.isCountingIn()) proc.analyseRecording(); };
        origBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2e7d32));
        playBtn.onClick = [this] { if (proc.isPlaying()) proc.stopPlayback(); else { proc.stopOriginal(); proc.startPlayback(); } };
        origBtn.onClick = [this] { if (proc.isPlayingOriginal()) proc.stopOriginal(); else proc.playOriginal(); };
        saveMidi.onClick = [this] { doSaveMidi(); };
        saveWav.onClick = [this] { doSaveWav(); };
        recStatus.setJustificationType(juce::Justification::centredLeft);

        for (int s = 0; s < MAXD; ++s)
        {
            rows[(size_t) s] = std::make_unique<DrumRow>(p, s);
            addChildComponent(*rows[(size_t) s]);
        }
        addBtn.onClick = [this] { proc.addSlot(); };
        addAndMakeVisible(addBtn);

        setResizable(true, true);
        setResizeLimits(1040, 740, 3000, 2000);
        setWantsKeyboardFocus(true);   // Ctrl+Z / Ctrl+Y work from anywhere in the window
        setSize(1100, 880);
        updateVisibility();
        startTimerHz(30);
    }

    void mouseDown(const juce::MouseEvent&) override { grabKeyboardFocus(); }

    bool keyPressed(const juce::KeyPress& k) override
    {
        const auto mods = k.getModifiers();
        if (mods.isCommandDown() && !mods.isAltDown())
        {
            const int code = (int) juce::CharacterFunctions::toUpperCase((juce::juce_wchar) k.getKeyCode());
            if (code == 'Z') { if (mods.isShiftDown()) proc.redoStep(); else proc.undoStep(); return true; }
            if (code == 'Y') { proc.redoStep(); return true; }
        }
        return false;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1b1e23));
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.setFont(11.0f);
        g.drawText(juce::String("v") + BB_VERSION + "  (built " + __DATE__ + " " + __TIME__ + ")", getWidth() - 310, getHeight() - 14, 300, 12,
                   juce::Justification::centredRight);
        if (proc.getSlotsFree())
        {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.setFont(12.0f);
            g.drawText("Drop a sample here to create a new drum (drop a beatbox take on the clip)", getWidth() - 480, getHeight() - 36, 470, 20,
                       juce::Justification::centredRight);
        }
    }

    // drop audio files on empty space -> a new drum per file
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (auto& f : files) if (isAudioFile(juce::File(f))) return true;
        return false;
    }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        for (auto& path : files)
        {
            const juce::File f(path);
            if (!isAudioFile(f)) continue;
            const int s = proc.addSlot();
            if (s < 0) break;
            proc.loadSample(s, f);
            proc.setSlotName(s, f.getFileNameWithoutExtension());
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(30);
        listening.setBounds(top.removeFromLeft(100));
        autoNoise.setBounds(top.removeFromLeft(135));
        guardBtn.setBounds(top.removeFromLeft(110));
        fullBtn.setBounds(top.removeFromLeft(100));
        learn.setBounds(top.removeFromLeft(160).reduced(0, 2));
        top.removeFromLeft(10);
        saveKit.setBounds(top.removeFromLeft(75).reduced(0, 2));
        top.removeFromLeft(4);
        loadKit.setBounds(top.removeFromLeft(75).reduced(0, 2));
        status.setBounds(top);
        area.removeFromTop(6);

        auto vis = area.removeFromTop(140);
        meter.setBounds(vis.removeFromRight(90));
        vis.removeFromRight(8);
        spectrum.setBounds(vis);
        area.removeFromTop(6);

        auto globals = area.removeFromTop(30);
        const int gw = globals.getWidth() / 4;
        for (auto* k : { &sens, &nmargin, &match })
            layoutKnob(*k, globals.removeFromLeft(gw).reduced(4, 0));
        layoutKnob(mix, globals.reduced(4, 0));
        area.removeFromTop(6);

        auto rec = area.removeFromTop(30);
        recBtn.setBounds(rec.removeFromLeft(95));       rec.removeFromLeft(4);
        analyseBtn.setBounds(rec.removeFromLeft(80));   rec.removeFromLeft(4);
        playBtn.setBounds(rec.removeFromLeft(70));      rec.removeFromLeft(4);
        origBtn.setBounds(rec.removeFromLeft(105));     rec.removeFromLeft(4);
        saveMidi.setBounds(rec.removeFromLeft(95));     rec.removeFromLeft(4);
        dragger.setBounds(rec.removeFromLeft(135));     rec.removeFromLeft(4);
        saveWav.setBounds(rec.removeFromLeft(80));      rec.removeFromLeft(4);
        audioDragger.setBounds(rec.removeFromLeft(160)); rec.removeFromLeft(10);
        recStatus.setBounds(rec);
        area.removeFromTop(6);
        timing.setBounds(area.removeFromTop(28));
        area.removeFromTop(8);

        int nOn = 0;
        for (int s = 0; s < MAXD; ++s) nOn += proc.slotOn(s) ? 1 : 0;
        auto rowArea = area.removeFromBottom(nOn * 36 + 40);   // + strip for the add button
        area.removeFromBottom(8);
        clip.setBounds(area);
        for (int s = 0; s < MAXD; ++s)
            if (proc.slotOn(s)) rows[(size_t) s]->setBounds(rowArea.removeFromTop(36));
        addBtn.setBounds(10, getHeight() - 38, 130, 28);
    }

private:
    struct Knob
    {
        juce::Slider s { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Label l;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };

    void addKnob(Knob& k, const juce::String& name, const juce::String& id)
    {
        k.l.setText(name, juce::dontSendNotification);
        k.s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
        addAndMakeVisible(k.l);
        addAndMakeVisible(k.s);
        k.att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, id, k.s);
    }
    void layoutKnob(Knob& k, juce::Rectangle<int> r)
    {
        k.l.setBounds(r.removeFromLeft(juce::jmin(105, r.getWidth() / 2)));
        k.s.setBounds(r);
    }

    void doSaveKit()
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Save kit (settings + training + sample paths)",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("MyBeatboxKit.bbkit"),
            "*.bbkit");
        juce::Component::SafePointer<BeatboxEditor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safe](const juce::FileChooser& fc)
                             {
                                 if (safe == nullptr || fc.getResult() == juce::File()) return;
                                 safe->proc.saveKit(fc.getResult().withFileExtension("bbkit"));
                             });
    }
    void doLoadKit()
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Load kit", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.bbkit");
        juce::Component::SafePointer<BeatboxEditor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe](const juce::FileChooser& fc)
                             {
                                 if (safe == nullptr) return;
                                 const auto f = fc.getResult();
                                 if (f.existsAsFile()) safe->proc.loadKit(f);
                             });
    }
    void doSaveWav()
    {
        if (proc.getRecHitCount() == 0) return;
        chooser = std::make_unique<juce::FileChooser>(
            "Save drums audio (the beatbox replaced by drums)",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("beatbox_drums.wav"), "*.wav");
        juce::Component::SafePointer<BeatboxEditor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safe](const juce::FileChooser& fc)
                             {
                                 if (safe == nullptr || fc.getResult() == juce::File()) return;
                                 safe->proc.saveAudio(fc.getResult().withFileExtension("wav"));
                             });
    }
    void doSaveMidi()
    {
        if (proc.getRecHitCount() == 0) return;
        chooser = std::make_unique<juce::FileChooser>(
            "Save MIDI",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("beatbox.mid"), "*.mid");
        juce::Component::SafePointer<BeatboxEditor> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safe](const juce::FileChooser& fc)
                             {
                                 if (safe == nullptr || fc.getResult() == juce::File()) return;
                                 safe->proc.saveMidi(fc.getResult().withFileExtension("mid"));
                             });
    }

    void updateVisibility()
    {
        int mask = 0, nOn = 0;
        for (int s = 0; s < MAXD; ++s)
            if (proc.slotOn(s)) { mask |= 1 << s; ++nOn; }
        const int ver = proc.getStateVersion();
        if (mask == lastMask && ver == lastVersion) return;
        for (int s = 0; s < MAXD; ++s)
        {
            const bool on = (mask >> s) & 1, was = lastMask >= 0 && ((lastMask >> s) & 1);
            rows[(size_t) s]->setVisible(on);
            if (on && (!was || ver != lastVersion)) rows[(size_t) s]->syncName();
        }
        addBtn.setEnabled(nOn < MAXD);
        lastMask = mask; lastVersion = ver;
        resized();
    }

    void timerCallback() override
    {
        proc.undoTick();
        updateVisibility();
        if (proc.takeAutoStop()) proc.analyseRecording();
        for (int s = 0; s < MAXD; ++s)
        {
            const int c = proc.hitCount[s].load();
            if (c != lastCount[s]) { lastCount[s] = c; rows[(size_t) s]->flash = 1.0f; }
            if (proc.slotOn(s)) rows[(size_t) s]->refresh();
        }

        const int ts = proc.getTrainSlot();
        juce::String msg;
        if (proc.isLearning()) msg = "Learning room noise... stay quiet";
        else if (ts >= 0)
            msg = proc.slotSynth(ts) == SYN_IGNORE
                      ? "Training '" + proc.getSlotName(ts) + "': make the sounds to IGNORE (breaths, pops), then Stop"
                      : "Training " + proc.getSlotName(ts) + ": beatbox that sound ~10 times, then Stop";
        else if (proc.getParam("active") < 0.5f) msg = "Listening is OFF";
        status.setText(msg, juce::dontSendNotification);

        const bool rec = proc.isRecording(), countIn = proc.isCountingIn();
        recBtn.setButtonText(rec || countIn ? "Stop rec" : "Record");
        recBtn.setToggleState(rec || countIn, juce::dontSendNotification);
        proc.autoSaveIfDirty();
        const bool pl = proc.isPlaying();
        playBtn.setButtonText(pl ? "Stop" : "Play");
        playBtn.setToggleState(pl, juce::dontSendNotification);
        const bool po = proc.isPlayingOriginal();
        origBtn.setButtonText(po ? "Stop original" : "Play original");
        origBtn.setToggleState(po, juce::dontSendNotification);
        origBtn.setEnabled(!rec && !countIn && proc.getRecLength() > 0);
        if (countIn) recStatus.setText("Count-in... " + juce::String(proc.getRecBpm(), 1) + " BPM, recording starts on the next bar", juce::dontSendNotification);
        else if (rec) recStatus.setText("Recording... " + juce::String(proc.getRecSeconds(), 1) + " s (max 30)", juce::dontSendNotification);
        else if (proc.getRecSeconds() > 0)
            recStatus.setText(juce::String(proc.getRecSeconds(), 1) + " s recorded, " + juce::String(proc.getRecHitCount())
                                  + " drum hits found", juce::dontSendNotification);
        else recStatus.setText("Press Record, beatbox, press Stop rec  -  tempo " + juce::String(proc.getTempo(), 1) + " BPM",
                               juce::dontSendNotification);
        dragger.repaint();
        audioDragger.repaint();
    }

    BeatboxProcessor& proc;
    SpectrumView spectrum;
    LevelMeter meter;
    FileDragger dragger, audioDragger;
    ClipView clip;
    TimingBar timing;
    juce::ToggleButton listening, autoNoise, guardBtn, fullBtn;
    juce::TextButton learn, saveKit { "Save kit" }, loadKit { "Load kit" }, addBtn { "+ Add drum" };
    juce::TextButton recBtn { "Record" }, analyseBtn { "Analyse" }, playBtn { "Play" }, origBtn { "Play original" }, saveMidi { "Save MIDI" }, saveWav { "Save WAV" };
    juce::Label status, recStatus;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> listenAtt, noiseAtt, guardAtt, fullAtt;
    Knob sens, nmargin, match, mix;
    std::array<std::unique_ptr<DrumRow>, MAXD> rows;
    int lastCount[MAXD] = {};
    int lastMask = -1, lastVersion = -1;
};
