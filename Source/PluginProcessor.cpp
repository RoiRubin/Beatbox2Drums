// Beatbox -> Drums v4
// - Up to 8 drums (Kick, Snare, Hat, Open Hat, Tom, or Ignore), each with a synth or your own sample
// - Train each drum by beatboxing examples; hits are matched to the closest trained sound
// - Drag & drop samples onto a drum (or empty space to create a new drum)
// - Breath guard + "Ignore" drum type to reject breathing / mouth noise
// - Save / load kits (settings + training + sample paths)
// - Record a beatbox, analyse it into drum hits, play it back, export / drag MIDI
#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* BeatboxProcessor::createEditor() { return new BeatboxEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BeatboxProcessor(); }
