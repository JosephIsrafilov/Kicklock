#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>

#include "util/ScopeFifo.h"
#include "util/RawCaptureBuffer.h"
#include "util/HitCaptureBuffer.h"
#include "dsp/CorrelationMeter.h"
#include "dsp/FractionalDelayLine.h"
#include "dsp/AllpassPhaseRotator.h"
#include "dsp/AlignmentAnalyzer.h"
#include "dsp/AnalyzerInstruction.h"
#include "dsp/TransientDetector.h"
#include "dsp/PerHitAnalyzer.h"
#include "dsp/PhaseFixEngine.h"
#include "ui/ScopeVisuals.h"
#include "ui/UiFormatters.h"

class KickLockAudioProcessor : public juce::AudioProcessor
{
public:
    KickLockAudioProcessor();
    ~KickLockAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBassProcessingNeutral() const noexcept;

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
    std::atomic<float> realtimeLowBandMatchPercent { 0.0f };
    std::atomic<float> dryInputMatchPercent { 0.0f };
    std::atomic<float> processedMatchPercent { 0.0f };
    std::atomic<float> latestAnalyzedBeforePercent { 50.0f };
    std::atomic<float> latestAnalyzedAfterPercent { 50.0f };
    std::atomic<float> latestVerifiedAfterPercent { -1.0f };
    std::atomic<float> latestVerificationDeltaPercent { 0.0f };
    std::atomic<float> latestFixConfidence { 0.0f };
    ScopeFifo scopeFifo;

    // Runs cross-correlation over the captured raw bass/kick. Default behavior
    // is recommend-only; AutoApplySafe may change bass-path parameters, but
    // never delays or otherwise processes the sidechain/kick reference.
    // Message thread only (allocates).
    AnalyzerInstruction analyzeAndApply (AnalyzeMode mode = AnalyzeMode::RecommendOnly);
    HitAnalysisResult analyzeLatestHit();
    int getLatestHitSequence() const noexcept;
    PhaseFixResult analyzeFix();
    bool applyLatestFix();
    PhaseFixResult getLatestFixResult() const;
    int getScopeDecimationFactor() const noexcept;
    bool hasSidechainReference() const noexcept;
    bool isTempoAvailable() const noexcept;
    float getLatestBpm() const noexcept;
    float getBassSignalRms() const noexcept;
    float getKickSignalRms() const noexcept;
    void setLatestFixResultForTesting (const PhaseFixResult&);

private:
    std::atomic<float>* delayMsParam = nullptr;
    std::atomic<float>* delayInterpParam = nullptr;
    std::atomic<float>* polarityInvertParam = nullptr;
    std::atomic<float>* phaseFilterEnabledParam = nullptr;
    std::atomic<float>* rotatorFreqParam = nullptr;
    std::atomic<float>* rotatorQParam = nullptr;
    std::atomic<float>* rotatorStagesParam = nullptr;

    std::array<FractionalDelayLine, 2> mainDelay;
    std::array<AllpassPhaseRotator, 2> rotator;
    CorrelationMeter dryInputCorrelationMeter;
    CorrelationMeter processedCorrelationMeter;

    // Rolling capture of raw (pre-processing) mono bass/kick for the Analyze
    // button. Sized in prepareToPlay (~2 s). Written on the audio thread,
    // then copied into a published buffer that the message thread snapshots.
    RawCaptureBuffer rawCapture;
    TransientDetector transientDetector;
    HitCaptureBuffer hitCapture;
    HitAnalysisHistory hitHistory;
    PhaseFixResult latestFixResult;
    std::vector<float> lastAnalyzedBassWindow;
    std::vector<float> lastAnalyzedKickWindow;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> phaseFilterWet;
    juce::dsp::IIR::Filter<float> dryMainHighPass;
    juce::dsp::IIR::Filter<float> dryMainLowPass;
    juce::dsp::IIR::Filter<float> dryKickHighPass;
    juce::dsp::IIR::Filter<float> dryKickLowPass;
    juce::dsp::IIR::Filter<float> processedMainHighPass;
    juce::dsp::IIR::Filter<float> processedMainLowPass;
    juce::dsp::IIR::Filter<float> processedKickHighPass;
    juce::dsp::IIR::Filter<float> processedKickLowPass;

    int lastInterpChoice = 0;
    int lastStageChoice = 0;
    float lastRotatorFreq = -1.0f;
    float lastRotatorQ = -1.0f;
    bool lastDelayActive = false;

    // Scope decimation: only push 1 in N samples to the scope fifo so the
    // display shows ~1 second of audio and scrolls slowly enough to read.
    // The correlation meter still receives every sample. Set in prepareToPlay
    // from the sample rate.
    int scopeDecimationFactor = 1;
    int scopeDecimationCounter = 0;
    std::atomic<bool> sidechainReferenceAvailable { false };
    std::atomic<bool> tempoAvailable { false };
    std::atomic<float> latestBpm { 0.0f };
    std::atomic<float> bassSignalRms { 0.0f };
    std::atomic<float> kickSignalRms { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessor)
};
