#pragma once
#include "Common.h"
#include "Detector.h"
#include "DrumSynth.h"
#include "Metronome.h"
#include "Quantizer.h"

//==============================================================================
class BeatboxProcessor : public juce::AudioProcessor, private juce::ValueTree::Listener
{
public:
    BeatboxProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          apvts(*this, nullptr, "STATE", layout())
    {
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
            recompute(s);
        }

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
        if (auto xml = getXmlFromBinary(d, n)) restore(*xml);
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
    bool hasPreset(int s) const { return s < 3; }

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
            tm[s] = Template();
            recompute(s);
        }
        exCount[s].store(0);
        markDirty();
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
        recId.fetch_add(1);
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
    void audition(juce::int64 start, juce::int64 end)
    {
        const int len = recLen.load();
        const int a = (int) juce::jlimit<juce::int64>(0, len, start), b = (int) juce::jlimit<juce::int64>(0, len, end);
        if (b <= a) return;
        audReqStart.store(a); audReqLen.store(b - a); audReq.store(true);
    }
    // add corrected hits to their drum's training, so future detection improves
    void teach(const std::vector<RecHit>& hs)
    {
        const int len = recLen.load();
        for (const auto& h : hs)
        {
            float f[NF];
            if (h.slot < 0 || h.slot >= MAXD || !slotOn(h.slot) || !featuresAt(h.sample, len, f)) continue;
            int count;
            {
                const juce::SpinLock::ScopedLockType l(tmplLock);
                auto& t = tm[h.slot];
                for (int b = 0; b < NF; ++b) { t.sum[b] += f[b]; t.sumsq[b] += (double) f[b] * f[b]; }
                ++t.count;
                recompute(h.slot);
                count = t.count;
            }
            exCount[h.slot].store(count);
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
            for (int i = 0; i < len; ++i)
            {
                if (!d.push(recBuf[(size_t) i], true, userThr, autoNoise, marginLin, learnedThr, sens, ev)) continue;
                if (guard && ev.rising) continue;
                float dist;
                const int s = classify(ev.f, on, lo, hi, matchLimit, dist);
                if (s < 0 || slotSynth(s) == SYN_IGNORE) continue;
                juce::int64 onset = (juce::int64) i - d.win + 1;
                if (onset < 0) onset = 0;
                out.push_back({ onset, s, ev.vel });
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
            const auto vel = (juce::uint8) juce::jlimit(1, 127, (int) (h.vel * 127.0f));
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

        bool on[MAXD]; float lvl[MAXD]; int syn[MAXD];
        std::shared_ptr<const Sample> smp[MAXD];
        for (int s = 0; s < MAXD; ++s)
        {
            on[s] = pOn[s]->load() > 0.5f;
            lvl[s] = juce::Decibels::decibelsToGain(pLvl[s]->load());
            syn[s] = (int) pSyn[s]->load();
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
                if (syn[h.slot] == SYN_HAT)
                    for (int o = 0; o < MAXD; ++o)
                        if (o != h.slot && syn[o] == SYN_OPEN) vs[o].on = false;
                auto& v = vs[h.slot];
                v.on = true; v.t = 0; v.ph = 0; v.prev = 0; v.pos = 0; v.vel = h.vel; v.smp = smp[h.slot];
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
            recId.fetch_add(1);
        }
        for (int s = 0; s < MAXD; ++s) recompute(s);
        for (auto& v : voices) v.on = false;
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
        const float sensEff = trainTarget >= 0 ? 1.3f : sens;   // easier onsets while training
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
        if (audReq.exchange(false)) { audPos = audReqStart.load(); audLeft = audReqLen.load(); }

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
            if (hit) handleHit(ev, on, lo, hi, trainTarget, matchLimit, guard);

            if (playing)
            {
                const auto& hv = *playHits;
                while (playIdx < hv.size() && hv[playIdx].sample <= playPos)
                {
                    if (on[hv[playIdx].slot]) fire(hv[playIdx].slot, hv[playIdx].vel);
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

        playPosA.store(playPos);
        scopeWrite.store(scopeWriteLocal & (scopeSize - 1));
        levelDb.store(juce::Decibels::gainToDecibels(live.fast, -100.0f));
        gateDb.store(juce::Decibels::gainToDecibels(live.lastNoiseThr, -100.0f));
    }

private:
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
        l.add(std::make_unique<F>("thresh", "Threshold dB", R(-70, 0, 0.1f), -30.0f));
        l.add(std::make_unique<F>("sens", "Onset Sensitivity", R(1.2f, 6, 0.01f), 2.0f));
        l.add(std::make_unique<B>("noiseauto", "Auto Noise Floor", true));
        l.add(std::make_unique<B>("guard", "Breath Guard", true));
        l.add(std::make_unique<F>("nmargin", "Noise Margin dB", R(3, 24, 0.1f), 9.0f));
        l.add(std::make_unique<F>("match", "Match Tolerance", R(1, 10, 0.1f), 5.0f));
        l.add(std::make_unique<F>("mix", "Drums Mix (0=beatbox)", R(0, 1, 0.01f), 1.0f));

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

    void restore(const juce::XmlElement& xml)
    {
        apvts.replaceState(juce::ValueTree::fromXml(xml));
        for (int s = 0; s < MAXD; ++s)
        {
            loadTemplate(s, apvts.state.getProperty(key(s, "tmpl")).toString());
            const juce::File f(apvts.state.getProperty(key(s, "path")).toString());
            if (f.existsAsFile()) loadSample(s, f); else setSampleInternal(s, nullptr);
        }
        trainSlot.store(-1);
        stateVersion.fetch_add(1);
    }
    void syncTemplatesToState()
    {
        for (int s = 0; s < MAXD; ++s)
            apvts.state.setProperty(key(s, "tmpl"), serialiseTemplate(s), nullptr);
    }

    // --- templates (caller holds tmplLock unless noted)
    void recompute(int s)
    {
        auto& t = tm[s];
        const float sigFloor = 3.0f;
        if (t.count > 0)
        {
            for (int b = 0; b < NF; ++b)
            {
                const double mu = t.sum[b] / t.count;
                const double var = juce::jmax(0.0, t.sumsq[b] / t.count - mu * mu);
                t.mu[b] = (float) mu;
                t.isig[b] = 1.0f / juce::jmax((float) std::sqrt(var), sigFloor);
            }
            t.valid = true;
        }
        else if (s < 3)
        {
            // rough built-in shapes until you train: kick = bass heavy, snare = mid+high, hat = high & short
            float mean = 0;
            for (int b = 0; b < NB; ++b)
            {
                const float fc = 40.0f * std::pow(350.0f, ((float) b + 0.5f) / (float) NB);
                float v;
                if (s == 0) v = juce::jlimit(-30.0f, 30.0f, -9.0f * std::log2(fc / 200.0f));
                else if (s == 1)
                {
                    const float a1 = std::log2(fc / 250.0f) / 0.8f, a2 = std::log2(fc / 5000.0f) / 1.5f;
                    v = 8.0f * std::exp(-a1 * a1) + 8.0f * std::exp(-a2 * a2);
                }
                else v = juce::jlimit(-30.0f, 30.0f, 7.0f * std::log2(fc / 2500.0f));
                t.mu[b] = v; mean += v;
            }
            mean /= (float) NB;
            for (int b = 0; b < NB; ++b) { t.mu[b] -= mean; t.isig[b] = 1.0f / 8.0f; }
            t.mu[NB] = s == 0 ? -8.0f : (s == 1 ? -10.0f : -20.0f);
            t.isig[NB] = 1.0f / 8.0f;
            t.valid = true;
        }
        else t.valid = false;
    }

    juce::String serialiseTemplate(int s)
    {
        const juce::SpinLock::ScopedLockType l(tmplLock);
        const auto& t = tm[s];
        juce::String out(t.count);
        for (int b = 0; b < NF; ++b) out << "," << t.sum[b];
        for (int b = 0; b < NF; ++b) out << "," << t.sumsq[b];
        return out;
    }
    void loadTemplate(int s, const juce::String& str)
    {
        juce::StringArray tok;
        tok.addTokens(str, ",", "");
        int count;
        {
            const juce::SpinLock::ScopedLockType l(tmplLock);
            tm[s] = Template();
            if (tok.size() == 1 + 2 * NF)
            {
                tm[s].count = tok[0].getIntValue();
                for (int b = 0; b < NF; ++b)
                {
                    tm[s].sum[b] = tok[1 + b].getDoubleValue();
                    tm[s].sumsq[b] = tok[1 + NF + b].getDoubleValue();
                }
            }
            recompute(s);
            count = tm[s].count;
        }
        exCount[s].store(count);
    }

    // closest trained drum (caller holds tmplLock). Returns -1 if nothing within matchLimit.
    int classify(const float* f, const bool* on, const float* lo, const float* hi, float matchLimit, float& bestD)
    {
        int best = -1; bestD = 1e9f;
        for (int s = 0; s < MAXD; ++s)
        {
            if (!on[s] || !tm[s].valid) continue;
            float acc = 0, cnt = 0; int bands = 0;
            for (int b = 0; b < NB; ++b)
            {
                if (bandCenter(b) < lo[s] || bandCenter(b) > hi[s]) continue;
                const float z = (f[b] - tm[s].mu[b]) * tm[s].isig[b];
                acc += z * z; cnt += 1.0f; ++bands;
            }
            if (bands == 0) continue;
            const float zd = (f[NB] - tm[s].mu[NB]) * tm[s].isig[NB];
            acc += 3.0f * zd * zd; cnt += 3.0f;     // decay (short vs sustained) counts extra
            const float dist = std::sqrt(acc / cnt);
            if (dist < bestD) { bestD = dist; best = s; }
        }
        return (best >= 0 && bestD <= matchLimit) ? best : -1;
    }
    static float bandCenter(int b)
    {
        const float ratio = std::pow(14000.0f / 40.0f, 1.0f / (float) NB);
        return 40.0f * std::pow(ratio, (float) b + 0.5f);
    }

    void fire(int s, float vel)
    {
        hitCount[s].fetch_add(1);
        const int syn = (int) pSyn[s]->load();
        if (syn == SYN_IGNORE) return;
        if (syn == SYN_HAT)   // closed hat chokes open hats
            for (int o = 0; o < MAXD; ++o)
                if (o != s && (int) pSyn[o]->load() == SYN_OPEN) voices[o].on = false;
        auto& v = voices[s];
        v.on = true; v.t = 0; v.ph = 0; v.prev = 0; v.pos = 0; v.vel = vel;
        const juce::SpinLock::ScopedTryLockType l(sampleLock);
        if (l.isLocked()) v.smp = samples[s];
    }

    void handleHit(const Detector::Event& ev, const bool* on, const float* lo, const float* hi,
                   int trainTarget, float matchLimit, bool guard)
    {
        if (trainTarget >= 0 && trainTarget < MAXD && on[trainTarget])
        {
            {
                const juce::SpinLock::ScopedTryLockType l(tmplLock);
                if (l.isLocked())
                {
                    auto& t = tm[trainTarget];
                    for (int b = 0; b < NF; ++b) { t.sum[b] += ev.f[b]; t.sumsq[b] += (double) ev.f[b] * ev.f[b]; }
                    ++t.count;
                    recompute(trainTarget);
                    exCount[trainTarget].store(t.count);
                    markDirty();
                }
            }
            fire(trainTarget, ev.vel);
            return;
        }
        if (guard && ev.rising) return;   // slow-rising sound = breathing, not a hit

        int best; float bestD;
        {
            const juce::SpinLock::ScopedTryLockType l(tmplLock);
            if (!l.isLocked()) return;
            best = classify(ev.f, on, lo, hi, matchLimit, bestD);
        }
        lastDist.store(bestD);
        if (best < 0) return;
        fire(best, ev.vel);

        // log it for the live clip view
        if (recording.load(std::memory_order_relaxed) && (int) pSyn[best]->load() != SYN_IGNORE)
        {
            const int n = liveHitN.load(std::memory_order_relaxed);
            if (n < (int) liveHits.size())
            {
                const auto onset = juce::jmax<juce::int64>(0, recLen.load(std::memory_order_relaxed) - live.win);
                liveHits[(size_t) n] = { onset, best, ev.vel };
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

    juce::Random rng;
    double sr = 44100;
    Detector live;
    int dryPos = 0;
    std::vector<float> dryBuf;

    std::atomic<float>* pOn[MAXD]; std::atomic<float>* pLvl[MAXD]; std::atomic<float>* pLo[MAXD];
    std::atomic<float>* pHi[MAXD]; std::atomic<float>* pSyn[MAXD];
    Template tm[MAXD];
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
    std::atomic<int> recId { 0 };
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
    std::atomic<bool> audReq { false };
    std::atomic<int> audReqStart { 0 }, audReqLen { 0 };
    int audPos = 0, audLeft = 0;

    static constexpr int scopeSize = 4096;
    std::atomic<float> scope[scopeSize];
    std::atomic<int> scopeWrite { 0 };
    int scopeWriteLocal = 0;
};
