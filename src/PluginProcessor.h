#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

#include "util/ScopeFifo.h"
#include "util/RawCaptureBuffer.h"
#include "dsp/CorrelationMeter.h"
#include "dsp/FractionalDelayLine.h"
#include "dsp/AllpassPhaseRotator.h"
#include "dsp/AlignmentAnalyzer.h"

class KickLockAudioProcessor : public juce::AudioProcessor
{
public:
    KickLockAudioProcessor();
    ~KickLockAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float> correlationPercent { 0.0f };
    ScopeFifo scopeFifo;

    // Runs cross-correlation over the captured raw bass/kick and applies the
    // best delay + polarity to the parameters. Message thread only (allocates).
    // Returns the recommendation and before/after match for the UI to report.
    AlignmentResult analyzeAndApply();

private:
    std::atomic<float>* delayMsParam = nullptr;
    std::atomic<float>* delayInterpParam = nullptr;
    std::atomic<float>* polarityInvertParam = nullptr;
    std::atomic<float>* rotatorFreqParam = nullptr;
    std::atomic<float>* rotatorQParam = nullptr;
    std::atomic<float>* rotatorStagesParam = nullptr;

    std::array<FractionalDelayLine, 2> mainDelay, sidechainDelay;
    std::array<AllpassPhaseRotator, 2> rotator;
    CorrelationMeter correlationMeter;

    // Rolling capture of raw (pre-processing) mono bass/kick for the Analyze
    // button. Sized in prepareToPlay (~2 s). Written on the audio thread,
    // snapshotted on the message thread.
    RawCaptureBuffer rawCapture;

    int lastInterpChoice = 0;
    int lastStageChoice = 0;
    float lastRotatorFreq = -1.0f;
    float lastRotatorQ = -1.0f;

    // Scope decimation: only push 1 in N samples to the scope fifo so the
    // display shows ~1 second of audio and scrolls slowly enough to read.
    // The correlation meter still receives every sample. Set in prepareToPlay
    // from the sample rate.
    int scopeDecimationFactor = 1;
    int scopeDecimationCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessor)
};
