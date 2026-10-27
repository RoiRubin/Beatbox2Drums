cmake_minimum_required(VERSION 3.22)
project(BeatboxToDrums VERSION 0.1.0)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(BeatboxToDrums
    COMPANY_NAME "Me"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    PLUGIN_MANUFACTURER_CODE Bbxd
    PLUGIN_CODE Bb2d
    FORMATS VST3
    COPY_PLUGIN_AFTER_BUILD TRUE
    PRODUCT_NAME "Beatbox To Drums")

target_sources(BeatboxToDrums PRIVATE
    Source/PluginProcessor.cpp
    Source/Common.h Source/Detector.h Source/PluginProcessor.h Source/PluginEditor.h
    Source/SpectrumView.h Source/LevelMeter.h Source/FileDragger.h Source/ClipView.h Source/DrumRow.h
    Source/TimingBar.h Source/Metronome.h Source/Quantizer.h Source/DrumSynth.h)

target_compile_definitions(BeatboxToDrums PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(BeatboxToDrums
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)