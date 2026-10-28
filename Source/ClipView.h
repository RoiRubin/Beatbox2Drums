#pragma once
#include "PluginProcessor.h"

//==============================================================================
// The recorded clip: waveform + detected hits. Scrolls live while recording; afterwards hits can be
// selected (click, shift-click, drag a range), re-assigned to another drum, moved, added or deleted.
class ClipView : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    // drop a beatbox audio file here to use it as the take (instead of recording) and analyse it
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (auto& f : files) if (isAudioFile(juce::File(f))) return true;
        return false;
    }
    void fileDragEnter(const juce::StringArray&, int, int) override { dropHover = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dropHover = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        dropHover = false;
        for (auto& f : files)
            if (isAudioFile(juce::File(f))) { proc.loadRecording(juce::File(f)); break; }
        repaint();
    }

    explicit ClipView(BeatboxProcessor& p) : proc(p)
    {
        setWantsKeyboardFocus(true);
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff14161a));
        const auto head = headerArea(), ruler = rulerArea(), wave = waveArea(), lane = laneArea();
        const int len = proc.getRecLength();
        const bool rec = proc.isRecording();

        // segment tints behind the waveform
        for (int i = 0; i < (int) hits.size(); ++i)
        {
            const float x0 = sampleToX((double) hits[(size_t) i].sample), x1 = sampleToX(segEnd(i, len));
            if (x1 < 0 || x0 > (float) getWidth()) continue;
            g.setColour(hitColour(i).withAlpha(sel[(size_t) i] ? 0.30f : 0.10f));
            g.fillRect(x0, (float) wave.getY(), x1 - x0, (float) wave.getHeight());
        }

        drawGrid(g, wave);

        // waveform (auto-scaled to the loudest part of the take)
        if (len > 0)
        {
            const float* d = proc.getRecData();
            const float gain = 0.95f / juce::jmax(0.02f, peakAbs);
            const float mid = (float) wave.getCentreY(), half = (float) wave.getHeight() * 0.5f;
            g.setColour(juce::Colours::white.withAlpha(0.75f));
            for (int px = 0; px < getWidth(); ++px)
            {
                const double s0 = xToSample((float) px), s1 = xToSample((float) px + 1.0f);
                if (s1 <= 0 || s0 >= len) continue;
                const int a = juce::jlimit(0, len - 1, (int) s0), e = juce::jlimit(a + 1, len, (int) s1);
                float mn = 0, mx = 0;
                if (e - a > 2 * PK)
                    for (int b = a / PK; b <= (e - 1) / PK && b < (int) pkMin.size(); ++b)
                    { mn = juce::jmin(mn, pkMin[(size_t) b]); mx = juce::jmax(mx, pkMax[(size_t) b]); }
                else
                    for (int i = a; i < e; ++i) { mn = juce::jmin(mn, d[i]); mx = juce::jmax(mx, d[i]); }
                g.drawVerticalLine(px, mid - juce::jmin(1.0f, mx * gain) * half, mid - juce::jmax(-1.0f, mn * gain) * half + 1.0f);
            }
            const float xe = sampleToX((double) len);
            if (xe < (float) getWidth())
            {
                g.setColour(juce::Colours::black.withAlpha(0.35f));
                g.fillRect(juce::jmax(0.0f, xe), (float) wave.getY(), (float) getWidth() - juce::jmax(0.0f, xe), (float) wave.getHeight());
            }
        }
        else
        {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.setFont(14.0f);
            g.drawText(proc.isCountingIn() ? "Count-in... get ready" : "Press Record and beatbox - the clip shows up here",
                       wave, juce::Justification::centred);
        }

        // onset lines + lane blocks
        g.setColour(juce::Colour(0xff1f2228));
        g.fillRect(lane);
        g.setFont(12.0f);
        for (int i = 0; i < (int) hits.size(); ++i)
        {
            const auto& h = hits[(size_t) i];
            const float x0 = sampleToX((double) h.sample), x1 = sampleToX(segEnd(i, len));
            if (x1 < 0 || x0 > (float) getWidth()) continue;
            const bool s = sel[(size_t) i] != 0;
            const auto col = hitColour(i);
            g.setColour(col.withAlpha(s ? 1.0f : 0.8f));
            g.fillRect(x0, (float) wave.getY(), s || i == hover ? 2.0f : 1.0f, (float) wave.getHeight());

            const juce::Rectangle<float> r(x0, (float) lane.getY() + 2.0f, juce::jmax(3.0f, x1 - x0 - 1.0f), (float) lane.getHeight() - 4.0f);
            g.setColour(col.withAlpha(s ? 0.95f : (i == hover ? 0.75f : 0.55f)));
            g.fillRoundedRectangle(r, 3.0f);
            if (s)
            {
                g.setColour(juce::Colours::white);
                g.drawRoundedRectangle(r, 3.0f, 1.5f);
            }
            if (r.getWidth() > 28.0f)
            {
                g.setColour(juce::Colours::black.withAlpha(0.85f));
                g.drawText(proc.slotOn(h.slot) ? proc.getSlotName(h.slot) : "(removed)", r.reduced(4, 0),
                           juce::Justification::centredLeft, true);
            }
        }

        // range being dragged
        if (drag == Drag::range && dragged)
        {
            const float a = sampleToX(juce::jmin(rangeA, rangeB)), b = sampleToX(juce::jmax(rangeA, rangeB));
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.fillRect(a, (float) wave.getY(), b - a, (float) (lane.getBottom() - wave.getY()));
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawRect(a, (float) wave.getY(), b - a, (float) (lane.getBottom() - wave.getY()), 1.0f);
        }

        drawVolumeLane(g, len);

        // playhead (drums playback, or the original recording)
        if (proc.isPlaying() || proc.isPlayingOriginal())
        {
            const double pp = proc.isPlaying() ? (double) proc.getPlayPos() : (double) proc.getOriginalPos();
            g.setColour(juce::Colour(0xff3ddc84));
            g.fillRect(sampleToX(pp) - 1.0f, (float) ruler.getY(), 2.0f, (float) (lane.getBottom() - ruler.getY()));
        }

        drawRuler(g, ruler);

        // header: drum legend with its number key, plus help / record status
        g.setColour(juce::Colour(0xff1f2228));
        g.fillRect(head);
        g.setFont(12.0f);
        if (rec)
        {
            g.setColour(juce::Colour(0xffe5484d));
            g.drawText(juce::String::fromUTF8("\xe2\x97\x8f REC  ") + juce::String(proc.getRecSeconds(), 1) + " s",
                       head.reduced(6, 0), juce::Justification::centredRight);
        }
        else if (len > 0)
        {
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawText("Click / drag: select   Right-click or 1-" + juce::String(juce::jmax(1, activeCount()))
                           + ": set drum   Del: delete   Drag block: move   Q: quantize   Ctrl+Z: undo   Wheel: zoom   Click ruler: play   Drag bottom bars: volume",
                       head.reduced(6, 0), juce::Justification::centredRight, true);
        }
        int x = head.getX() + 6, n = 0;
        for (int s = 0; s < MAXD; ++s)
        {
            if (!proc.slotOn(s)) continue;
            const auto txt = juce::String(++n) + " " + proc.getSlotName(s);
            const int w = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), txt) + 18;
            g.setColour(slotColour(s));
            g.fillEllipse((float) x, (float) head.getCentreY() - 4.0f, 8.0f, 8.0f);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawText(txt, x + 11, head.getY(), w - 11, head.getHeight(), juce::Justification::centredLeft);
            x += w;
        }

        g.setColour(juce::Colours::white.withAlpha(hasKeyboardFocus(false) ? 0.35f : 0.12f));
        g.drawRect(getLocalBounds());
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        drag = Drag::none;
        dragged = false;
        if (proc.isRecording() || proc.getRecLength() == 0) return;
        const auto p = e.position;
        const double s = xToSample(p.x);

        if (e.mods.isPopupMenu())
        {
            const int h = hitAt(p);
            if (h >= 0 && !sel[(size_t) h]) selectOnly(h);
            showMenu(s);
            return;
        }
        if (p.y < (float) waveArea().getY())   // ruler: play from here
        {
            proc.startPlayback((juce::int64) juce::jmax(0.0, s));
            return;
        }

        if (volArea().toFloat().contains(p))   // volume lane: select the segment; dragging sets its volume
        {
            const int v = segmentAt(s);
            if (v < 0) return;
            if (e.mods.isShiftDown() || e.mods.isCommandDown()) sel[(size_t) v] = !sel[(size_t) v];
            else if (!sel[(size_t) v]) selectOnly(v);
            if (sel[(size_t) v]) { drag = Drag::vol; dragOrig = hits; }
            repaint();
            return;
        }

        const int g = grabAt(p);
        if (g >= 0)
        {
            if (e.mods.isShiftDown() || e.mods.isCommandDown()) sel[(size_t) g] = !sel[(size_t) g];
            else if (!sel[(size_t) g]) selectOnly(g);
            if (sel[(size_t) g])
            {
                audition(g);
                drag = Drag::move;
                dragOrig = hits;
                dragSel = sel;
                dragStart = s;
            }
        }
        else
        {
            drag = Drag::range;
            rangeA = rangeB = s;
            rangeBase = e.mods.isShiftDown() || e.mods.isCommandDown() ? sel : std::vector<char>(hits.size(), 0);
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (drag == Drag::none) return;
        if (!dragged && e.getDistanceFromDragStart() < 3) return;
        dragged = true;
        if (drag == Drag::vol)
        {
            const auto vl = volArea();
            const float v = std::round(juce::jlimit(0.0f, 1.0f, (float) (vl.getBottom() - e.position.y) / (float) vl.getHeight()) * 100.0f) / 100.0f;
            for (size_t i = 0; i < hits.size(); ++i)
                if (sel[i]) hits[i].vol = v;
            publish();   // plays with the new volume straight away
            return;
        }
        const double s = xToSample(e.position.x);
        if (drag == Drag::range)
        {
            rangeB = s;
            const double a = juce::jmin(rangeA, rangeB), b = juce::jmax(rangeA, rangeB);
            for (size_t i = 0; i < hits.size(); ++i)
                sel[i] = rangeBase[i] || ((double) hits[i].sample >= a && (double) hits[i].sample <= b);
        }
        else if (drag == Drag::move)
        {
            const auto delta = (juce::int64) (s - dragStart);
            const juce::int64 last = juce::jmax(0, proc.getRecLength() - 1);
            hits = dragOrig;
            sel = dragSel;
            for (size_t i = 0; i < hits.size(); ++i)
                if (sel[i]) hits[i].sample = juce::jlimit<juce::int64>(0, last, hits[i].sample + delta);
            sortHits();
        }
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (drag == Drag::range && !dragged)   // plain click in the waveform: pick the segment under it
        {
            const int h = hitAt(e.position);
            if (h < 0) sel.assign(hits.size(), 0);
            else if (e.mods.isShiftDown() || e.mods.isCommandDown()) sel[(size_t) h] = !sel[(size_t) h];
            else selectOnly(h);
            if (h >= 0 && sel[(size_t) h]) audition(h);
        }
        else if ((drag == Drag::move || drag == Drag::vol) && dragged)
        {
            publish();
        }
        drag = Drag::none;
        dragged = false;
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const int h = proc.isRecording() ? -1 : hitAt(e.position);
        const bool inVol = !proc.isRecording() && volArea().toFloat().contains(e.position) && segmentAt(xToSample(e.position.x)) >= 0;
        setMouseCursor(inVol ? juce::MouseCursor::UpDownResizeCursor
                             : (!proc.isRecording() && grabAt(e.position) >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                                                               : juce::MouseCursor::NormalCursor));
        if (h != hover) { hover = h; repaint(); }
    }
    void mouseExit(const juce::MouseEvent&) override { hover = -1; repaint(); }

    void paintOverChildren(juce::Graphics& g) override
    {
        if (!dropHover) return;
        g.setColour(juce::Colour(0xff2d6cdf).withAlpha(0.25f));
        g.fillRect(getLocalBounds());
        g.setColour(juce::Colours::white);
        g.setFont(15.0f);
        g.drawText("Drop to use this audio as the take", getLocalBounds(), juce::Justification::centred);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (proc.isRecording()) return;
        if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY))
        {
            const float d = std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : w.deltaY;
            viewStart -= (double) d * viewLen * 0.5;
        }
        else
        {
            const double anchor = xToSample(e.position.x);
            viewLen *= std::pow(2.0, -(double) w.deltaY * 4.0);
            clampView();
            viewStart = anchor - (double) e.position.x / juce::jmax(1, getWidth()) * viewLen;
        }
        clampView();
        repaint();
    }

    bool keyPressed(const juce::KeyPress& k) override
    {
        if (proc.isRecording()) return false;
        const auto mods = k.getModifiers();
        const int code = (int) juce::CharacterFunctions::toUpperCase((juce::juce_wchar) k.getKeyCode());
        if (mods.isCommandDown())
        {
            if (code == 'Z') { if (mods.isShiftDown()) redo(); else undo(); return true; }
            if (code == 'Y') { redo(); return true; }
            if (code == 'A') { sel.assign(hits.size(), 1); repaint(); return true; }
            return false;
        }
        if (code == 'Q') { quantize(); return true; }
        if (code >= '1' && code <= '8')
        {
            const int s = nthActiveSlot(code - '1');
            if (s >= 0) assignSelected(s);
            return true;
        }
        if (k.isKeyCode(juce::KeyPress::deleteKey) || k.isKeyCode(juce::KeyPress::backspaceKey)) { deleteSelected(); return true; }
        if (k.isKeyCode(juce::KeyPress::escapeKey)) { sel.assign(hits.size(), 0); repaint(); return true; }
        return false;
    }
    // snap the selected hits (or all of them, if none are selected) to the grid
    void quantize()
    {
        if (proc.isRecording() || hits.empty()) return;
        proc.quantize(hits, sel, selectedCount() > 0);
        hover = -1;
        publish();
    }

    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }

private:
    static constexpr int PK = 128;   // samples per cached waveform peak
    enum class Drag { none, range, move, vol };
    static constexpr int VOLH = 44;   // height of the volume lane

    juce::Rectangle<int> headerArea() const { return getLocalBounds().removeFromTop(18); }
    juce::Rectangle<int> rulerArea() const { return getLocalBounds().withTrimmedTop(18).removeFromTop(14); }
    juce::Rectangle<int> laneArea() const { return getLocalBounds().removeFromBottom(24); }
    int volH() const { return juce::jlimit(24, VOLH, (getHeight() - 56) / 3); }   // shrinks when the clip is short
    juce::Rectangle<int> volArea() const { return getLocalBounds().withTrimmedBottom(24).removeFromBottom(volH()); }
    juce::Rectangle<int> waveArea() const { return getLocalBounds().withTrimmedTop(32).withTrimmedBottom(24 + volH()); }

    double xToSample(float x) const { return viewStart + (double) x / juce::jmax(1, getWidth()) * viewLen; }
    float sampleToX(double s) const { return (float) ((s - viewStart) / viewLen * getWidth()); }

    juce::Colour hitColour(int i) const
    {
        const int s = hits[(size_t) i].slot;
        return proc.slotOn(s) ? slotColour(s) : juce::Colour(0xff7a7f87);
    }
    // a hit's segment runs to the next hit (at most 0.5 s)
    double segEnd(int i, int len) const
    {
        const double start = (double) hits[(size_t) i].sample;
        double e = start + 0.5 * proc.getRate();
        if (i + 1 < (int) hits.size()) e = juce::jmin(e, (double) hits[(size_t) i + 1].sample);
        return juce::jmax(start + 1.0, juce::jmin(e, (double) len));
    }

    // hit whose onset line or lane block is under p (these can be dragged)
    int grabAt(juce::Point<float> p) const
    {
        if (laneArea().toFloat().contains(p)) return segmentAt(xToSample(p.x));
        if (!waveArea().toFloat().contains(p)) return -1;
        int best = -1;
        float bd = 5.0f;
        for (int i = 0; i < (int) hits.size(); ++i)
        {
            const float dx = std::abs(sampleToX((double) hits[(size_t) i].sample) - p.x);
            if (dx < bd) { bd = dx; best = i; }
        }
        return best;
    }
    // hit under p anywhere in the waveform / lane
    int hitAt(juce::Point<float> p) const
    {
        if (p.y < (float) waveArea().getY()) return -1;
        const int g = grabAt(p);
        return g >= 0 ? g : segmentAt(xToSample(p.x));
    }
    int segmentAt(double s) const
    {
        const int len = proc.getRecLength();
        for (int i = 0; i < (int) hits.size(); ++i)
            if (s >= (double) hits[(size_t) i].sample && s < segEnd(i, len)) return i;
        return -1;
    }

    int activeCount() const
    {
        int n = 0;
        for (int s = 0; s < MAXD; ++s) n += proc.slotOn(s) ? 1 : 0;
        return n;
    }
    int nthActiveSlot(int n) const
    {
        for (int s = 0; s < MAXD; ++s)
            if (proc.slotOn(s) && n-- == 0) return s;
        return -1;
    }

    void selectOnly(int i) { sel.assign(hits.size(), 0); sel[(size_t) i] = 1; }
    int selectedCount() const { return (int) std::count(sel.begin(), sel.end(), (char) 1); }

    void audition(int i) { proc.audition(hits[(size_t) i].sample, (juce::int64) segEnd(i, proc.getRecLength())); }

    // keep hits sorted by time, carrying the selection along
    void sortHits()
    {
        std::vector<size_t> idx(hits.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [this](size_t a, size_t b) { return hits[a].sample < hits[b].sample; });
        std::vector<RecHit> h;
        std::vector<char> s;
        for (auto i : idx) { h.push_back(hits[i]); s.push_back(sel[i]); }
        hits.swap(h);
        sel.swap(s);
    }

    //---- edits
    void publish()
    {
        proc.setRecHits(hits);
        knownVersion = proc.getHitsVersion();
        repaint();
    }
    void assignSelected(int slot)
    {
        if (selectedCount() == 0) return;
        for (size_t i = 0; i < hits.size(); ++i)
            if (sel[i]) hits[i].slot = slot;
        publish();
    }
    void deleteSelected()
    {
        if (selectedCount() == 0) return;
        std::vector<RecHit> h;
        for (size_t i = 0; i < hits.size(); ++i)
            if (!sel[i]) h.push_back(hits[i]);
        hits.swap(h);
        sel.assign(hits.size(), 0);
        hover = -1;
        publish();
    }
    void addHit(double s, int slot)
    {
        hits.push_back({ (juce::int64) juce::jmax(0.0, s), slot, 0.8f });
        sel.assign(hits.size(), 0);
        sel.back() = 1;
        sortHits();
        publish();
    }
    // undo / redo cover everything in the plugin, not just the clip (see BeatboxProcessor::undoTick)
    void undo() { proc.undoStep(); }
    void redo() { proc.redoStep(); }

    void showMenu(double s)
    {
        const int nSel = selectedCount();
        const int len = proc.getRecLength();
        juce::PopupMenu m, add;
        if (nSel > 0)
        {
            m.addSectionHeader(juce::String(nSel) + (nSel == 1 ? " hit selected" : " hits selected"));
            for (int d = 0, k = 1; d < MAXD; ++d)
            {
                if (!proc.slotOn(d)) continue;
                bool all = true;
                for (size_t i = 0; i < hits.size(); ++i)
                    if (sel[i] && hits[i].slot != d) all = false;
                m.addColouredItem(100 + d, "Make it " + proc.getSlotName(d) + "   (" + juce::String(k++) + ")",
                                  slotColour(d), true, all);
            }
            m.addItem(1, "Delete   (Del)");
            m.addItem(3, "Listen");
            m.addItem(9, "Reset volume to automatic");
            m.addItem(2, "Teach these to their drums (improves detection)");
            m.addSeparator();
        }
        for (int d = 0; d < MAXD; ++d)
            if (proc.slotOn(d)) add.addColouredItem(200 + d, proc.getSlotName(d), slotColour(d));
        m.addSubMenu("Add hit here", add, s >= 0 && s < len);
        m.addSeparator();
        m.addItem(4, "Select all   (Ctrl+A)", !hits.empty());
        m.addItem(5, "Undo   (Ctrl+Z)", proc.canUndo());
        m.addItem(6, "Redo   (Ctrl+Y)", proc.canRedo());
        m.addItem(7, "Zoom to fit");
        m.addItem(8, nSel > 0 ? "Quantize selected   (Q)" : "Quantize all   (Q)", !hits.empty());

        juce::Component::SafePointer<ClipView> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options(), [safe, s](int r)
        {
            if (safe == nullptr || r == 0) return;
            safe->menuResult(r, s);
        });
    }
    void menuResult(int r, double s)
    {
        if (proc.isRecording()) return;
        if (r >= 200) { addHit(s, r - 200); return; }
        if (r >= 100) { assignSelected(r - 100); return; }
        switch (r)
        {
            case 1: deleteSelected(); break;
            case 2:
            {
                std::vector<RecHit> t;
                for (size_t i = 0; i < hits.size(); ++i)
                    if (sel[i]) t.push_back(hits[i]);
                proc.teach(t);
                break;
            }
            case 3:
            {
                int a = -1, b = -1;
                for (int i = 0; i < (int) hits.size(); ++i)
                    if (sel[(size_t) i]) { if (a < 0) a = i; b = i; }
                if (a >= 0)
                    proc.audition(hits[(size_t) a].sample,
                                  juce::jmin((juce::int64) segEnd(b, proc.getRecLength()),
                                             hits[(size_t) a].sample + (juce::int64) (4.0 * proc.getRate())));
                break;
            }
            case 9:
                if (selectedCount() > 0)
                {
                    for (size_t i = 0; i < hits.size(); ++i)
                        if (sel[i]) hits[i].vol = -1.0f;
                    publish();
                }
                break;
            case 4: sel.assign(hits.size(), 1); break;
            case 5: undo(); break;
            case 6: redo(); break;
            case 7: zoomToFit(); break;
            case 8: quantize(); break;
            default: break;
        }
        repaint();
    }

    //---- view
    void zoomToFit()
    {
        const double sr = proc.getRate();
        const int len = proc.getRecLength();
        viewStart = 0;
        viewLen = len > 0 ? juce::jmax((double) len * 1.02, 0.5 * sr) : 8.0 * sr;
    }
    void clampView()
    {
        const double sr = proc.getRate();
        const double full = juce::jmax((double) proc.getRecLength() * 1.05, 8.0 * sr);
        viewLen = juce::jlimit(0.05 * sr, full, viewLen);
        viewStart = juce::jlimit(0.0, juce::jmax(0.0, full - viewLen), viewStart);
    }

    // one bar per segment; its height is the volume the drum plays at. Drag a bar to set it by hand.
    void drawVolumeLane(juce::Graphics& g, int len)
    {
        const auto vl = volArea();
        g.setColour(juce::Colour(0xff181b20));
        g.fillRect(vl);
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        for (float f : { 0.25f, 0.5f, 0.75f })
            g.drawHorizontalLine((int) ((float) vl.getBottom() - f * (float) vl.getHeight()), (float) vl.getX(), (float) vl.getRight());
        g.setFont(10.0f);
        g.setColour(juce::Colours::white.withAlpha(0.30f));
        g.drawText(proc.fullLevel() ? "Volume - Full level: automatic hits play at 100%" : "Volume - follows how loud you beatboxed",
                   vl.getRight() - 340, vl.getY() + 1, 336, 12, juce::Justification::centredRight);

        for (int i = 0; i < (int) hits.size(); ++i)
        {
            const auto& h = hits[(size_t) i];
            const float x0 = sampleToX((double) h.sample), x1 = sampleToX(segEnd(i, len));
            if (x1 < 0 || x0 > (float) getWidth()) continue;
            const bool s = sel[(size_t) i] != 0, manual = h.vol >= 0.0f;
            const float v = juce::jlimit(0.0f, 1.0f, proc.hitVolume(h));
            const float bh = v * (float) (vl.getHeight() - 2);
            const juce::Rectangle<float> r(x0, (float) vl.getBottom() - bh, juce::jmax(3.0f, x1 - x0 - 1.0f), bh);
            const auto col = hitColour(i);
            g.setColour(col.withAlpha(manual ? (s ? 0.90f : 0.70f) : (s ? 0.60f : 0.32f)));
            g.fillRect(r);
            g.setColour(manual ? juce::Colours::white : col);
            g.fillRect(r.getX(), r.getY() - 1.0f, r.getWidth(), 2.0f);
            if ((s || i == hover) && r.getWidth() > 30.0f)
            {
                g.setColour(juce::Colours::white.withAlpha(0.9f));
                g.drawText(juce::String(juce::roundToInt(v * 100.0f)) + "%" + (manual ? "" : " auto"),
                           (int) x0 + 3, vl.getY() + 1, (int) r.getWidth() - 4, 12, juce::Justification::centredLeft);
            }
        }
    }

    // tempo grid: bar lines, beat lines and (when zoomed in enough) the quantize steps
    void drawGrid(juce::Graphics& g, juce::Rectangle<int> wave)
    {
        const double sr = proc.getRate(), bpm = proc.getRecBpm();
        if (bpm < 20.0 || (proc.getRecLength() == 0 && !proc.isRecording())) return;
        const double beat = sr * 60.0 / bpm, step = Quantizer::stepSamples(bpm, sr, (int) proc.getParam("qdiv"));
        const double pxPerSample = (double) getWidth() / viewLen;
        const double unit = step * pxPerSample >= 5.0 ? step : (beat * pxPerSample >= 5.0 ? beat : beat * 4.0);
        for (long k = juce::jmax(0L, (long) std::floor(viewStart / unit)); (double) k * unit <= viewStart + viewLen; ++k)
        {
            const double pos = (double) k * unit, inBeat = pos / beat;
            const bool isBeat = std::abs(inBeat - std::round(inBeat)) < 1e-6;
            const bool isBar = isBeat && std::llround(inBeat) % 4 == 0;
            g.setColour(juce::Colours::white.withAlpha(isBar ? 0.28f : (isBeat ? 0.16f : 0.06f)));
            g.drawVerticalLine((int) sampleToX(pos), (float) wave.getY(), (float) wave.getBottom());
        }
    }

    void drawRuler(juce::Graphics& g, juce::Rectangle<int> r)
    {
        g.setColour(juce::Colour(0xff1b1e23));
        g.fillRect(r);
        const double sr = proc.getRate();
        const double pxPerSec = getWidth() / (viewLen / sr);
        double step = 10.0;
        for (double c : { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0 })
            if (c * pxPerSec >= 60.0) { step = c; break; }
        g.setFont(10.0f);
        const double t0 = viewStart / sr, t1 = (viewStart + viewLen) / sr;
        for (double t = std::ceil(t0 / step) * step; t <= t1; t += step)
        {
            const float x = sampleToX(t * sr);
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.drawVerticalLine((int) x, (float) r.getY() + 6.0f, (float) r.getBottom());
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.drawText(juce::String(t, step < 0.1 ? 2 : (step < 1.0 ? 1 : 0)) + "s",
                       (int) x + 3, r.getY(), 50, r.getHeight(), juce::Justification::centredLeft);
        }
    }

    void updatePeaks(int len)
    {
        if (len <= cachedLen) return;
        const float* d = proc.getRecData();
        const int nb = (len + PK - 1) / PK;
        pkMin.resize((size_t) nb);
        pkMax.resize((size_t) nb);
        for (int b = cachedLen / PK; b < nb; ++b)
        {
            float mn = 0, mx = 0;
            for (int i = b * PK, e = juce::jmin(len, i + PK); i < e; ++i) { mn = juce::jmin(mn, d[i]); mx = juce::jmax(mx, d[i]); }
            pkMin[(size_t) b] = mn;
            pkMax[(size_t) b] = mx;
            peakAbs = juce::jmax(peakAbs, mx, -mn);
        }
        cachedLen = len;
    }

    void timerCallback() override
    {
        const int id = proc.getRecId();
        const int len = proc.getRecLength();
        if (id != knownRecId || len < cachedLen)   // new take
        {
            knownRecId = id;
            newTake = true;
            cachedLen = 0;
            pkMin.clear(); pkMax.clear();
            peakAbs = 0;
        }
        updatePeaks(len);

        const bool rec = proc.isRecording();
        if (rec)
        {
            proc.getLiveHits(hits);
            sel.assign(hits.size(), 0);
            drag = Drag::none;
            viewLen = 8.0 * proc.getRate();
            viewStart = juce::jmax(0.0, (double) len - viewLen);
        }
        else
        {
            const int ver = proc.getHitsVersion();
            if (ver != knownVersion)   // re-analysed (or first look)
            {
                if (wasRecording || knownVersion < 0 || newTake) zoomToFit();
                newTake = false;
                knownVersion = ver;
                const auto h = proc.getRecHits();
                hits = h != nullptr ? *h : std::vector<RecHit>();
                sel.assign(hits.size(), 0);
                hover = -1;
                drag = Drag::none;
            }
            else if (wasRecording) zoomToFit();

            if (proc.isPlaying() || proc.isPlayingOriginal())   // keep the playhead in view
            {
                const double pp = proc.isPlaying() ? (double) proc.getPlayPos() : (double) proc.getOriginalPos();
                if (pp < viewStart || pp > viewStart + viewLen) { viewStart = pp - viewLen * 0.05; clampView(); }
            }
        }
        wasRecording = rec;
        repaint();
    }

    BeatboxProcessor& proc;
    std::vector<RecHit> hits;   // editable copy, sorted by time
    std::vector<char> sel;      // selection flags, parallel to hits
    int knownVersion = -1, knownRecId = -1, hover = -1;
    bool wasRecording = false, newTake = false, dropHover = false;
    double viewStart = 0, viewLen = 8.0 * 44100.0;

    std::vector<float> pkMin, pkMax;
    int cachedLen = 0;
    float peakAbs = 0;

    Drag drag = Drag::none;
    bool dragged = false;
    double rangeA = 0, rangeB = 0, dragStart = 0;
    std::vector<char> rangeBase, dragSel;
    std::vector<RecHit> dragOrig;
};
