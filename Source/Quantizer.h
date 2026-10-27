#pragma once
#include "Common.h"

// Snaps drum hits to a tempo grid. The grid is anchored at sample 0 of the take
// (the first click of the metronome), so bars and beats line up with the host.
struct Quantizer
{
    static juce::StringArray divisionNames()
    {
        return { "1/4", "1/8", "1/16", "1/32", "1/8 triplet", "1/16 triplet" };
    }
    static int stepsPerBeat(int division)
    {
        static const int n[] = { 1, 2, 4, 8, 3, 6 };
        return n[juce::jlimit(0, 5, division)];
    }
    static double stepSamples(double bpm, double sr, int division)
    {
        return sr * 60.0 / juce::jmax(20.0, bpm) / (double) stepsPerBeat(division);
    }

    // strength 0..1 moves each hit that far towards its nearest grid line. `sel` flags (parallel to
    // hits) follow the hits through the re-sort; with onlySelected, unselected hits stay put.
    // Hits of the same drum that end up on the same grid line are merged (the louder one is kept).
    static void apply(std::vector<RecHit>& hits, std::vector<char>& sel, double bpm, double sr,
                      int division, float strength, bool onlySelected)
    {
        if (hits.empty()) return;
        sel.resize(hits.size(), 0);
        const double step = stepSamples(bpm, sr, division);
        for (size_t i = 0; i < hits.size(); ++i)
        {
            if (onlySelected && !sel[i]) continue;
            const double s = (double) hits[i].sample;
            const double snapped = std::round(s / step) * step;
            hits[i].sample = juce::jmax<juce::int64>(0, std::llround(s + (snapped - s) * (double) strength));
        }

        std::vector<size_t> idx(hits.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return hits[a].sample < hits[b].sample; });

        std::vector<RecHit> h;
        std::vector<char> s;
        for (auto i : idx)
        {
            bool merged = false;
            for (size_t j = h.size(); j-- > 0 && h[j].sample == hits[i].sample && !merged;)
                if (h[j].slot == hits[i].slot)
                {
                    h[j].vel = juce::jmax(h[j].vel, hits[i].vel);
                    if (h[j].vol < 0.0f) h[j].vol = hits[i].vol;
                    s[j] = s[j] || sel[i];
                    merged = true;
                }
            if (merged) continue;
            h.push_back(hits[i]);
            s.push_back(sel[i]);
        }
        hits.swap(h);
        sel.swap(s);
    }
};
