#include "PluginProcessor.h"

namespace
{
    using Float = juce::AudioParameterFloat;
    using Range = juce::NormalisableRange<float>;

    std::unique_ptr<Float> makeFloat (const char* id, const char* name, float lo, float hi,
                                      float step, float def, const char* unit)
    {
        return std::make_unique<Float> (juce::ParameterID { id, 1 }, name, Range (lo, hi, step), def,
                                        juce::AudioParameterFloatAttributes().withLabel (unit));
    }
}

SpeechLevelerProcessor::SpeechLevelerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout SpeechLevelerProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "denoise", 1 }, "Denoise", true));
    layout.add (makeFloat ("reduction",   "Noise Reduction",   0.0f, 30.0f, 0.5f, 12.0f, "dB"));
    layout.add (makeFloat ("sensitivity", "Noise Sensitivity", 0.0f, 12.0f, 0.5f,  3.0f, "dB"));

    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "agc", 1 }, "Auto Level", true));
    layout.add (makeFloat ("target",    "Target RMS",  -30.0f, -10.0f, 0.1f, -20.0f, "dB"));
    layout.add (makeFloat ("gate",      "Gate",        -70.0f, -30.0f, 0.5f, -50.0f, "dB"));
    layout.add (makeFloat ("maxboost",  "Max Boost",     0.0f,  36.0f, 0.5f,  24.0f, "dB"));
    layout.add (makeFloat ("threshold", "Threshold",   -40.0f,   0.0f, 0.1f, -22.0f, "dB"));
    layout.add (makeFloat ("ratio",     "Ratio",         1.0f,  10.0f, 0.1f,   3.0f, ":1"));
    layout.add (makeFloat ("attack",    "Attack",        1.0f, 100.0f, 0.5f,  10.0f, "ms"));
    layout.add (makeFloat ("release",   "Release",      20.0f, 500.0f, 1.0f, 120.0f, "ms"));
    layout.add (makeFloat ("makeup",    "Makeup",      -12.0f,  18.0f, 0.1f,   3.0f, "dB"));
    layout.add (makeFloat ("ceiling",   "Ceiling",     -12.0f,   0.0f, 0.1f,  -6.0f, "dB"));

    return layout;
}

DynamicsParams SpeechLevelerProcessor::readParams() const
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    DynamicsParams p;
    p.denoiseEnabled     = get ("denoise") > 0.5f;
    p.denoiseReductionDb = get ("reduction");
    p.denoiseSensitivity = get ("sensitivity");
    p.agcEnabled  = get ("agc") > 0.5f;
    p.targetRmsDb = get ("target");
    p.gateDb      = get ("gate");
    p.maxBoostDb  = get ("maxboost");
    p.thresholdDb = get ("threshold");
    p.ratio       = get ("ratio");
    p.attackMs    = get ("attack");
    p.releaseMs   = get ("release");
    p.makeupDb    = get ("makeup");
    p.ceilingDb   = get ("ceiling");
    return p;
}

void SpeechLevelerProcessor::prepareToPlay (double sampleRate, int)
{
    preparedChannels = getTotalNumOutputChannels();
    channelPtrs.assign ((size_t) std::max (1, preparedChannels), nullptr);

    dynamics.prepare (sampleRate, preparedChannels);
    dynamics.setParams (readParams());

    // host kompensuje opoznienie lookahead limitera
    setLatencySamples (dynamics.getLatencySamples());
}

bool SpeechLevelerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (in != out || out.isDisabled())
        return false;

    return out.size() >= 1 && out.size() <= 8;
}

void SpeechLevelerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // zabezpieczenie: liczba kanalow musi zgadzac sie z ta z prepareToPlay
    if (buffer.getNumChannels() != preparedChannels)
        return;

    dynamics.setParams (readParams());

    for (int c = 0; c < preparedChannels; ++c)
        channelPtrs[(size_t) c] = buffer.getWritePointer (c);

    dynamics.process (channelPtrs.data(), buffer.getNumSamples());
}

juce::AudioProcessorEditor* SpeechLevelerProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void SpeechLevelerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void SpeechLevelerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpeechLevelerProcessor();
}
