#include "TestCommon.h"
#include "dsp/ConflictFingerprint.h"
#include "dsp/DynamicRuntimeSelector.h"

#include <chrono>
#include <iostream>

namespace
{
    constexpr double kFingerprintSampleRate = 48000.0;

    struct FingerprintMaterial
    {
        std::vector<float> bass;
        std::vector<float> kick;
    };

    FingerprintMaterial makeFingerprintMaterial (int conflictBand)
    {
        const int n = (int) std::lround (kFingerprintSampleRate * 0.012);
        FingerprintMaterial result { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
        const int attack = (int) std::lround (kFingerprintSampleRate * 0.001);
        const int body = (int) std::lround (kFingerprintSampleRate * 0.003);
        for (int i = 0; i < n; ++i)
        {
            const float kick = 0.8f * std::exp (-(float) i / 84.0f);
            result.kick[(size_t) i] = kick;
            const int band = i < attack ? 0 : (i < body ? 1 : 2);
            result.bass[(size_t) i] = band == conflictBand ? -kick : 0.15f * kick;
        }
        return result;
    }

    RuntimeBaseSettings matchingBase()
    {
        RuntimeBaseSettings base;
        base.delayMs = 0.0f;
        base.polarityInvert = false;
        base.crossoverEnabled = true;
        base.crossoverHz = 150.0f;
        base.allpassStages = 2;
        base.delayInterpolationIndex = 0;
        return base;
    }

    ConflictStateEntry makeState (uint64_t fingerprint, float frequency, float q, float delay,
                                  int midi, ConflictRegion region)
    {
        ConflictStateEntry state;
        state.fingerprint = fingerprint;
        state.allpassFreqHz = frequency;
        state.allpassQ = q;
        state.delayMs = delay;
        state.stages = 2;
        state.regionType = region;
        state.hitCount = NoteMap::kMinHitsPerNote;
        state.confidence = 0.85f;
        state.matchPercent = 90.0f;
        state.improvementPoints = 4.0f;
        state.applied = true;
        state.noteLabel = midi;
        return state;
    }

    NotePhaseMapSnapshot makeStateMap (uint64_t first, uint64_t second)
    {
        auto map = NoteMap::makeEmptyNoteMap();
        map.valid = true;
        map.base.crossoverEnabled = true;
        map.base.crossoverHz = 150.0f;
        map.base.allpassStages = 2;
        map.base.learnedSampleRate = kFingerprintSampleRate;
        map.global.allpassFreqHz = 80.0f;
        map.global.allpassQ = 0.8f;
        map.states[0] = makeState (first, 60.0f, 0.6f, -1.25f, 36, ConflictRegion::attack);
        map.states[1] = makeState (second, 160.0f, 1.8f, 2.0f, 43, ConflictRegion::body);
        return map;
    }

    void setProcessorValue (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    FingerprintMaterial lowPassForProcessor (const FingerprintMaterial& source)
    {
        auto filter = [] (const std::vector<float>& input)
        {
            juce::dsp::LinkwitzRileyFilter<float> lowpass;
            lowpass.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
            lowpass.prepare ({ kFingerprintSampleRate, 64, 1 });
            lowpass.setCutoffFrequency (150.0f);
            std::vector<float> output (input.size());
            for (size_t i = 0; i < input.size(); ++i)
                output[i] = lowpass.processSample (0, input[i]);
            return output;
        };
        return { filter (source.bass), filter (source.kick) };
    }

    int learnDetectorOnset (const std::vector<float>& kick)
    {
        TransientDetector detector;
        detector.prepare (kFingerprintSampleRate);
        detector.setThreshold (1.0e-7f);
        detector.setMinimumEnergyGate (1.0e-8f);
        detector.setAttackReleaseMs (2.0f, 60.0f);
        detector.setTriggerRatio (1.35f);
        detector.setHoldoffMs (90.0f);
        for (int i = 0; i < (int) kick.size(); ++i)
            if (detector.processSample (kick[(size_t) i]))
                return i;
        return -1;
    }
}

class RuntimeFingerprintTests : public juce::UnitTest
{
public:
    RuntimeFingerprintTests() : juce::UnitTest ("Runtime fingerprint matching", "L") {}

    void runTest() override
    {
        beginTest ("Ground truth: shared train/serve descriptor selects the known state and rejects an unlike onset");
        {
            const auto attack = makeFingerprintMaterial (0);
            const auto body = makeFingerprintMaterial (1);
            const auto trainedAttack = makeConflictFingerprint (attack.bass.data(), attack.kick.data(),
                                                                 (int) attack.kick.size(), kFingerprintSampleRate, 0);
            const auto trainedBody = makeConflictFingerprint (body.bass.data(), body.kick.data(),
                                                               (int) body.kick.size(), kFingerprintSampleRate, 0);
            expect (trainedAttack.valid && trainedBody.valid);

            RuntimeConflictFingerprintCapture runtime;
            runtime.prepare (kFingerprintSampleRate);
            ConflictFingerprint served;
            bool received = false;
            for (size_t i = 0; i < attack.kick.size(); ++i)
            {
                runtime.pushSample (attack.bass[i], attack.kick[i]);
                received = runtime.takeCompleted (served) || received;
            }
            expect (received && served.valid);
            expectLessThan (ConflictFingerprint::distance (trainedAttack.pack(), served.pack()), 0.02f,
                            "worker and audio-thread descriptor must agree for the same onset");

            const auto map = makeStateMap (trainedAttack.pack(), trainedBody.pack());
            DynamicNoteState state;
            const auto selected = selectDynamicRuntime (map, matchingBase(), 90.0f, 0.9f, 0.0f,
                                                        1.0f, 64, 12000, state, &served);
            logMessage ("ground truth selectedState=" + juce::String (selected.selectedState)
                        + " distance=" + juce::String (selected.fingerprintDistance, 6)
                        + " freq=" + juce::String (selected.targetFreqHz, 3)
                        + " delayMs=" + juce::String (selected.targetDelayMs, 3));
            expectEquals (selected.selectedState, 0);
            expect (! selected.fallbackActive);
            expectWithinAbsoluteError (selected.targetFreqHz, 60.0f, 1.0e-5f);
            expectWithinAbsoluteError (selected.targetDelayMs, -1.25f, 1.0e-5f);

            ConflictFingerprint unlike;
            unlike.valid = true;
            unlike.values = { 0.0f, 0.0f, 1.0f, 0.0f };
            const auto fallback = selectDynamicRuntime (map, matchingBase(), 90.0f, 0.9f, 0.0f,
                                                        1.0f, 64, 12000, state, &unlike);
            logMessage ("unlike distance=" + juce::String (fallback.fingerprintDistance, 6)
                        + " fallback=" + juce::String ((int) fallback.fallbackActive));
            expect (fallback.fallbackActive);
            expectEquals (fallback.selectedState, -1);
        }

        beginTest ("Dense state changes retarget 3 ms allpass and delay ramps without a full-scale zipper jump");
        {
            AllpassPhaseRotator rotator;
            rotator.prepare (kFingerprintSampleRate, 2);
            rotator.setRampDurationMs (3.0f);
            rotator.setParameters (60.0f, 0.6f);

            FractionalDelayLine delay;
            delay.prepare (kFingerprintSampleRate, 12.0f);
            delay.setRampDurationMs (3.0f);
            delay.setDelaySamples (120.0f);

            float previous = 0.0f;
            float worstJump = 0.0f;
            for (int i = 0; i < 12000; ++i)
            {
                if (i > 0 && i % 24 == 0) // faster than the 144-sample ramp
                {
                    const bool alternate = ((i / 24) & 1) != 0;
                    rotator.setParameters (alternate ? 180.0f : 55.0f, alternate ? 2.2f : 0.55f);
                    delay.setDelaySamples (alternate ? 360.0f : 96.0f);
                }
                const float input = 0.45f * std::sin ((float) i * 0.01047f);
                const float output = rotator.processSample (delay.processSample (input));
                expect (std::isfinite (output));
                if (i > 512)
                    worstJump = std::max (worstJump, std::abs (output - previous));
                previous = output;
            }
            logMessage ("dense-ramp worst adjacent jump=" + juce::String (worstJump, 6));
            expectLessThan (worstJump, 0.25f);
        }

        beginTest ("PluginProcessor consumes the onset fingerprint without live pitch selection");
        {
            const auto material = makeFingerprintMaterial (0);
            const auto filtered = lowPassForProcessor (material);
            const int onset = learnDetectorOnset (filtered.kick);
            const auto fingerprint = makeConflictFingerprint (filtered.bass.data(), filtered.kick.data(),
                                                               (int) filtered.kick.size(), kFingerprintSampleRate, onset);
            expect (onset >= 0 && fingerprint.valid);
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kFingerprintSampleRate, 64);
            processor.prepareToPlay (kFingerprintSampleRate, 64);
            setProcessorValue (processor, "correction_mode", 1.0f);
            setProcessorValue (processor, "allpass_enable", 1.0f);
            setProcessorValue (processor, "phaseFilterEnabled", 1.0f);
            setProcessorValue (processor, "delay_ms", 0.0f);
            setProcessorValue (processor, "delayMs", 0.0f);
            setProcessorValue (processor, "polarity_invert", 0.0f);
            setProcessorValue (processor, "polarityInvert", 0.0f);
            setProcessorValue (processor, "crossover_enable", 1.0f);
            setProcessorValue (processor, "crossover_freq", 150.0f);
            setProcessorValue (processor, "rotatorStages", 0.0f);
            setProcessorValue (processor, "delayInterp", 0.0f);
            expect (processor.publishNoteMapForTesting (makeStateMap (fingerprint.pack(), fingerprint.pack())));

            for (int offset = 0; offset < 512; offset += 64)
            {
                juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                             processor.getTotalNumOutputChannels()), 64);
                buffer.clear();
                for (int i = 0; i < 64; ++i)
                {
                    const int index = offset + i;
                    const float kick = index < (int) material.kick.size() ? material.kick[(size_t) index] : 0.0f;
                    const float bass = index < (int) material.bass.size() ? material.bass[(size_t) index] : 0.0f;
                    buffer.setSample (0, i, bass);
                    if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass);
                    if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick);
                    if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick);
                }
                juce::MidiBuffer midi;
                processor.processBlock (buffer, midi);
            }
            logMessage ("plugin activeMidi=" + juce::String (processor.activeMidiNote.load())
                        + " fallback=" + juce::String ((int) processor.dynamicFallbackActive.load())
                        + " stale=" + juce::String ((int) processor.dynamicMapStale.load()));
            expectEquals (processor.activeMidiNote.load(), 36);
            expect (! processor.dynamicFallbackActive.load());
        }

        beginTest ("Runtime matching cost is bounded to the fixed state array");
        {
            const auto material = makeFingerprintMaterial (0);
            const auto fingerprint = makeConflictFingerprint (material.bass.data(), material.kick.data(),
                                                               (int) material.kick.size(), kFingerprintSampleRate, 0);
            auto map = makeStateMap (fingerprint.pack(), fingerprint.pack());
            for (int i = 2; i < NotePhaseMapSnapshot::kMaxStates; ++i)
                map.states[(size_t) i] = makeState (fingerprint.pack(), 80.0f + (float) i, 0.8f,
                                                     0.0f, 36, ConflictRegion::tail);

            const auto start = std::chrono::steady_clock::now();
            DynamicNoteState state;
            int selected = 0;
            for (int i = 0; i < 100000; ++i)
                selected += selectDynamicRuntime (map, matchingBase(), 90.0f, 0.9f, 0.0f,
                                                   1.0f, 1, 12000, state, &fingerprint).selectedState;
            const auto milliseconds = std::chrono::duration<double, std::milli> (
                std::chrono::steady_clock::now() - start).count();
            std::cout << "Layer C runtime fingerprint benchmark: " << milliseconds / 100000.0
                      << " ms/select (16-state bound) at 48 kHz" << std::endl;
            expect (selected >= 0);
        }
    }
};

static RuntimeFingerprintTests runtimeFingerprintTests;
