#include "PluginProcessor.h"
#include "PluginEditor.h"

KickLockAudioProcessor::KickLockAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    delayMsParam = apvts.getRawParameterValue ("delayMs");
    delayInterpParam = apvts.getRawParameterValue ("delayInterp");
    polarityInvertParam = apvts.getRawParameterValue ("polarityInvert");
    rotatorFreqParam = apvts.getRawParameterValue ("rotatorFreq");
    rotatorQParam = apvts.getRawParameterValue ("rotatorQ");
    rotatorStagesParam = apvts.getRawParameterValue ("rotatorStages");
}

juce::AudioProcessorValueTreeState::ParameterLayout KickLockAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "delayMs", 1 },
        "Audio Bass Delay",
        juce::NormalisableRange<float> (0.0f, 50.0f, 0.01f),
        0.0f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "delayInterp", 1 },
        "Delay Interpolation",
        juce::StringArray { "Linear", "Allpass" },
        0));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "polarityInvert", 1 },
        "Polarity Invert",
        false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "rotatorFreq", 1 },
        "Rotator Frequency",
        juce::NormalisableRange<float> (20.0f, 2000.0f, 0.0f, 0.3f),
        200.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "rotatorQ", 1 },
        "Rotator Q",
        juce::NormalisableRange<float> (0.1f, 10.0f, 0.01f),
        0.7f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "rotatorStages", 1 },
        "Rotator Stages",
        juce::StringArray { "2", "3", "4" },
        0));

    return layout;
}

void KickLockAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        mainDelay[(size_t) ch].prepare (sampleRate, 50.0f);
        rotator[(size_t) ch].prepare (sampleRate, 2);
    }

    correlationMeter.prepare (sampleRate, (int) sampleRate / 20);
    scopeFifo.prepare (8192);

    // ~2 seconds of raw bass/kick for the Analyze button's cross-correlation.
    rawCapture.prepare ((int) (sampleRate * 2.0));
    transientDetector.prepare (sampleRate);
    transientDetector.setThreshold (0.004f);
    transientDetector.setMinimumEnergyGate (0.0004f);
    transientDetector.setAttackReleaseMs (2.0f, 60.0f);
    transientDetector.setHoldoffMs (90.0f);
    hitCapture.prepare (sampleRate, 25.0f, 180.0f);
    hitHistory.reset();

    // Show roughly one second of audio across the 2048-sample scope buffer:
    // push 1 in N samples so the trace scrolls slowly enough to follow.
    scopeDecimationFactor = juce::jmax (1, (int) (sampleRate / 2048.0));
    scopeDecimationCounter = 0;

    lastInterpChoice = 0;
    lastStageChoice = 0;
    lastRotatorFreq = -1.0f;
    lastRotatorQ = -1.0f;
}

void KickLockAudioProcessor::releaseResources()
{
}

bool KickLockAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainIn.isDisabled() || mainOut.isDisabled())
        return false;

    if (mainIn != mainOut)
        return false;

    // Main and sidechain buses may each be mono or stereo only.
    for (const auto& bus : layouts.inputBuses)
        if (! bus.isDisabled() && bus != juce::AudioChannelSet::mono() && bus != juce::AudioChannelSet::stereo())
            return false;

    for (const auto& bus : layouts.outputBuses)
        if (! bus.isDisabled() && bus != juce::AudioChannelSet::mono() && bus != juce::AudioChannelSet::stereo())
            return false;

    return true;
}

void KickLockAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Clear any output channels that don't have a corresponding input,
    // since buffers may contain garbage.
    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    auto mainBuffer = getBusBuffer (buffer, true, 0);
    auto sidechainBuffer = getBusBuffer (buffer, true, 1);

    const auto* sidechainBus = getBus (true, 1);
    const bool hasSidechain = sidechainBus != nullptr && sidechainBus->isEnabled() && sidechainBuffer.getNumChannels() > 0;

    const int numSamples = mainBuffer.getNumSamples();

    // Capture RAW (pre-processing) mono bass/kick for the Analyze button, before
    // polarity/delay/rotator touch the buffers. Skipped without a sidechain.
    if (hasSidechain)
    {
        const int mainCh = mainBuffer.getNumChannels();
        const int scCh   = sidechainBuffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            float mSum = 0.0f;
            for (int ch = 0; ch < mainCh; ++ch)
                mSum += mainBuffer.getSample (ch, i);
            const float bassMono = mainCh > 0 ? mSum / (float) mainCh : 0.0f;

            float sSum = 0.0f;
            for (int ch = 0; ch < scCh; ++ch)
                sSum += sidechainBuffer.getSample (ch, i);
            const float kickMono = scCh > 0 ? sSum / (float) scCh : 0.0f;

            rawCapture.push (bassMono, kickMono);
            const bool transientDetected = transientDetector.processSample (kickMono);
            hitCapture.pushSample (bassMono, kickMono, transientDetected);
        }
    }

    // 1) Polarity invert on the main bus.
    if (polarityInvertParam->load() > 0.5f)
    {
        for (int ch = 0; ch < mainBuffer.getNumChannels(); ++ch)
            mainBuffer.applyGain (ch, 0, numSamples, -1.0f);
    }

    // 2) Fractional delay on the main/bass bus only. The sidechain/kick is a
    // reference signal in normal DAW routing, not audible audio this plugin can
    // move. Analyzer results that would require moving the kick are reported as
    // timeline/edit recommendations instead of being applied here.
    const float delayMs = juce::jmax (0.0f, delayMsParam->load());
    const double delaySamples = delayMs / 1000.0 * getSampleRate();

    for (int ch = 0; ch < 2; ++ch)
        mainDelay[(size_t) ch].setDelaySamples ((float) delaySamples);

    // Only switch interpolation type on an actual change: it resets the
    // allpass interpolator's internal state, which would otherwise glitch
    // the signal every single block if called unconditionally.
    const int interpChoice = (int) delayInterpParam->load();
    if (interpChoice != lastInterpChoice)
    {
        const auto type = interpChoice == 0 ? InterpolationType::Linear : InterpolationType::Allpass;

        for (int ch = 0; ch < 2; ++ch)
            mainDelay[(size_t) ch].setInterpolationType (type);

        lastInterpChoice = interpChoice;
    }

    for (int ch = 0; ch < mainBuffer.getNumChannels(); ++ch)
        for (int i = 0; i < numSamples; ++i)
            mainBuffer.setSample (ch, i, mainDelay[(size_t) ch].processSample (mainBuffer.getSample (ch, i)));

    // 3) Allpass phase rotator on the main bus.
    const int stageChoice = (int) rotatorStagesParam->load();
    if (stageChoice != lastStageChoice)
    {
        // Re-preparing changes the active stage count but also resets filter
        // state, so this is likewise gated on an actual change rather than
        // done every block.
        for (int ch = 0; ch < 2; ++ch)
            rotator[(size_t) ch].prepare (getSampleRate(), stageChoice + 2);

        lastStageChoice = stageChoice;
    }

    // setParameters() rebuilds allpass coefficients, which allocates. Only
    // call it when the frequency or Q actually changed (or on the first block
    // after prepareToPlay, forced by the -1 sentinels) so the audio thread
    // never allocates in steady state. Coefficients survive a stage-change
    // prepare() (which only resets filter state), so no re-apply is needed
    // there.
    const float rotatorFreq = rotatorFreqParam->load();
    const float rotatorQ = rotatorQParam->load();
    if (rotatorFreq != lastRotatorFreq || rotatorQ != lastRotatorQ)
    {
        for (int ch = 0; ch < 2; ++ch)
            rotator[(size_t) ch].setParameters (rotatorFreq, rotatorQ);

        lastRotatorFreq = rotatorFreq;
        lastRotatorQ = rotatorQ;
    }

    for (int ch = 0; ch < mainBuffer.getNumChannels(); ++ch)
        for (int i = 0; i < numSamples; ++i)
            mainBuffer.setSample (ch, i, rotator[(size_t) ch].processSample (mainBuffer.getSample (ch, i)));

    // 4) Feed the correlation meter and scope fifo with mono-summed
    // main/sidechain values. Skipped entirely when there's no sidechain,
    // since a zero-sidechain signal would be meaningless for both.
    if (hasSidechain)
    {
        const int mainChannels = mainBuffer.getNumChannels();
        const int sidechainChannels = sidechainBuffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            float mainSum = 0.0f;
            for (int ch = 0; ch < mainChannels; ++ch)
                mainSum += mainBuffer.getSample (ch, i);
            const float mainMono = mainChannels > 0 ? mainSum / (float) mainChannels : 0.0f;

            float sidechainSum = 0.0f;
            for (int ch = 0; ch < sidechainChannels; ++ch)
                sidechainSum += sidechainBuffer.getSample (ch, i);
            const float sidechainMono = sidechainChannels > 0 ? sidechainSum / (float) sidechainChannels : 0.0f;

            correlationMeter.pushSample (mainMono, sidechainMono);

            // Decimate the scope feed only: the meter still sees every sample,
            // but the scope gets 1 in N so its window spans ~1 second.
            if (++scopeDecimationCounter >= scopeDecimationFactor)
            {
                scopeDecimationCounter = 0;
                scopeFifo.pushSample (mainMono, sidechainMono);
            }
        }
    }

    correlationPercent.store (correlationMeter.getCorrelation());
}

juce::AudioProcessorEditor* KickLockAudioProcessor::createEditor()
{
    return new KickLockAudioProcessorEditor (*this);
}

bool KickLockAudioProcessor::hasEditor() const
{
    return true;
}

const juce::String KickLockAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool KickLockAudioProcessor::acceptsMidi() const
{
    return false;
}

bool KickLockAudioProcessor::producesMidi() const
{
    return false;
}

bool KickLockAudioProcessor::isMidiEffect() const
{
    return false;
}

double KickLockAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int KickLockAudioProcessor::getNumPrograms()
{
    return 1;
}

int KickLockAudioProcessor::getCurrentProgram()
{
    return 0;
}

void KickLockAudioProcessor::setCurrentProgram (int /*index*/)
{
}

const juce::String KickLockAudioProcessor::getProgramName (int /*index*/)
{
    return {};
}

void KickLockAudioProcessor::changeProgramName (int /*index*/, const juce::String& /*newName*/)
{
}

void KickLockAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void KickLockAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

AnalyzerInstruction KickLockAudioProcessor::analyzeAndApply (AnalyzeMode mode)
{
    // Snapshot the raw capture (message thread; allocation is fine here).
    std::vector<float> bass, kick;
    const int n = rawCapture.snapshot (bass, kick);

    const auto result = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n,
                                                    getSampleRate(), 50.0f);
    const auto instruction = AnalyzerInstructionBuilder::build (result, mode);

    if (! result.valid || mode != AnalyzeMode::AutoApplySafe
        || instruction.action == AlignmentAction::LowConfidence)
        return instruction;

    // AutoApplySafe writes through parameters so both the sliders and the host
    // update. Only bass-path changes are legal here. Negative analyzer delay
    // means "move/delay the kick", so it remains a recommendation.
    if (result.delayMs > 0.0f)
        if (auto* delayParam = apvts.getParameter ("delayMs"))
            delayParam->setValueNotifyingHost (delayParam->convertTo0to1 (result.delayMs));

    if (auto* polParam = apvts.getParameter ("polarityInvert"))
        polParam->setValueNotifyingHost (result.invertPolarity ? 1.0f : 0.0f);

    // Apply the recommended phase-rotator settings too, when the search found
    // an improvement. rotatorStages is a choice param: index 0->2, 1->3, 2->4.
    if (result.adjustRotator)
    {
        if (auto* freqParam = apvts.getParameter ("rotatorFreq"))
            freqParam->setValueNotifyingHost (freqParam->convertTo0to1 (result.rotatorFreqHz));

        if (auto* qParam = apvts.getParameter ("rotatorQ"))
            qParam->setValueNotifyingHost (qParam->convertTo0to1 (result.rotatorQ));

        if (auto* stagesParam = apvts.getParameter ("rotatorStages"))
        {
            const int stageIndex = juce::jlimit (0, 2, result.rotatorStages - 2);
            stagesParam->setValueNotifyingHost (stagesParam->convertTo0to1 ((float) stageIndex));
        }
    }

    return instruction;
}

HitAnalysisResult KickLockAudioProcessor::analyzeLatestHit()
{
    std::vector<float> bass, kick;
    const int n = hitCapture.snapshotLatest (bass, kick);

    if (n <= 0)
    {
        HitAnalysisResult result;
        result.instruction.action = AlignmentAction::NotEnoughSignal;
        result.instruction.message = "Waiting for detected kick hit.";
        return result;
    }

    auto result = PerHitAnalyzer::analyzeHit (bass.data(), kick.data(), n,
                                              getSampleRate(), hitCapture.getSequence());

    if (! result.valid)
        return result;

    hitHistory.push (result);

    result.instruction.message << " | hit avg "
                               << juce::String ((int) std::round (hitHistory.getRollingMatchPercent())) << "%"
                               << " over " << juce::String (hitHistory.getCount());

    return result;
}

int KickLockAudioProcessor::getLatestHitSequence() const noexcept
{
    return hitCapture.getSequence();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KickLockAudioProcessor();
}
