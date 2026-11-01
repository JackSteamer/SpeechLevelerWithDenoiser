cmake_minimum_required(VERSION 3.22)

project(SpeechLeveler VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Uniwersalny plik binarny: Apple Silicon + Intel
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(SpeechLeveler
    COMPANY_NAME                "SpeechLeveler"
    PLUGIN_MANUFACTURER_CODE    Slvr
    PLUGIN_CODE                 Slvl
    FORMATS                     AU Standalone
    PRODUCT_NAME                "Speech Leveler"
    BUNDLE_ID                   com.speechleveler.plugin
    AU_MAIN_TYPE                kAudioUnitType_Effect
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    COPY_PLUGIN_AFTER_BUILD     TRUE      # kopiuje AU do ~/Library/Audio/Plug-Ins/Components
)

target_sources(SpeechLeveler PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/Dynamics.h
)

target_compile_definitions(SpeechLeveler PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(SpeechLeveler
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Test rdzenia DSP (bez JUCE): cmake --build build --target dynamics_test
add_executable(dynamics_test Tests/test_dynamics.cpp)
target_include_directories(dynamics_test PRIVATE Source)
