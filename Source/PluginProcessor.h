#pragma once
#include "Common.h"
#include "Detector.h"
#include "DrumSynth.h"
#include "Metronome.h"
#include "Quantizer.h"

//==============================================================================
// Everything Ctrl+Z can bring back: settings, drum names / samples / training (the kit), the drum hits
// with their volumes, and the recorded beatbox itself.
struct UndoSnapshot
{
    juce::String kitXml;
    std::shared_ptr<const std::vector<RecHit>> hits;
    std::shared_ptr<const std::vector<float>> audio;   // shared between snapshots of the same take
    int recId = 0, recLen = 0;
    double bpm = 120.0, rate = 44100.0;
};

class BeatboxProcessor : public juce::AudioProcessor, private juce::ValueTree::Listener
{
public:
    BeatboxProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          apvts(*this, nullptr, "STATE", layout())
    {
        pFull = apvts.getRawParameterValue("fulllvl");
        for (auto& s : scope) s.store(0.0f);
        for (int s = 0; s < MAXD; ++s)
        {
            hitCount[s].store(0);
            exCount[s].store(0);
            const juce::String pre = "d" + juce::String(s) + "_";
            pOn[s] = apvts.getRawParameterValue(pre + "on");
            pLvl[s] = apvts.getRawParameterValue(pre + "lvl");
            pLo[s] = apvts.getRawParameterValue(pre + "lo");
            pHi[s] = apvts.getRawParameterValue(pre + "hi");
            pSyn[s] = apvts.getRawParameterValue(pre + "syn");
            pChoke[s] = apvts.getRawParameterValue(pre + "choke");
            ex[s].reserve(MAXEX);
        }
        noiseEx.reserve(MAXNOISE);
        buildPresets();
        rebuildNorm();

        // pick up the kit (training, sounds, settings) saved last time; a host project overrides it
        const auto saved = autoSaveFile();
        if (saved.existsAsFile()) loadKit(saved);
        dirty.store(false);
        apvts.state.addListener(this);
    }
    ~BeatboxProcessor() override
    {
        apvts.state.removeListener(this);
        autoSaveIfDirty(true);
    }

    const juce::String getName() const override { return "Beatbox To Drums"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == l.getMainOutputChannelSet()
            && !l.getMainOutputChannelSet().isDisabled();
    }

    //---- state / kits
    void getStateInformation(juce::MemoryBlock& d) override
    {
        syncTemplatesToState();
        if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, d);
    }
    void setStateInformation(const void* d, int n) override
    {
        if (auto xml = getXmlFromBinary(d, n)) { restore(*xml); undoReset.store(true); }
    }

    //---- undo / redo of everything. undoTick() (message thread, ~30 Hz) watches for changes and files a
    // snapshot of the state before each burst of edits; a burst ends after 0.4 s of quiet (or, while
    // training / recording, when that stops), so dragging a slider or a whole training session is one step.
    void undoTick()
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (undoReset.exchange(false)) { undoStack.clear(); redoStack.clear(); haveBase = false; }
        const auto sig = undoSignature();
        if (!haveBase) { base = capture(); haveBase = true; inBurst = false; lastSig = sig; return; }
        if (ignoring && now - ignoreStart < 300) { lastSig = sig; return; }   // settling after an undo / redo
        ignoring = false;
        if (sig != lastSig)
        {
            if (!inBurst)
            {
                undoStack.push_back(base);
                if (undoStack.size() > MAXUNDO) undoStack.erase(undoStack.begin());
                redoStack.clear();
                inBurst = true;
            }
            lastSig = sig;
            lastChangeAt = now;
        }
        else if (inBurst && !(trainSlot.load() >= 0 || recording.load() || countInLeft.load() > 0 || learnLeft.load() > 0)
                 && now - lastChangeAt > 400)
        {
            base = capture();
            inBurst = false;
        }
    }
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    bool undoStep()
    {
        if (undoStack.empty() || recording.load() || countInLeft.load() > 0) return false;
        redoStack.push_back(capture());
        const auto s = undoStack.back();
        undoStack.pop_back();
        applySnapshot(s);
        afterUndoRedo(s);
        return true;
    }
    bool redoStep()
    {
        if (redoStack.empty() || recording.load() || countInLeft.load() > 0) return false;
        undoStack.push_back(capture());
        const auto s = redoStack.back();
        redoStack.pop_back();
        applySnapshot(s);
        afterUndoRedo(s);
        return true;
    }
    bool saveKit(const juce::File& f)
    {
        syncTemplatesToState();
        auto xml = apvts.copyState().createXml();
        return xml != nullptr && xml->writeTo(f);
    }
    bool loadKit(const juce::File& f)
    {
        auto xml = juce::XmlDocument::parse(f);
        if (xml == nullptr) return false;
        restore(*xml);
        return true;
    }
    int getStateVersion() const { return stateVersion.load(); }

    //---- editor API
    float getParam(const juce::String& id) const { return apvts.getRawParameterValue(id)->load(); }
    bool slotOn(int s) const { return pOn[s]->load() > 0.5f; }
    bool getSlotsFree() const
    {
        for (int s = 0; s < MAXD; ++s) if (!slotOn(s)) return true;
        return false;
    }
    int slotSynth(int s) const { return (int) pSyn[s]->load(); }
    void getScope(float* dest, int n) const
    {
        const int w = scopeWrite.load();
        for (int i = 0; i < n; ++i)
            dest[i] = scope[(size_t) ((w - n + i) & (scopeSize - 1))].load(std::memory_order_relaxed);
    }
    void startLearning() { learnPeak = 0; learnLeft.store((int) (2.0 * sr)); }
    bool isLearning() const { return learnLeft.load() > 0; }

    int getTrainSlot() const { return trainSlot.load(); }
    void setTrainSlot(int s) { trainSlot.store(s); }
    int getExampleCount(int s) const { return exCount[s].load(); }
    bool hasPreset(int s) const { return slotSynth(s) != SYN_IGNORE; }   // untrained drums match against their synth sound

    static juce::String key(int s, const char* what) { return "d" + juce::String(s) + "_" + what; }

    juce::String getSlotName(int s) const
    {
        static const char* defaults[3] = { "Kick", "Snare", "Hat" };
        const auto v = apvts.state.getProperty(key(s, "name"));
        if (v.toString().isNotEmpty()) return v.toString();
        return s < 3 ? juce::String(defaults[s]) : "Drum " + juce::String(s + 1);
    }
    void setSlotName(int s, const juce::String& n) { apvts.state.setProperty(key(s, "name"), n, nullptr); }

    juce::String getSampleName(int s) const
    {
        return juce::File(apvts.state.getProperty(key(s, "path")).toString()).getFileName();
    }

    void setParam(const juce::String& id, float v)
    {
        if (auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(v));
    }

    int addSlot()
    {
        for (int s = 0; s < MAXD; ++s)
        {
            if (slotOn(s)) continue;
            clearTemplate(s);
            setSampleInternal(s, nullptr);
            apvts.state.removeProperty(key(s, "path"), nullptr);
            setSlotName(s, "Drum " + juce::String(s + 1));
            setParam(key(s, "lvl"), 0.0f);
            setParam(key(s, "lo"), 20.0f);
            setParam(key(s, "hi"), 20000.0f);
            setParam(key(s, "syn"), (float) SYN_TOM);
            setParam(key(s, "on"), 1.0f);
            return s;
        }
        return -1;
    }
    void removeSlot(int s)
    {
        if (trainSlot.load() == s) trainSlot.store(-1);
        setParam(key(s, "on"), 0.0f);
        clearTemplate(s);
        setSampleInternal(s, nullptr);
        apvts.state.removeProperty(key(s, "path"), nullptr);
        apvts.state.removeProperty(key(s, "name"), nullptr);
    }

    bool loadSample(int s, const juce::File& file)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(file));
        if (r == nullptr || r->lengthInSamples <= 0) return false;
        const juce::int64 maxLen = (juce::int64) (r->sampleRate * 10.0);
        const int n = (int) (r->lengthInSamples < maxLen ? r->lengthInSamples : maxLen);
        juce::AudioBuffer<float> b((int) r->numChannels, n);
        r->read(&b, 0, n, 0, true, true);
        auto smp = std::make_shared<Sample>();
        smp->sr = r->sampleRate;
        smp->data.assign((size_t) n, 0.0f);
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < n; ++i)
                smp->data[(size_t) i] += b.getSample(c, i) / (float) b.getNumChannels();
        setSampleInternal(s, smp);
        apvts.state.setProperty(key(s, "path"), file.getFullPathName(), nullptr);
        return true;
    }
    void useSynth(int s)
    {
        setSampleInternal(s, nullptr);
        apvts.state.removeProperty(key(s, "path"), nullptr);
    }
    void clearTemplate(int s)
    {
        {
            const juce::SpinLock::ScopedLockType l(tmplLock);
            ex[s].clear();
            trainVersion.fetch_add(1);
            for (size_t i = noiseEx.size(); i-- > 0;)          // tails auto-learned while training this drum
                if (noiseEx[i].origin == s) noiseEx.erase(noiseEx.begin() + (std::ptrdiff_t) i);
            rebuildNorm();
        }
        exCount[s].store(0);
        markDirty();
    }

    static juce::StringArray chokeNames()
    {
        juce::StringArray n { "Auto", "No choke" };
        for (int g = 2; g < 2 + MAXD; ++g) n.add("Group " + juce::String(g - 1));
        return n;
    }

    // Use an audio file as the take instead of recording: it replaces the current recording and is analysed
    // straight away. Returns false if the file can't be read or a recording is in progress.
    bool loadRecording(const juce::File& file)
    {
        if (recording.load() || countInLeft.load() > 0 || recBuf.empty()) return false;
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(file));
        if (r == nullptr || r->lengthInSamples <= 0 || r->sampleRate <= 0) return false;

        const double ratio = r->sampleRate / sr;
        const juce::int64 want = (juce::int64) ((double) juce::jmin<juce::int64>(r->lengthInSamples, (juce::int64) (r->sampleRate * 31.0)) / ratio);
        const int outLen = (int) juce::jmin<juce::int64>(want, (juce::int64) recBuf.size());   // keeps the first 30 s
        const int inLen = (int) juce::jmin<juce::int64>(r->lengthInSamples, (juce::int64) std::ceil((double) outLen * ratio) + 4);
        if (outLen <= 0) return false;

        juce::AudioBuffer<float> b((int) r->numChannels, inLen);
        r->read(&b, 0, inLen, 0, true, true);
        std::vector<float> mono((size_t) inLen + 8, 0.0f);   // padding: the interpolator reads a few samples ahead
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < inLen; ++i)
                mono[(size_t) i] += b.getSample(c, i) / (float) b.getNumChannels();

        stopPlayback();
        stopOriginal();
        std::vector<float> out((size_t) outLen);
        if (std::abs(ratio - 1.0) < 1.0e-6) std::copy(mono.begin(), mono.begin() + outLen, out.begin());
        else
        {
            juce::LagrangeInterpolator li;
            li.process(ratio, mono.data(), out.data(), outLen);
        }

        std::copy(out.begin(), out.end(), recBuf.begin());
        recLen.store(outLen, std::memory_order_release);
        recBpm.store(getTempo());
        liveHitN.store(0);
        setRecHits({});
        recId.store(recIdMax.fetch_add(1) + 1);
        analyseRecording();
        return true;
    }

    //---- recording / analysis / playback
    void startRecording()
    {
        stopPlayback();
        const double bpm = getTempo();
        recBpm.store(bpm);
        liveHitN.store(0);
        recLen.store(0);
        setRecHits({});
        recId.store(recIdMax.fetch_add(1) + 1);
        if (getParam("metro") > 0.5f && getParam("countin") > 0.5f)   // one bar of clicks, then record
        {
            countInTotalA.store((int) (4.0 * 60.0 * sr / bpm));
            countInLeft.store(countInTotalA.load());
        }
        else recording.store(true);
    }
    void stopRecording() { countInLeft.store(0); recording.store(false); }
    bool isRecording() const { return recording.load(); }
    bool takeAutoStop() { return autoStopped.exchange(false); }   // recording hit the 30 s limit
    double getRecSeconds() const { return (double) recLen.load() / sr; }
    int getRecHitCount() const { return recHitCount.load(); }
    bool isPlaying() const { return playingA.load(); }
    void startPlayback(juce::int64 from = 0) { playFrom.store(from); playReq.store(true); }
    void stopPlayback() { stopReq.store(true); }

    //---- clip view access (message thread)
    double getRate() const { return sr; }
    int getRecLength() const { return recLen.load(std::memory_order_acquire); }
    const float* getRecData() const { return recBuf.data(); }
    int getRecId() const { return recId.load(); }
    int getHitsVersion() const { return hitsVersion.load(); }
    juce::int64 getPlayPos() const { return playPosA.load(); }
    juce::int64 getOriginalPos() const { return audPosA.load(); }   // read position while "Play original" runs

    // Full level: every automatic hit plays at 100% instead of following how loud the beatbox was
    bool fullLevel() const { return pFull != nullptr && pFull->load() > 0.5f; }
    // volume (0..1) a recorded hit plays at: the user's own value if set, else automatic
    float hitVolume(const RecHit& h) const { return h.vol >= 0.0f ? h.vol : (fullLevel() ? 1.0f : h.vel); }

    std::shared_ptr<const std::vector<RecHit>> getRecHits() const
    {
        const juce::SpinLock::ScopedLockType l(hitsLock);
        return recHits;
    }
    // replace the hit list (must be sorted by sample); playback picks it up straight away
    void setRecHits(std::vector<RecHit> v)
    {
        auto p = std::make_shared<const std::vector<RecHit>>(std::move(v));
        recHitCount.store((int) p->size());
        { const juce::SpinLock::ScopedLockType l(hitsLock); recHits = std::move(p); }
        hitsVersion.fetch_add(1);
    }
    // hits found by the live detector during the current recording
    void getLiveHits(std::vector<RecHit>& out) const
    {
        const int n = liveHitN.load(std::memory_order_acquire);
        out.assign(liveHits.begin(), liveHits.begin() + n);
    }
    // play a slice of the recorded beatbox through the output
    void audition(juce::int64 start, juce::int64 end) { requestAudition(start, end, false); }
    // play the whole recorded beatbox (the original voice, no drums)
    void playOriginal()
    {
        if (recording.load() || countInLeft.load() > 0) return;
        stopPlayback();
        requestAudition(0, recLen.load(), true);
    }
    void stopOriginal() { audStopReq.store(true); }
    bool isPlayingOriginal() const { return audOrigA.load(); }
    // add corrected hits to their drum's training, so future detection improves
    void teach(const std::vector<RecHit>& hs)
    {
        const int len = recLen.load();
        for (const auto& h : hs)
        {
            float f[NF];
            if (h.slot < 0 || h.slot >= MAXD || !slotOn(h.slot) || !featuresAt(h.sample, len, f)) continue;
            {
                const juce::SpinLock::ScopedLockType l(tmplLock);
                addExample(h.slot, f);
            }
            markDirty();
        }
    }

    void analyseRecording()
    {
        const int len = recLen.load();
        std::vector<RecHit> out;
        if (len > 0 && !recording.load())
        {
            Detector d;
            d.prepare(sr);
            bool on[MAXD]; float lo[MAXD], hi[MAXD];
            readSlots(on, lo, hi);
            const bool guard = getParam("guard") > 0.5f, autoNoise = getParam("noiseauto") > 0.5f;
            const float userThr = juce::Decibels::decibelsToGain(getParam("thresh"));
            const float sens = getParam("sens");
            const float marginLin = juce::Decibels::decibelsToGain(getParam("nmargin"));
            const float matchLimit = getParam("match");
            const float learnedThr = learned.load() * 1.5f;

            const juce::SpinLock::ScopedLockType l(tmplLock);
            Detector::Event ev;
            juce::int64 lastAt[MAXD]; float lastLvl[MAXD] = {};
            VelRing rings[MAXD];
            for (auto& a : lastAt) a = -(juce::int64) (10.0 * sr);
            for (int i = 0; i < len; ++i)
            {
                if (!d.push(recBuf[(size_t) i], true, userThr, autoNoise, marginLin, learnedThr, sens, ev)) continue;
                if (guard && ev.rising) continue;
                float dist;
                const int s = classify(ev, on, lo, hi, matchLimit, dist);
                if (s < 0 || slotSynth(s) == SYN_IGNORE) continue;
                juce::int64 onset = (juce::int64) i - d.win + 1;
                if (onset < 0) onset = 0;
                if (isEcho(onset - lastAt[s], ev.level, lastLvl[s], sr)) continue;
                lastAt[s] = onset; lastLvl[s] = ev.level;
                out.push_back({ onset, s, velFromRing(rings[s], ev.peak) });
            }
        }
        if (getParam("qauto") > 0.5f)
        {
            std::vector<char> none;
            quantize(out, none, false);
        }
        setRecHits(std::move(out));
    }

    bool saveMidi(const juce::File& file)
    {
        std::shared_ptr<const std::vector<RecHit>> hits;
        { const juce::SpinLock::ScopedLockType l(hitsLock); hits = recHits; }
        if (hits == nullptr || hits->empty()) return false;

        auto baseNote = [](int syn) { return syn == SYN_SNARE ? 38 : syn == SYN_HAT ? 42 : syn == SYN_OPEN ? 46
                                           : syn == SYN_TOM ? 45 : 36; };
        int note[MAXD]; bool used[128] = {};
        for (int s = 0; s < MAXD; ++s)
        {
            int nt = baseNote(slotSynth(s));
            while (nt < 127 && used[nt]) ++nt;
            used[nt] = true; note[s] = nt;
        }

        const int ppq = 960;
        const double bpm = recBpm.load() > 20.0 ? recBpm.load() : 120.0;
        juce::MidiMessageSequence seq;
        seq.addEvent(juce::MidiMessage::tempoMetaEvent((int) (60000000.0 / bpm)), 0.0);
        for (const auto& h : *hits)
        {
            if (!slotOn(h.slot) || slotSynth(h.slot) == SYN_IGNORE) continue;
            const double tick = (double) h.sample / sr * bpm / 60.0 * ppq;
            const float hv = hitVolume(h);
            if (hv <= 0.001f) continue;   // muted
            const auto vel = (juce::uint8) juce::jlimit(1, 127, (int) (hv * 127.0f));
            seq.addEvent(juce::MidiMessage::noteOn(10, note[h.slot], vel), tick);
            seq.addEvent(juce::MidiMessage::noteOff(10, note[h.slot]), tick + 120.0);
        }
        seq.sort();
        seq.updateMatchedPairs();
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote(ppq);
        mf.addTrack(seq);
        file.deleteFile();
        if (auto os = file.createOutputStream()) return mf.writeTo(*os);
        return false;
    }
    juce::File exportTempMidi()
    {
        auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("BeatboxToDrums_recording.mid");
        return saveMidi(f) ? f : juce::File();
    }

    // render the hit list with the drum sounds only (the beatbox voice is replaced), as a mono 24-bit WAV
    bool saveAudio(const juce::File& file)
    {
        const auto hits = getRecHits();
        if (hits == nullptr || hits->empty()) return false;

        bool on[MAXD]; float lvl[MAXD]; int syn[MAXD], choke[MAXD];
        std::shared_ptr<const Sample> smp[MAXD];
        for (int s = 0; s < MAXD; ++s)
        {
            on[s] = pOn[s]->load() > 0.5f;
            lvl[s] = juce::Decibels::decibelsToGain(pLvl[s]->load());
            syn[s] = (int) pSyn[s]->load();
            choke[s] = (int) pChoke[s]->load();
        }
        { const juce::SpinLock::ScopedLockType l(sampleLock); for (int s = 0; s < MAXD; ++s) smp[s] = samples[s]; }

        const juce::int64 end = juce::jmax<juce::int64>(recLen.load(), hits->back().sample + (juce::int64) (2.0 * sr));
        std::vector<float> out((size_t) end, 0.0f);
        Voice vs[MAXD];
        juce::Random r;
        size_t idx = 0;
        for (juce::int64 i = 0; i < end; ++i)
        {
            while (idx < hits->size() && (*hits)[idx].sample <= i)
            {
                const auto& h = (*hits)[idx++];
                if (!on[h.slot] || syn[h.slot] == SYN_IGNORE) continue;
                const int grp = chokeGroupOf(choke[h.slot], syn[h.slot]);
                if (grp >= 0)
                    for (int o = 0; o < MAXD; ++o)
                        if (o != h.slot && chokeGroupOf(choke[o], syn[o]) == grp) vs[o].on = false;
                const float hv = hitVolume(h);
                if (hv <= 0.001f) continue;   // muted
                auto& v = vs[h.slot];
                v.on = true; v.t = 0; v.ph = 0; v.prev = 0; v.pos = 0; v.vel = hv; v.smp = smp[h.slot];
            }
            float d = 0;
            for (int s = 0; s < MAXD; ++s)
                if (vs[s].on) d += DrumSynth::render(vs[s], syn[s], sr, r) * lvl[s];
            out[(size_t) i] = std::tanh(d);
        }

        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
        if (os == nullptr) return false;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), sr, 1, 24, {}, 0));
        if (w == nullptr) return false;
        os.release();   // the writer owns the stream now
        const float* ch[1] = { out.data() };
        const bool ok = w->writeFromFloatArrays(ch, 1, (int) out.size());
        w.reset();
        return ok;
    }
    juce::File exportTempAudio()
    {
        lastAudioExport.deleteFile();   // may fail if FL still holds it; harmless
        auto f = juce::File::getSpecialLocation(juce::File::tempDirectory)
                     .getChildFile("BeatboxToDrums_drums_" + juce::String(juce::Time::getMillisecondCounter()) + ".wav");
        lastAudioExport = saveAudio(f) ? f : juce::File();
        return lastAudioExport;
    }

    //---- auto-save: the kit is kept in the user's app-data folder so training survives restarts
    static juce::File autoSaveFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("BeatboxToDrums").getChildFile("autosave.bbkit");
    }
    void markDirty()
    {
        lastChangeMs.store(juce::Time::getMillisecondCounter());
        dirty.store(true);
    }
    // message thread; waits until changes have settled for a moment unless forced
    void autoSaveIfDirty(bool force = false)
    {
        if (!dirty.load()) return;
        if (!force && juce::Time::getMillisecondCounter() - lastChangeMs.load() < 1500) return;
        dirty.store(false);
        const auto f = autoSaveFile();
        f.getParentDirectory().createDirectory();
        saving.store(true);
        saveKit(f);
        saving.store(false);
    }

    //---- tempo / metronome / quantize
    bool tempoFromHost() const { return getParam("hostsync") > 0.5f && hostBpmValid.load(); }
    double getTempo() const { return tempoFromHost() ? hostBpm.load() : (double) getParam("bpm"); }
    double getRecBpm() const { return recBpm.load(); }
    bool isCountingIn() const { return countInLeft.load() > 0; }

    // snap `hits` (sorted, with parallel selection flags) to the grid using the take's tempo and the quantize settings
    void quantize(std::vector<RecHit>& hits, std::vector<char>& sel, bool onlySelected) const
    {
        Quantizer::apply(hits, sel, recBpm.load(), sr, (int) getParam("qdiv"), getParam("qstr") / 100.0f, onlySelected);
    }

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> levelDb { -100.0f }, gateDb { -100.0f }, lastDist { 0.0f };
    std::atomic<int> hitCount[MAXD];

    //==========================================================================
    void prepareToPlay(double sampleRate, int) override
    {
        sr = sampleRate;
        live.prepare(sr);
        metro.prepare(sr);
        dryBuf.assign((size_t) live.win, 0.0f);
        dryPos = 0;
        setLatencySamples(live.win);
        if (recBuf.size() != (size_t) (30.0 * sr))   // 30 s max recording
        {
            recording.store(false);
            recLen.store(0);
            recBuf.assign((size_t) (30.0 * sr), 0.0f);
            recId.store(recIdMax.fetch_add(1) + 1);
        }
        {
            const juce::SpinLock::ScopedLockType l(tmplLock);
            buildPresets();
            rebuildNorm();
        }
        for (auto& v : voices) v.on = false;
        for (auto& t : lastHitAt) t = -(1LL << 40);
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int nCh = buf.getNumChannels(), n = buf.getNumSamples();
        auto P = [this](const char* id) { return apvts.getRawParameterValue(id)->load(); };

        const bool active = P("active") > 0.5f;
        const bool autoNoise = P("noiseauto") > 0.5f;
        const bool guard = P("guard") > 0.5f;
        const float userThr = juce::Decibels::decibelsToGain(P("thresh"));
        const float sens = P("sens");
        const float marginLin = juce::Decibels::decibelsToGain(P("nmargin"));
        const float matchLimit = P("match");
        const float mix = P("mix");
        const bool metroOn = P("metro") > 0.5f;
        const float clickGain = juce::Decibels::decibelsToGain(P("metrovol"));
        const double beatSamp = sr * 60.0 / juce::jmax(20.0, recBpm.load());

        if (auto* ph = getPlayHead())
            if (const auto pos = ph->getPosition())
                if (const auto b = pos->getBpm()) { hostBpm.store(*b); hostBpmValid.store(true); }

        bool on[MAXD]; float lvl[MAXD], lo[MAXD], hi[MAXD]; int syn[MAXD];
        for (int s = 0; s < MAXD; ++s)
        {
            on[s] = pOn[s]->load() > 0.5f;
            lvl[s] = juce::Decibels::decibelsToGain(pLvl[s]->load());
            lo[s] = pLo[s]->load(); hi[s] = pHi[s]->load();
            syn[s] = (int) pSyn[s]->load();
        }
        const int trainTarget = trainSlot.load();
        const float sensEff = sens;   // same onset rules while training, so junk isn't learned
        const float learnedThr = learned.load() * 1.5f;

        if (stopReq.exchange(false)) { playing = false; playingA.store(false); }
        const int hver = hitsVersion.load();
        if (playReq.exchange(false))
        {
            const juce::SpinLock::ScopedTryLockType l(hitsLock);
            if (l.isLocked() && recHits != nullptr && !recHits->empty())
            {
                playHits = recHits; playVer = hver; playing = true; playingA.store(true);
                playPos = juce::jmax<juce::int64>(0, playFrom.load());
                playIdx = firstHitAt(*playHits, playPos);
                lastBeat = playPos == 0 ? -1 : (juce::int64) std::floor((double) playPos / beatSamp);
            }
        }
        else if (playing && hver != playVer)   // hits were edited while playing
        {
            const juce::SpinLock::ScopedTryLockType l(hitsLock);
            if (l.isLocked() && recHits != nullptr)
            {
                playHits = recHits; playVer = hver;
                playIdx = firstHitAt(*playHits, playPos);
            }
        }
        if (audStopReq.exchange(false)) { audLeft = 0; audOrigA.store(false); }
        if (audReq.exchange(false)) { audPos = audReqStart.load(); audLeft = audReqLen.load(); audOrigA.store(audReqWhole.load()); }

        for (int i = 0; i < n; ++i)
        {
            float x = 0;
            for (int c = 0; c < nCh; ++c) x += buf.getReadPointer(c)[i];
            x /= (float) juce::jmax(1, nCh);

            scope[(size_t) ((scopeWriteLocal++) & (scopeSize - 1))].store(x, std::memory_order_relaxed);

            juce::int64 clickPos = -1;   // where we are on the take's timeline, if a metronome click may sound
            if (recording.load(std::memory_order_relaxed))
            {
                const int w = recLen.load(std::memory_order_relaxed);
                if (w == 0) lastBeat = -1;   // first click of a take always sounds
                if (w < (int) recBuf.size()) { recBuf[(size_t) w] = x; recLen.store(w + 1, std::memory_order_release); clickPos = w; }
                else { recording.store(false); autoStopped.store(true); }
            }
            const int cil = countInLeft.load(std::memory_order_relaxed);
            if (cil > 0)   // count-in: recording starts on the sample after the last click
            {
                clickPos = (juce::int64) countInTotalA.load(std::memory_order_relaxed) - cil;
                countInLeft.store(cil - 1, std::memory_order_relaxed);
                if (cil == 1) recording.store(true);
            }

            const float dry = dryBuf[(size_t) dryPos];
            dryBuf[(size_t) dryPos] = x;
            dryPos = (dryPos + 1) % (int) dryBuf.size();

            const int ll = learnLeft.load(std::memory_order_relaxed);
            Detector::Event ev;
            const bool hit = live.push(x, active && ll <= 0, userThr, autoNoise, marginLin, learnedThr, sensEff, ev);
            if (ll > 0)
            {
                learnPeak = juce::jmax(learnPeak, live.fast);
                if (ll == 1) learned.store(learnPeak);
                learnLeft.store(ll - 1, std::memory_order_relaxed);
            }
            ++sampleClock;
            if (hit) handleHit(ev, on, lo, hi, trainTarget, matchLimit, guard);

            if (playing)
            {
                const auto& hv = *playHits;
                while (playIdx < hv.size() && hv[playIdx].sample <= playPos)
                {
                    if (on[hv[playIdx].slot]) fire(hv[playIdx].slot, hitVolume(hv[playIdx]));
                    ++playIdx;
                }
                if (clickPos < 0) clickPos = playPos;
                ++playPos;
                if (playIdx >= hv.size() && (hv.empty() || playPos > hv.back().sample + (juce::int64) (1.5 * sr)))
                {
                    playing = false; playingA.store(false);
                }
            }

            if (metroOn && clickPos >= 0)
            {
                const auto b = (juce::int64) std::floor((double) clickPos / beatSamp);
                if (b != lastBeat) { lastBeat = b; metro.trigger((b & 3) == 0); }
            }
            const float click = metro.next(clickGain);

            float aud = 0;
            if (audLeft > 0)
            {
                aud = recBuf[(size_t) audPos++] * juce::jmin(1.0f, (float) audLeft / 200.0f);   // short fade-out
                --audLeft;
            }

            // render drums
            float d = 0;
            for (int s = 0; s < MAXD; ++s)
            {
                auto& v = voices[s];
                if (!v.on) continue;
                const float o = DrumSynth::render(v, syn[s], sr, rng);
                d += o * lvl[s];
            }

            const float out = dry * (1.0f - mix) + std::tanh(d) * mix + aud + click;
            for (int c = 0; c < nCh; ++c) buf.getWritePointer(c)[i] = out;
        }

        if (audLeft <= 0) audOrigA.store(false);
        audPosA.store(audPos);
        playPosA.store(playPos);
        scopeWrite.store(scopeWriteLocal & (scopeSize - 1));
        levelDb.store(juce::Decibels::gainToDecibels(live.fast, -100.0f));
        gateDb.store(juce::Decibels::gainToDecibels(live.lastNoiseThr, -100.0f));
    }

private:
    //---- undo internals (message thread)
    juce::uint64 undoSignature() const
    {
        juce::uint64 h = 1469598103934665603ULL;
        auto mix = [&h](juce::uint64 v) { h ^= v; h *= 1099511628211ULL; };
        for (auto* p : getParameters())
        {
            const float v = p->getValue();
            juce::uint32 b = 0;
            std::memcpy(&b, &v, sizeof(b));
            mix(b);
        }
        mix((juce::uint64) hitsVersion.load());
        mix((juce::uint64) recId.load());
        mix((juce::uint64) trainVersion.load());
        for (int s = 0; s < MAXD; ++s)
        {
            mix((juce::uint64) apvts.state.getProperty(key(s, "name")).toString().hashCode64());
            mix((juce::uint64) apvts.state.getProperty(key(s, "path")).toString().hashCode64());
        }
        return h;
    }
    UndoSnapshot capture()
    {
        UndoSnapshot s;
        syncTemplatesToState();
        if (auto xml = apvts.copyState().createXml())
            s.kitXml = xml->toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
        s.hits = getRecHits();
        s.recId = recId.load();
        s.recLen = recLen.load();
        s.bpm = recBpm.load();
        s.rate = sr;
        if (undoAudio != nullptr && undoAudioId == s.recId && (int) undoAudio->size() == s.recLen) s.audio = undoAudio;
        else if (s.recLen > 0 && (size_t) s.recLen <= recBuf.size())
        {
            undoAudio = std::make_shared<const std::vector<float>>(recBuf.begin(), recBuf.begin() + s.recLen);
            undoAudioId = s.recId;
            s.audio = undoAudio;
        }
        return s;
    }
    void applySnapshot(const UndoSnapshot& s)
    {
        stopPlayback();
        stopOriginal();
        if (s.rate == sr && (s.recId != recId.load() || s.recLen != recLen.load()))
        {
            const int n = s.audio != nullptr ? (int) juce::jmin(s.audio->size(), recBuf.size()) : 0;
            if (n > 0) std::copy(s.audio->begin(), s.audio->begin() + n, recBuf.begin());
            recLen.store(n, std::memory_order_release);
            recBpm.store(s.bpm);
            recId.store(s.recId);
        }
        liveHitN.store(0);
        setRecHits(s.hits != nullptr ? *s.hits : std::vector<RecHit>());
        if (auto xml = juce::XmlDocument::parse(s.kitXml)) restore(*xml, true);
    }
    void afterUndoRedo(const UndoSnapshot& s)
    {
        base = s;
        haveBase = true;
        inBurst = false;
        ignoring = true;
        ignoreStart = juce::Time::getMillisecondCounter();
    }

    // settings / names / sample paths changed -> keep the auto-save up to date
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override
    {
        if (!saving.load()) markDirty();
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout layout()
    {
        using F = juce::AudioParameterFloat;
        using B = juce::AudioParameterBool;
        using R = juce::NormalisableRange<float>;
        juce::AudioProcessorValueTreeState::ParameterLayout l;

        l.add(std::make_unique<B>("active", "Listening", true));
        l.add(std::make_unique<F>("thresh", "Threshold dB", R(-70, 0, 0.1f), -55.0f));
        l.add(std::make_unique<F>("sens", "Onset Sensitivity", R(1.2f, 6, 0.01f), 2.0f));
        l.add(std::make_unique<B>("noiseauto", "Auto Noise Floor", true));
        l.add(std::make_unique<B>("guard", "Breath Guard", false));   // off: it rejects real open hats
        l.add(std::make_unique<F>("nmargin", "Noise Margin dB", R(3, 24, 0.1f), 12.0f));
        l.add(std::make_unique<F>("match", "Match Tolerance", R(0.3f, 3.0f, 0.01f), 1.5f));
        l.add(std::make_unique<F>("mix", "Drums Mix (0=beatbox)", R(0, 1, 0.01f), 1.0f));
        l.add(std::make_unique<B>("fulllvl", "Full Level", false));

        l.add(std::make_unique<B>("metro", "Metronome", true));
        l.add(std::make_unique<B>("countin", "Count-in", true));
        l.add(std::make_unique<F>("metrovol", "Click Level dB", R(-36, 0, 0.1f), -12.0f));
        l.add(std::make_unique<B>("hostsync", "Host Tempo", true));
        l.add(std::make_unique<F>("bpm", "Tempo", R(30, 300, 0.1f), 120.0f));
        l.add(std::make_unique<juce::AudioParameterChoice>("qdiv", "Quantize Grid", Quantizer::divisionNames(), 2));
        l.add(std::make_unique<F>("qstr", "Quantize Strength %", R(0, 100, 1), 100.0f));
        l.add(std::make_unique<B>("qauto", "Auto-Quantize", false));

        for (int s = 0; s < MAXD; ++s)
        {
            const juce::String pre = "d" + juce::String(s) + "_";
            const juce::String nm = "Drum " + juce::String(s + 1) + " ";
            l.add(std::make_unique<B>(pre + "on", nm + "On", s < 3));
            l.add(std::make_unique<F>(pre + "lvl", nm + "Level dB", R(-24, 6, 0.1f), 0.0f));
            l.add(std::make_unique<F>(pre + "lo", nm + "Low Hz", R(20.0f, 20000.0f, 1.0f, 0.3f), 20.0f));
            l.add(std::make_unique<F>(pre + "hi", nm + "High Hz", R(20.0f, 20000.0f, 1.0f, 0.3f), 20000.0f));
            l.add(std::make_unique<juce::AudioParameterChoice>(
                pre + "syn", nm + "Type",
                juce::StringArray { "Kick", "Snare", "Hat", "Open Hat", "Tom", "Ignore" }, s < 3 ? s : SYN_TOM));
            l.add(std::make_unique<juce::AudioParameterChoice>(
                pre + "choke", nm + "Choke Group", chokeNames(), CHOKE_AUTO));
        }
        return l;
    }

    void readSlots(bool* on, float* lo, float* hi)
    {
        for (int s = 0; s < MAXD; ++s)
        {
            on[s] = pOn[s]->load() > 0.5f;
            lo[s] = pLo[s]->load();
            hi[s] = pHi[s]->load();
        }
    }

    void setSampleInternal(int s, std::shared_ptr<const Sample> smp)
    {
        const juce::SpinLock::ScopedLockType l(sampleLock);
        samples[s] = std::move(smp);
    }

    void restore(const juce::XmlElement& xml, bool keepSamples = false)
    {
        juce::String oldPath[MAXD];
        for (int s = 0; s < MAXD; ++s) oldPath[s] = apvts.state.getProperty(key(s, "path")).toString();
        apvts.replaceState(juce::ValueTree::fromXml(xml));
        const bool oldKit = (int) apvts.state.getProperty("fmt", 1) < KIT_FORMAT;
        if (oldKit)   // v1 kits: training can't be converted, and the old gate missed quiet sounds
        {
            for (int s = 0; s < MAXD; ++s) apvts.state.removeProperty(key(s, "tmpl"), nullptr);
            setParam("thresh", -55.0f); setParam("nmargin", 12.0f); setParam("match", 1.5f); setParam("guard", 0.0f);
            apvts.state.setProperty("fmt", KIT_FORMAT, nullptr);
        }
        {
            const juce::SpinLock::ScopedLockType l(tmplLock);
            loadNoise(apvts.state.getProperty("noise_ex").toString());
        }
        for (int s = 0; s < MAXD; ++s)
        {
            loadExamples(s, apvts.state.getProperty(key(s, "ex")).toString());
            const auto path = apvts.state.getProperty(key(s, "path")).toString();
            if (keepSamples && path == oldPath[s]) continue;   // undo / redo: same sample, no need to reload it
            const juce::File f(path);
            if (f.existsAsFile()) loadSample(s, f); else setSampleInternal(s, nullptr);
        }
        trainSlot.store(-1);
        stateVersion.fetch_add(1);
    }
    void syncTemplatesToState()
    {
        for (int s = 0; s < MAXD; ++s)
            apvts.state.setProperty(key(s, "ex"), serialiseExamples(s), nullptr);
        apvts.state.setProperty("noise_ex", serialiseNoise(), nullptr);
        apvts.state.setProperty("fmt", KIT_FORMAT, nullptr);
    }

    // --- training examples & matching (caller holds tmplLock unless noted)
    void addExample(int s, const float* f)
    {
        auto& v = ex[s];
        if ((int) v.size() >= MAXEX) v.erase(v.begin());    // keep the newest; capacity is reserved, no allocation
        Feat e; std::copy(f, f + NF, e.begin());
        v.push_back(e);
        exCount[s].store((int) v.size());
        trainVersion.fetch_add(1);
        rebuildNorm();
    }
    void addNoise(const Feat& f, int origin)
    {
        trainVersion.fetch_add(1);
        if ((int) noiseEx.size() >= MAXNOISE) noiseEx.erase(noiseEx.begin());
        noiseEx.push_back({ f, origin });
        rebuildNorm();
    }

    // scale every feature by its spread across everything learned, so no single feature dominates
    void rebuildNorm()
    {
        double sum[NF] = {}, sq[NF] = {};
        int n = 0;
        auto add = [&](const Feat& f) { for (int b = 0; b < NF; ++b) { sum[b] += f[b]; sq[b] += (double) f[b] * f[b]; } ++n; };
        for (auto& v : ex) for (auto& f : v) add(f);
        for (auto& nz : noiseEx) add(nz.f);
        if (n < 4) for (auto& v : presetEx) for (auto& f : v) add(f);
        for (int b = 0; b < NF; ++b)
        {
            const double mu = n > 0 ? sum[b] / n : 0.0;
            const double sd = n > 1 ? std::sqrt(juce::jmax(0.0, sq[b] / n - mu * mu)) : 1.0;
            normMu[b] = (float) mu;
            normIs[b] = 1.0f / (float) juce::jmax(1.0e-3, sd);
        }
    }

    // the built-in synth sounds, measured like a hit, stand in for drums you haven't trained
    void buildPresets()
    {
        Detector d;
        d.prepare(sr);
        juce::Random r(1234);
        std::vector<float> buf((size_t) d.win);
        for (int syn = 0; syn < 5; ++syn)
        {
            presetEx[syn].clear();
            for (float vel : { 0.4f, 0.7f, 1.0f })
            {
                Voice v; v.on = true; v.vel = vel;
                for (auto& x : buf) x = v.on ? DrumSynth::render(v, syn, sr, r) : 0.0f;
                Detector::Event ev;
                if (d.measure(buf.data(), (int) buf.size(), ev))
                {
                    Feat f; std::copy(ev.f, ev.f + NF, f.begin());
                    presetEx[syn].push_back(f);
                }
            }
        }
    }

    float distance(const float* a, const Feat& b) const
    {
        float acc = 0;
        for (int i = 0; i < NF; ++i) { const float z = (a[i] - b[(size_t) i]) * normIs[i]; acc += z * z; }
        return std::sqrt(acc / (float) NF);
    }

    // k nearest examples vote (closer = stronger vote). Returns the drum, or -1 if the winner is
    // "noise" or even the closest example is further than matchLimit. bestD = closest distance.
    int classify(const Detector::Event& ev, const bool* on, const float* lo, const float* hi, float matchLimit, float& bestD)
    {
        float nd[KNN]; int nl[KNN];
        for (int i = 0; i < KNN; ++i) { nd[i] = 1e9f; nl[i] = -3; }
        auto consider = [&](const Feat& e, int label)
        {
            const float d = distance(ev.f, e);
            if (d >= nd[KNN - 1]) return;
            int i = KNN - 1;
            while (i > 0 && nd[i - 1] > d) { nd[i] = nd[i - 1]; nl[i] = nl[i - 1]; --i; }
            nd[i] = d; nl[i] = label;
        };
        for (int s = 0; s < MAXD; ++s)
        {
            if (!on[s] || ev.centroidHz < lo[s] || ev.centroidHz > hi[s]) continue;   // per-drum Low/High limits
            const int syn = (int) pSyn[s]->load();
            if (!ex[s].empty()) { for (auto& e : ex[s]) consider(e, s); }
            else if (syn >= 0 && syn < 5) for (auto& e : presetEx[syn]) consider(e, s);
        }
        for (auto& nz : noiseEx) consider(nz.f, -2);

        bestD = nd[0];
        if (nl[0] == -3) return -1;
        float votes[MAXD + 1] = {};
        for (int i = 0; i < KNN; ++i)
            if (nl[i] != -3) votes[nl[i] == -2 ? MAXD : nl[i]] += 1.0f / (nd[i] + 1.0e-3f);
        int win = 0;
        for (int i = 1; i <= MAXD; ++i) if (votes[i] > votes[win]) win = i;
        if (win == MAXD || bestD > matchLimit) return -1;
        return win;
    }

    static juce::String featToString(const Feat& f)
    {
        juce::String o;
        for (int b = 0; b < NF; ++b) { if (b) o << ","; o << juce::String(f[(size_t) b], 4); }
        return o;
    }
    static bool featFromTokens(const juce::StringArray& t, int first, Feat& f)
    {
        if (t.size() - first != NF) return false;
        for (int b = 0; b < NF; ++b) f[(size_t) b] = t[first + b].getFloatValue();
        return true;
    }
    juce::String serialiseExamples(int s)   // takes the lock
    {
        const juce::SpinLock::ScopedLockType l(tmplLock);
        juce::StringArray rows;
        for (auto& f : ex[s]) rows.add(featToString(f));
        return rows.joinIntoString(";");
    }
    void loadExamples(int s, const juce::String& str)   // takes the lock
    {
        juce::StringArray rows;
        rows.addTokens(str, ";", "");
        const juce::SpinLock::ScopedLockType l(tmplLock);
        ex[s].clear();
        for (auto& r : rows)
        {
            juce::StringArray t; t.addTokens(r, ",", "");
            Feat f;
            if (featFromTokens(t, 0, f) && (int) ex[s].size() < MAXEX) ex[s].push_back(f);
        }
        exCount[s].store((int) ex[s].size());
        trainVersion.fetch_add(1);
        rebuildNorm();
    }
    juce::String serialiseNoise()   // takes the lock
    {
        const juce::SpinLock::ScopedLockType l(tmplLock);
        juce::StringArray rows;
        for (auto& nz : noiseEx) rows.add(juce::String(nz.origin) + "," + featToString(nz.f));
        return rows.joinIntoString(";");
    }
    void loadNoise(const juce::String& str)
    {
        juce::StringArray rows;
        rows.addTokens(str, ";", "");
        noiseEx.clear();
        trainVersion.fetch_add(1);
        for (auto& r : rows)
        {
            juce::StringArray t; t.addTokens(r, ",", "");
            Feat f;
            if (t.size() > 0 && featFromTokens(t, 1, f) && (int) noiseEx.size() < MAXNOISE)
                noiseEx.push_back({ f, t[0].getIntValue() });
        }
        rebuildNorm();
    }

    // Hit volume is relative to how loud that drum's recent hits were (the loudest of the last 12 plays at
    // 100%, one 18 dB quieter at the minimum), so it works with any microphone level.
    struct VelRing { float db[12] = {}; int n = 0, pos = 0; };
    static float velFromRing(VelRing& r, float peak)
    {
        const float db = juce::Decibels::gainToDecibels(peak, -100.0f);
        r.db[r.pos] = db;
        r.pos = (r.pos + 1) % 12;
        r.n = juce::jmin(r.n + 1, 12);
        float ref = -100.0f;
        for (int i = 0; i < r.n; ++i) ref = juce::jmax(ref, r.db[i]);
        const float t = juce::jlimit(0.0f, 1.0f, 1.0f - (ref - db) / 18.0f);
        return 0.2f + 0.8f * t;
    }
    VelRing velRing[MAXD];

    // The same drum twice in a row is almost always one sound detected twice (a snare's noisy tail):
    // inside RETRIG_MS it is always dropped; inside ECHO_MS it is dropped if clearly quieter than the last one.
    static bool isEcho(juce::int64 gap, float level, float lastLevel, double sampleRate)
    {
        if (gap < (juce::int64) (RETRIG_MS * 0.001 * sampleRate)) return true;
        return gap < (juce::int64) (ECHO_MS * 0.001 * sampleRate) && level < lastLevel * ECHO_RATIO;
    }

    void fire(int s, float vel)
    {
        hitCount[s].fetch_add(1);
        const int syn = (int) pSyn[s]->load();
        if (syn == SYN_IGNORE || vel <= 0.001f) return;
        const int grp = chokeGroupOf((int) pChoke[s]->load(), syn);
        if (grp >= 0)   // cut off the other drums in this drum's choke group
            for (int o = 0; o < MAXD; ++o)
                if (o != s && chokeGroupOf((int) pChoke[o]->load(), (int) pSyn[o]->load()) == grp) voices[o].on = false;
        auto& v = voices[s];
        v.on = true; v.t = 0; v.ph = 0; v.prev = 0; v.pos = 0; v.vel = vel;
        const juce::SpinLock::ScopedTryLockType l(sampleLock);
        if (l.isLocked()) v.smp = samples[s];
    }

    void handleHit(const Detector::Event& ev, const bool* on, const float* lo, const float* hi,
                   int trainTarget, float matchLimit, bool guard)
    {
        if (guard && ev.rising) return;   // slow-rising sound = breathing, not a hit
        if (trainTarget >= 0 && trainTarget < MAXD && on[trainTarget])
        {
            // a much quieter sound right after a training hit is its tail ("b-oom"): learn it as noise
            const bool tail = sampleClock - lastTrainAt < (juce::int64) (0.4 * sr) && ev.level < lastTrainLevel * 0.5f;
            {
                const juce::SpinLock::ScopedTryLockType l(tmplLock);
                if (l.isLocked())
                {
                    Feat f; std::copy(ev.f, ev.f + NF, f.begin());
                    if (tail) addNoise(f, trainTarget); else addExample(trainTarget, f.data());
                    markDirty();
                }
            }
            if (tail) return;
            lastTrainAt = sampleClock; lastTrainLevel = ev.level;
            fire(trainTarget, velFromRing(velRing[trainTarget], ev.peak));
            return;
        }

        int best; float bestD;
        {
            const juce::SpinLock::ScopedTryLockType l(tmplLock);
            if (!l.isLocked()) return;
            best = classify(ev, on, lo, hi, matchLimit, bestD);
        }
        lastDist.store(bestD);
        if (best < 0) return;
        if (isEcho(sampleClock - lastHitAt[best], ev.level, lastHitLevel[best], sr)) return;
        lastHitAt[best] = sampleClock; lastHitLevel[best] = ev.level;
        const float hitVel = velFromRing(velRing[best], ev.peak);
        fire(best, fullLevel() ? 1.0f : hitVel);

        // log it for the live clip view
        if (recording.load(std::memory_order_relaxed) && (int) pSyn[best]->load() != SYN_IGNORE)
        {
            const int n = liveHitN.load(std::memory_order_relaxed);
            if (n < (int) liveHits.size())
            {
                const auto onset = juce::jmax<juce::int64>(0, recLen.load(std::memory_order_relaxed) - live.win);
                liveHits[(size_t) n] = { onset, best, hitVel };
                liveHitN.store(n + 1, std::memory_order_release);
            }
        }
    }

    // features of a window starting at `onset` in the recording, as the detector would have measured them
    bool featuresAt(juce::int64 onset, int len, float* f) const
    {
        Detector d;
        d.prepare(sr);
        Detector::Event ev;
        const int start = (int) juce::jmax<juce::int64>(0, onset - (juce::int64) (0.1 * sr));   // settle the filters
        for (int i = start; i < (int) onset && i < len; ++i)
            d.push(recBuf[(size_t) i], false, 1.0f, false, 1.0f, 0.0f, 100.0f, ev);
        d.startWindow();
        for (juce::int64 i = onset; i < onset + d.win; ++i)
            if (d.push(i < len ? recBuf[(size_t) i] : 0.0f, false, 1.0f, false, 1.0f, 0.0f, 100.0f, ev))
            {
                std::copy(ev.f, ev.f + NF, f);
                return true;
            }
        return false;
    }

    static size_t firstHitAt(const std::vector<RecHit>& hv, juce::int64 pos)
    {
        return (size_t) (std::lower_bound(hv.begin(), hv.end(), pos,
                                          [](const RecHit& h, juce::int64 p) { return h.sample < p; }) - hv.begin());
    }

    void requestAudition(juce::int64 start, juce::int64 end, bool whole)
    {
        const int len = recLen.load();
        const int a = (int) juce::jlimit<juce::int64>(0, len, start), b = (int) juce::jlimit<juce::int64>(0, len, end);
        if (b <= a) return;
        audReqStart.store(a); audReqLen.store(b - a); audReqWhole.store(whole); audReq.store(true);
    }

    juce::Random rng;
    double sr = 44100;
    Detector live;
    int dryPos = 0;
    std::vector<float> dryBuf;

    std::atomic<float>* pOn[MAXD]; std::atomic<float>* pLvl[MAXD]; std::atomic<float>* pLo[MAXD];
    std::atomic<float>* pHi[MAXD]; std::atomic<float>* pSyn[MAXD]; std::atomic<float>* pChoke[MAXD];
    static constexpr int MAXNOISE = 128, KIT_FORMAT = 2, KNN = 3;
    static constexpr double RETRIG_MS = 80.0, ECHO_MS = 220.0;
    static constexpr float ECHO_RATIO = 0.6f;
    juce::int64 lastHitAt[MAXD] = {};
    float lastHitLevel[MAXD] = {};
    struct NoiseEx { Feat f; int origin; };
    std::vector<Feat> ex[MAXD];          // training examples per drum
    std::vector<NoiseEx> noiseEx;        // auto-learned tails ("ignore" sounds)
    std::vector<Feat> presetEx[5];       // synth sounds, used by untrained drums
    float normMu[NF] = {}, normIs[NF] = {};
    juce::int64 sampleClock = 0, lastTrainAt = -(1 << 30);
    float lastTrainLevel = 0;
    Voice voices[MAXD];
    std::shared_ptr<const Sample> samples[MAXD];
    juce::SpinLock tmplLock, sampleLock;
    mutable juce::SpinLock hitsLock;
    std::atomic<int> trainSlot { -1 };
    std::atomic<int> exCount[MAXD];
    std::atomic<int> stateVersion { 0 };

    std::atomic<int> learnLeft { 0 };
    std::atomic<float> learned { 0.0f };
    float learnPeak = 0;

    // recording / playback
    std::vector<float> recBuf;
    std::atomic<int> recLen { 0 };
    std::atomic<bool> recording { false }, autoStopped { false };
    std::atomic<int> recId { 0 }, recIdMax { 0 }, trainVersion { 0 };
    std::vector<UndoSnapshot> undoStack, redoStack;
    UndoSnapshot base;
    bool haveBase = false, inBurst = false, ignoring = false;
    juce::uint64 lastSig = 0;
    juce::uint32 lastChangeAt = 0, ignoreStart = 0;
    std::atomic<bool> undoReset { false };
    std::shared_ptr<const std::vector<float>> undoAudio;
    int undoAudioId = -1;
    static constexpr size_t MAXUNDO = 60;
    std::atomic<double> hostBpm { 120.0 };
    std::atomic<double> recBpm { 120.0 };
    std::shared_ptr<const std::vector<RecHit>> recHits, playHits;
    std::atomic<int> recHitCount { 0 }, hitsVersion { 0 };
    std::array<RecHit, 4096> liveHits {};
    std::atomic<int> liveHitN { 0 };
    std::atomic<bool> playReq { false }, stopReq { false }, playingA { false };
    std::atomic<juce::int64> playFrom { 0 }, playPosA { 0 };
    bool playing = false;
    juce::int64 playPos = 0;
    size_t playIdx = 0;
    int playVer = -1;
    Metronome metro;
    juce::int64 lastBeat = -1;
    std::atomic<int> countInLeft { 0 }, countInTotalA { 0 };
    std::atomic<bool> hostBpmValid { false }, dirty { false }, saving { false };
    std::atomic<juce::uint32> lastChangeMs { 0 };
    juce::File lastAudioExport;
    std::atomic<bool> audReq { false }, audStopReq { false }, audReqWhole { false }, audOrigA { false };
    std::atomic<int> audReqStart { 0 }, audReqLen { 0 };
    int audPos = 0, audLeft = 0;
    std::atomic<int> audPosA { 0 };
    std::atomic<float>* pFull = nullptr;

    static constexpr int scopeSize = 4096;
    std::atomic<float> scope[scopeSize];
    std::atomic<int> scopeWrite { 0 };
    int scopeWriteLocal = 0;
};
