#include "TestCommon.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "dsp/OfflineFundamentalEstimator.h"

namespace
{
    constexpr double kPitchSampleRate = 48000.0;

    std::vector<float> makeBassLikeTone (float fundamentalHz, float subharmonicGain,
                                         float overtoneGain = 0.23f, float noiseGain = 0.0f)
    {
        const int samples = (int) std::lround (kPitchSampleRate * 0.240);
        std::vector<float> signal ((size_t) samples);
        for (int i = 0; i < samples; ++i)
        {
            const float t = (float) i / (float) kPitchSampleRate;
            signal[(size_t) i] = 0.80f * std::sin (2.0f * juce::MathConstants<float>::pi * fundamentalHz * t)
                              + subharmonicGain * std::sin (juce::MathConstants<float>::pi * fundamentalHz * t + 0.31f)
                              + overtoneGain * std::sin (4.0f * juce::MathConstants<float>::pi * fundamentalHz * t + 0.17f)
                              + 0.10f * std::sin (6.0f * juce::MathConstants<float>::pi * fundamentalHz * t)
                              + noiseGain * (0.61f * std::sin (2.0f * juce::MathConstants<float>::pi * 173.0f * t)
                                           + 0.39f * std::sin (2.0f * juce::MathConstants<float>::pi * 287.0f * t));
        }
        return signal;
    }

    FundamentalEstimate estimate (const std::vector<float>& signal)
    {
        return OfflineFundamentalEstimator::estimate (signal.data(), (int) signal.size(),
                                                       kPitchSampleRate, 25.0f, 300.0f);
    }

    std::vector<float> loadRecordedFixture (const char* filename)
    {
        auto directory = juce::File::getCurrentWorkingDirectory();
        for (int i = 0; i < 6; ++i)
        {
            const auto file = directory.getChildFile ("tests/assets/pitch_cc0").getChildFile (filename);
            if (file.existsAsFile())
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                if (reader == nullptr || reader->numChannels < 1 || reader->lengthInSamples < 2)
                    return {};

                juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
                std::vector<float> mono ((size_t) buffer.getNumSamples());
                const float weight = 1.0f / (float) buffer.getNumChannels();
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                        mono[(size_t) sample] += buffer.getSample (ch, sample) * weight;
                // Match the local Learn estimator's analysis horizon. Skip the
                // pluck attack (which is intentionally non-stationary) before
                // measuring the stable recorded bass body.
                const int attackSkip = (int) std::lround (kPitchSampleRate * 0.100);
                const int window = (int) std::lround (kPitchSampleRate * 0.240);
                if ((int) mono.size() > attackSkip)
                {
                    const int available = std::min (window, (int) mono.size() - attackSkip);
                    mono = std::vector<float> (mono.begin() + attackSkip,
                                               mono.begin() + attackSkip + available);
                }
                return mono;
            }
            directory = directory.getParentDirectory();
        }
        return {};
    }
}

class PitchOctaveRegressionTests : public juce::UnitTest
{
public:
    PitchOctaveRegressionTests() : juce::UnitTest ("Pitch octave resolution", "N") {}

    void runTest() override
    {
        beginTest ("Known C1, B1, A1, and C2 fundamentals retain their MIDI frequencies");
        {
            for (const float expectedHz : { 32.7032f, 61.7354f, 55.0000f, 65.4064f })
            {
                const auto result = estimate (makeBassLikeTone (expectedHz, 0.0f));
                logMessage ("expected=" + juce::String (expectedHz, 4)
                            + " selected=" + juce::String (result.frequencyHz, 4)
                            + " confidence=" + juce::String (result.confidence, 4));
                expect (result.valid && ! result.octaveAmbiguous);
                expectWithinAbsoluteError (result.frequencyHz, expectedHz, 1.0f);
            }
        }

        beginTest ("Recorded CC0 bass fixtures retain their published A1 and C2 pitch");
        {
            for (const auto& fixture : std::array<std::pair<const char*, float>, 2> {
                     std::pair { "cabled_mess_a1.wav", 55.0000f },
                     std::pair { "cabled_mess_c2.wav", 65.4064f } })
            {
                const auto signal = loadRecordedFixture (fixture.first);
                expect (! signal.empty(), "recorded fixture is available: " + juce::String (fixture.first));
                if (signal.empty())
                    continue;
                const auto result = estimate (signal);
                logMessage ("recorded=" + juce::String (fixture.first)
                            + " expected=" + juce::String (fixture.second, 4)
                            + " selected=" + juce::String (result.frequencyHz, 4)
                            + " confidence=" + juce::String (result.confidence, 4)
                            + " lag=" + juce::String (result.selectedLag)
                            + " cmndf=" + juce::String (result.selectedCmndf, 4)
                            + " ambiguous=" + juce::String (result.octaveAmbiguous ? 1 : 0));
                expect (result.valid && ! result.octaveAmbiguous);
                expectWithinAbsoluteError (result.frequencyHz, fixture.second, 1.0f);
            }
        }

        beginTest ("Weak subharmonic selection is promoted only when periodic and harmonic evidence support 2F");
        {
            constexpr float selectedCmndf = 0.201f;
            constexpr float higherCmndf = 0.160f;
            constexpr float lowHarmonic = 0.28416f;
            constexpr float highHarmonic = 0.43594f;
            const auto decision = OfflineFundamentalEstimator::resolveOctaveEvidence (
                selectedCmndf, 1.0f - higherCmndf >= OfflineFundamentalEstimator::kMinConfidence,
                lowHarmonic, highHarmonic);
            logMessage ("auto evidence selectedCmndf=" + juce::String (selectedCmndf, 5)
                        + " higherCmndf=" + juce::String (higherCmndf, 5)
                        + " lowH=" + juce::String (lowHarmonic, 5)
                        + " highH=" + juce::String (highHarmonic, 5)
                        + " decision=" + juce::String ((int) decision));
            expectEquals ((int) decision, (int) OctaveEvidenceResolution::promoteHigher);
        }

        beginTest ("Unresolved F versus 2F evidence is marked ambiguous and never promoted");
        {
            const auto ambiguous = OfflineFundamentalEstimator::resolveOctaveEvidence (
                0.201f, true, 0.30186f, 0.29938f);
            const auto strongLow = OfflineFundamentalEstimator::resolveOctaveEvidence (
                0.020f, true, 0.10f, 0.90f);
            logMessage ("ambiguous decision=" + juce::String ((int) ambiguous)
                        + " strong-low decision=" + juce::String ((int) strongLow));
            expectEquals ((int) ambiguous, (int) OctaveEvidenceResolution::ambiguous);
            expectEquals ((int) strongLow, (int) OctaveEvidenceResolution::keepSelected);
        }
    }
};

static PitchOctaveRegressionTests pitchOctaveRegressionTests;
