#include "TestCommon.h"

// P10.1 - Realtime capture: O(1) push, no per-block full copy, correct
// chronological snapshot, and an early snapshot must not report capacity-length
// zero padding as if it were captured material.
class RealtimeCaptureTests : public juce::UnitTest
{
public:
    RealtimeCaptureTests() : juce::UnitTest ("RealtimeCapture", "P10") {}

    void runTest() override
    {
        beginTest ("Early snapshot returns only pushed samples, not capacity zeros");
        {
            RawCaptureBuffer buffer;
            buffer.prepare (2048);

            for (int i = 0; i < 100; ++i)
                buffer.push ((float) (i + 1), (float) (-(i + 1)));

            expectEquals (buffer.getFilledSamples(), 100);

            std::vector<float> bass, kick;
            const int count = buffer.snapshot (bass, kick);

            expectEquals (count, 100);
            expectEquals ((int) bass.size(), 100);
            // No trailing zero padding from the unfilled 1948 samples.
            expectWithinAbsoluteError (bass[0], 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (bass[99], 100.0f, 1.0e-6f);
            expectWithinAbsoluteError (kick[99], -100.0f, 1.0e-6f);
        }

        beginTest ("Snapshot is chronological (oldest first) after the ring wraps");
        {
            RawCaptureBuffer buffer;
            buffer.prepare (4);
            for (int i = 0; i < 6; ++i)
                buffer.push ((float) i, (float) (100 + i));

            std::vector<float> bass, kick;
            const int count = buffer.snapshot (bass, kick);

            expectEquals (count, 4);
            expectWithinAbsoluteError (bass[0], 2.0f, 1.0e-6f);   // oldest kept
            expectWithinAbsoluteError (bass[3], 5.0f, 1.0e-6f);   // newest
            expectWithinAbsoluteError (kick[0], 102.0f, 1.0e-6f);
            expectWithinAbsoluteError (kick[3], 105.0f, 1.0e-6f);
        }

        beginTest ("Push does not allocate or copy the whole buffer per call");
        {
            // Behavioural proxy for "O(1), no full-buffer copy": a huge ring can
            // absorb many single-sample pushes cheaply, and each push only ever
            // touches one slot (fill count advances by exactly one).
            RawCaptureBuffer buffer;
            buffer.prepare (96000);

            for (int i = 0; i < 500; ++i)
            {
                const int before = buffer.getFilledSamples();
                buffer.push (0.5f, -0.5f);
                expectEquals (buffer.getFilledSamples(), before + 1);
            }
        }

        beginTest ("Filled count saturates at capacity");
        {
            RawCaptureBuffer buffer;
            buffer.prepare (8);
            for (int i = 0; i < 40; ++i)
                buffer.push ((float) i, (float) i);

            expectEquals (buffer.getFilledSamples(), 8);
        }
    }
};

static RealtimeCaptureTests realtimeCaptureTestsInstance;

//==============================================================================
// P10.2 - End-to-end analyze/apply: feed known kick+bass errors through the
// processor, ensure analyzeFix recommends the intended correction, applyLatestFix
// writes those settings, and a second pass materially improves the processed
// match meter.
class PhaseFixEndToEndTests : public juce::UnitTest
{
public:
    PhaseFixEndToEndTests() : juce::UnitTest ("PhaseFixEndToEnd", "P10") {}

    void runTest() override
    {
        struct Case
        {
            const char* name = "";
            int kickDelaySamples = 0;
            bool invertBass = false;
        };

        const Case cases[] =
        {
            { "Analyze/apply fixes a 1.5 ms early inverted bass", 72, true },
            { "Analyze/apply also fixes a 2.5 ms early inverted bass", 120, true }
        };

        for (const auto& testCase : cases)
        {
            beginTest (testCase.name);

            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            const std::vector<int> bassDelays (4, 0);
            const std::vector<int> kickDelays (4, testCase.kickDelaySamples);
            const std::vector<bool> invertBass (4, testCase.invertBass);

            feedHitSeries (processor, bassDelays, kickDelays, invertBass, 2048);
            const float beforeDry = processor.dryInputMatchPercent.load();
            const float beforeProcessed = processor.processedMatchPercent.load();

            const auto fix = processor.analyzeFix();
            const float expectedDelayMs = (float) testCase.kickDelaySamples / (float) kSampleRate * 1000.0f;

            expectGreaterThan (fix.contributingHits, 1);
            expect (fix.applyAllowed, fix.message);
            expectWithinAbsoluteError (fix.bassDelayMs, expectedDelayMs, 0.3f);
            expect (fix.bassPolarityInvert == testCase.invertBass);

            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "delayMs"), fix.bassDelayMs, 0.02f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"),
                                       fix.bassPolarityInvert ? 1.0f : 0.0f, 1.0e-7f);

            feedHitSeries (processor, bassDelays, kickDelays, invertBass, 2048);
            const float afterDry = processor.dryInputMatchPercent.load();
            const float afterProcessed = processor.processedMatchPercent.load();

            expectGreaterThan (afterProcessed, beforeProcessed + 15.0f);
            expectGreaterThan (afterProcessed, afterDry + 15.0f);
            expectWithinAbsoluteError (afterDry, beforeDry, 5.0f);
        }
    }

private:
    static float rawParam (KickLockAudioProcessor& processor, const char* id)
    {
        if (const auto* value = processor.apvts.getRawParameterValue (id))
            return value->load();

        return 0.0f;
    }

    static void feedHitSeries (KickLockAudioProcessor& processor,
                               const std::vector<int>& bassDelays,
                               const std::vector<int>& kickDelays,
                               const std::vector<bool>& invertBass,
                               int blockSize)
    {
        const int hitCount = std::min ({ (int) bassDelays.size(),
                                         (int) kickDelays.size(),
                                         (int) invertBass.size() });
        const int hitSpacing = 12000;
        const int eventLength = 5000;
        const int startOffset = 1500;
        const int totalSamples = startOffset + hitCount * hitSpacing + eventLength;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int numSamples = std::min (blockSize, totalSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             numSamples);
            buffer.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                const int sample = start + i;
                float bass = 0.0f;
                float kick = 0.0f;

                for (int hit = 0; hit < hitCount; ++hit)
                {
                    const int base = startOffset + hit * hitSpacing;
                    const int bassIndex = sample - (base + bassDelays[(size_t) hit]);
                    const int kickIndex = sample - (base + kickDelays[(size_t) hit]);

                    if (bassIndex >= 0 && bassIndex < eventLength)
                    {
                        const float env = (float) std::exp (-6.0 * (double) bassIndex / kSampleRate);
                        const float value = env * (float) std::sin (kTwoPi * 70.0 * (double) bassIndex / kSampleRate);
                        bass += invertBass[(size_t) hit] ? -value : value;
                    }

                    if (kickIndex >= 0 && kickIndex < eventLength)
                    {
                        const float env = (float) std::exp (-6.0 * (double) kickIndex / kSampleRate);
                        kick += env * (float) std::sin (kTwoPi * 70.0 * (double) kickIndex / kSampleRate);
                    }
                }

                buffer.setSample (0, i, bass);
                if (buffer.getNumChannels() > 1)
                    buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2)
                    buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3)
                    buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }
};

static PhaseFixEndToEndTests phaseFixEndToEndTestsInstance;

//==============================================================================
// P10.2 - Sidechain status: a kick transient plus following silence stays
// "active" through the hold window; a normal beat never flickers to too-low
// between hits; Apply Fix availability is not tied to instant RMS.
class SidechainStatusTests : public juce::UnitTest
{
public:
    SidechainStatusTests() : juce::UnitTest ("SidechainStatus", "P10") {}

    void runTest() override
    {
        beginTest ("Held activity survives the silent gap after a hit");
        {
            SignalActivityTracker tracker;
            tracker.prepare (kSampleRate, 500.0f, 3.0e-3f);

            const int block = 512;
            // One loud block (a kick), then ~400 ms of silence in 512-sample
            // blocks. 400 ms < 500 ms hold, so it must still read active.
            tracker.pushBlock (0.3f, block);
            expect (tracker.isActive());

            const int silentBlocks = (int) (kSampleRate * 0.4 / block);
            for (int i = 0; i < silentBlocks; ++i)
                tracker.pushBlock (0.0f, block);

            expect (tracker.isActive(), "should still be active inside hold window");
        }

        beginTest ("Activity lapses after the hold window fully elapses");
        {
            SignalActivityTracker tracker;
            tracker.prepare (kSampleRate, 300.0f, 3.0e-3f);

            const int block = 512;
            tracker.pushBlock (0.3f, block);

            const int longSilence = (int) (kSampleRate * 0.6 / block) + 2; // > 300 ms
            for (int i = 0; i < longSilence; ++i)
                tracker.pushBlock (0.0f, block);

            expect (! tracker.isActive(), "should lapse after hold window");
        }

        beginTest ("A repeating beat never flickers inactive between kicks");
        {
            SignalActivityTracker kick;
            kick.prepare (kSampleRate, 500.0f, 3.0e-3f);

            const int block = 512;
            const int blocksPerBeat = (int) (kSampleRate * 0.4 / block); // 150 BPM-ish
            bool everInactiveAfterFirst = false;

            for (int beat = 0; beat < 8; ++beat)
            {
                kick.pushBlock (0.4f, block); // transient block
                for (int i = 1; i < blocksPerBeat; ++i)
                {
                    kick.pushBlock (0.0f, block); // gap
                    if (! kick.isActive())
                        everInactiveAfterFirst = true;
                }
            }

            expect (! everInactiveAfterFirst, "status flickered to inactive between kicks");
        }

        beginTest ("Material-usable separates present-but-quiet from playing");
        {
            SignalActivityTracker tracker;
            tracker.prepare (kSampleRate, 500.0f, 1.0e-3f);

            // Present (crosses activation) but below the usable phase-read floor.
            tracker.pushBlock (2.0e-3f, 512);
            expect (tracker.isActive());
            expect (! tracker.isUsable (8.0e-3f), "quiet signal should not be usable");

            tracker.pushBlock (0.05f, 512);
            expect (tracker.isUsable (8.0e-3f), "loud signal should be usable");
        }

        beginTest ("Apply Fix availability ignores instant level, needs sidechain + result");
        {
            // Between kicks (instant RMS ~ 0) Apply must stay available as long as
            // the sidechain is connected and a fix is ready.
            expect (applyFixAvailable (true, true));
            expect (! applyFixAvailable (false, true));  // no sidechain
            expect (! applyFixAvailable (true, false));  // no result yet
        }

        beginTest ("Classifier does not report too-low while both are held active");
        {
            const auto status = classifyAnalysisMaterialStatus (true, true, true, true, true);
            expectEquals ((int) status, (int) AnalysisMaterialStatus::ReadyToAnalyze);

            const auto capturing = classifyAnalysisMaterialStatus (true, true, true, true, false);
            expectEquals ((int) capturing, (int) AnalysisMaterialStatus::CapturingMaterial);

            // Between hits both are still held active -> never WaitingForKick/too-low.
            const auto betweenHits = classifyAnalysisMaterialStatus (true, true, true, true, true);
            expect (betweenHits != AnalysisMaterialStatus::SignalTooLow);
            expect (betweenHits != AnalysisMaterialStatus::WaitingForKick);
        }
    }
};

static SidechainStatusTests sidechainStatusTestsInstance;

//==============================================================================
// P10.3 - Multi-band scoring: bands span ~20 Hz-2 kHz, the low-end-weighted
// score is not dominated by a high-frequency click, inverted bass scores low in
// the low bands, and an aligned pair scores high.
class MultiBandScoringTests : public juce::UnitTest
{
public:
    MultiBandScoringTests() : juce::UnitTest ("MultiBandScoring", "P10") {}

    void runTest() override
    {
        beginTest ("Band plan spans roughly 20 Hz to 2 kHz");
        {
            expectEquals (MultiBandCorrelationResult::numBands, 5);
            expectWithinAbsoluteError (PhaseBands::table.front().lowHz, 20.0f, 0.001f);
            expectWithinAbsoluteError (PhaseBands::table.back().highHz, 2000.0f, 0.001f);
            // Low end must carry the most decision weight.
            expectGreaterThan (PhaseBands::table[(size_t) PhaseBands::subBand].weight,
                               PhaseBands::table[(size_t) PhaseBands::attackBand].weight);
        }

        beginTest ("Aligned low-frequency pair scores high");
        {
            constexpr int n = 8192;
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                const float v = (float) std::sin (kTwoPi * 55.0 * t);
                bass[(size_t) i] = v;
                kick[(size_t) i] = v;
            }

            const auto r = MultiBandCorrelation::analyze (bass.data(), kick.data(), n, kSampleRate);
            expectGreaterThan (r.weightedMatchPercent, 90.0f);
            expectGreaterThan (r.lowEndMatchPercent, 90.0f);
        }

        beginTest ("Inverted bass scores low in the low bands");
        {
            constexpr int n = 8192;
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                const float v = (float) std::sin (kTwoPi * 55.0 * t);
                bass[(size_t) i] = -v;
                kick[(size_t) i] = v;
            }

            const auto r = MultiBandCorrelation::analyze (bass.data(), kick.data(), n, kSampleRate);
            expectLessThan (r.lowEndMatchPercent, 10.0f);
            expectLessThan (r.weightedMatchPercent, 10.0f);
        }

        beginTest ("A shared HF click does not dominate the low-end-weighted score");
        {
            // Low end is INVERTED (should read low); a loud shared 5 kHz click sits
            // in-phase but out of band (ATTACK tops out at 2 kHz, weight 0.05).
            // The weighted score must follow the low end, not the click.
            constexpr int n = 16384;
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                const double click = 5.0 * std::sin (kTwoPi * 5000.0 * t) * std::exp (-t * 150.0);
                const double low = std::sin (kTwoPi * 55.0 * t);
                kick[(size_t) i] = (float) (click + low);
                bass[(size_t) i] = (float) (click - low); // low end inverted
            }

            const auto r = MultiBandCorrelation::analyze (bass.data(), kick.data(), n, kSampleRate);
            expect (r.weightedMatchPercent < 40.0f,
                    "weighted score = " + juce::String (r.weightedMatchPercent));
            expectLessThan (r.lowEndMatchPercent, 20.0f);
        }

        beginTest ("Realtime multi-band meter tracks aligned vs inverted low end");
        {
            RealtimeMultiBandMeter meter;
            meter.prepare (kSampleRate, (int) (kSampleRate * 0.25));

            for (int i = 0; i < 12000; ++i)
            {
                const float v = (float) std::sin (kTwoPi * 55.0 * (double) i / kSampleRate);
                meter.pushSample (v, v);
            }
            expectGreaterThan (meter.getLowEndMatchPercent(), 85.0f);

            RealtimeMultiBandMeter inverted;
            inverted.prepare (kSampleRate, (int) (kSampleRate * 0.25));
            for (int i = 0; i < 12000; ++i)
            {
                const float v = (float) std::sin (kTwoPi * 55.0 * (double) i / kSampleRate);
                inverted.pushSample (-v, v);
            }
            expectLessThan (inverted.getLowEndMatchPercent(), 15.0f);
        }
    }
};

static MultiBandScoringTests multiBandScoringTestsInstance;

//==============================================================================
// P10.4 - Analyze stability: a stable loop gives a stable recommendation,
// inconsistent hits are flagged Unstable, a large offset is never auto-applied,
// and a small (0.5-2 ms) delay can be recommended.
class AnalyzeStabilityTests : public juce::UnitTest
{
public:
    AnalyzeStabilityTests() : juce::UnitTest ("AnalyzeStability", "P10") {}

    void runTest() override
    {
        constexpr int n = 8192;

        auto fillBurst = [] (std::vector<float>& bass, std::vector<float>& kick,
                             int bassOffset, int kickOffset, double freq)
        {
            std::fill (bass.begin(), bass.end(), 0.0f);
            std::fill (kick.begin(), kick.end(), 0.0f);
            for (int i = 0; i < (int) bass.size(); ++i)
            {
                const int bi = i - bassOffset;
                const int ki = i - kickOffset;
                if (bi >= 0)
                {
                    const double t = (double) bi / kSampleRate;
                    bass[(size_t) i] = (float) (std::sin (kTwoPi * freq * t) * std::exp (-t * 5.0));
                }
                if (ki >= 0)
                {
                    const double t = (double) ki / kSampleRate;
                    kick[(size_t) i] = (float) (std::sin (kTwoPi * freq * t) * std::exp (-t * 5.0));
                }
            }
        };

        beginTest ("Repeated analysis of the same window is deterministic");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1040, 65.0); // bass early -> small +delay

            const auto a = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 5.0f);
            const auto b = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 5.0f);
            const auto c = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 5.0f);

            expectWithinAbsoluteError (b.bassDelayMs, a.bassDelayMs, 1.0e-4f);
            expectWithinAbsoluteError (c.bassDelayMs, a.bassDelayMs, 1.0e-4f);
            expect (b.bassPolarityInvert == a.bassPolarityInvert);
            expectEquals ((int) b.quality, (int) a.quality);
        }

        beginTest ("A small 0.5-2 ms delay can be recommended");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1040, 65.0); // ~0.83 ms

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);
            expect (r.valid);
            expectGreaterThan (r.bassDelayMs, 0.4f);
            expectLessThan (r.bassDelayMs, 2.0f);
            expect (PhaseFixEngine::canApply (r), r.message);
        }

        beginTest ("A 40 ms offset is never auto-applied as bass delay");
        {
            constexpr int largeLag = 1920; // 40 ms at 48 kHz
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1000 + largeLag, 65.0);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 5.0f);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::LargeTimingOffset);
            expect (! PhaseFixEngine::canApply (r));
            expectLessOrEqual (r.bassDelayMs, 5.0f);
        }

        beginTest ("Stable multi-hit loop aggregates to a confident, applicable fix");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedStableHits (processor, 2048);

            const auto fix = processor.analyzeFix();

            expectGreaterThan (fix.contributingHits, 1);
            expect (! fix.unstableRecommendation, fix.message);
            expect (fix.applyAllowed);
            expect (PhaseFixEngine::canApply (fix), fix.message);
            expectWithinAbsoluteError (fix.bassDelayMs, 40.0f / 48.0f, 0.2f);
        }

        beginTest ("Perfectly aligned multi-hit loop stays AlreadyGood, not Unstable");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHits (processor, 2048, /*bassDelay*/ 0, /*kickDelay*/ 0, /*alternate*/ false);

            const auto fix = processor.analyzeFix();
            expectGreaterThan (fix.contributingHits, 1);
            expect (! fix.unstableRecommendation, fix.message);
            expectEquals ((int) fix.quality, (int) PhaseFixQuality::AlreadyGood);
            expect (! fix.applyAllowed);
            expect (! fix.optionalApplyAllowed);
        }

        beginTest ("Conflicting hits are reported Unstable, not silently applied");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedConflictingHits (processor, 2048);

            const auto fix = processor.analyzeFix();
            expectGreaterThan (fix.contributingHits, 1);
            expectEquals ((int) fix.quality, (int) PhaseFixQuality::Unstable);
            expect (! fix.applyAllowed);
            expect (! fix.optionalApplyAllowed);
            expect (! PhaseFixEngine::canApply (fix));
        }
    }

private:
    static void feedStableHits (KickLockAudioProcessor& processor, int blockSize)
    {
        feedHits (processor, blockSize, /*bassDelay*/ 0, /*kickDelay*/ 40, /*alternate*/ false);
    }

    static void feedConflictingHits (KickLockAudioProcessor& processor, int blockSize)
    {
        feedHits (processor, blockSize, 0, 40, /*alternate*/ true);
    }

    static void feedHits (KickLockAudioProcessor& processor, int blockSize,
                          int bassDelay, int kickDelay, bool alternate)
    {
        const int hitCount = 3;
        const int hitSpacing = 12000;
        const int eventLength = 5000;
        const int startOffset = 1500;
        const int totalSamples = startOffset + hitCount * hitSpacing + eventLength;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int numSamples = std::min (blockSize, totalSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             numSamples);
            buffer.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                const int sample = start + i;
                float bass = 0.0f;
                float kick = 0.0f;

                for (int hit = 0; hit < hitCount; ++hit)
                {
                    const int base = startOffset + hit * hitSpacing;
                    const bool swap = alternate && (hit % 2 == 1);
                    const int bd = swap ? kickDelay : bassDelay;
                    const int kd = swap ? bassDelay : kickDelay;
                    // Alternated hits also flip bass polarity, so the conflicting
                    // case disagrees on BOTH timing and polarity (the robust
                    // "Unstable" trigger the existing multi-hit test relies on).
                    const float bassSign = swap ? -1.0f : 1.0f;
                    const int bassIndex = sample - (base + bd);
                    const int kickIndex = sample - (base + kd);

                    if (bassIndex >= 0 && bassIndex < eventLength)
                    {
                        const double t = (double) bassIndex / kSampleRate;
                        bass += bassSign * (float) (std::sin (kTwoPi * 70.0 * t) * std::exp (-t * 6.0));
                    }
                    if (kickIndex >= 0 && kickIndex < eventLength)
                    {
                        const double t = (double) kickIndex / kSampleRate;
                        kick += (float) (std::sin (kTwoPi * 70.0 * t) * std::exp (-t * 6.0));
                    }
                }

                buffer.setSample (0, i, bass);
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }
};

static AnalyzeStabilityTests analyzeStabilityTestsInstance;

//==============================================================================
// P10.5 - Phase Delta helpers: relative PDC shifts bass vs kick only, the peak
// delta uses the intended (envelope) signal, and the smoothing helper strips
// broadband jitter without moving a symmetric peak.
class PhaseDeltaHelperTests : public juce::UnitTest
{
public:
    PhaseDeltaHelperTests() : juce::UnitTest ("PhaseDeltaHelpers", "P10") {}

    void runTest() override
    {
        beginTest ("Relative PDC offset shifts the bass index only");
        {
            const auto none = resolveRelativeDisplayHistoryIndices (100, 2048, 1800, 12, 0, 4);
            const auto shifted = resolveRelativeDisplayHistoryIndices (100, 2048, 1800, 12, 24, 4);

            expectEquals (shifted.kickIndex, none.kickIndex);         // kick unmoved
            expectEquals (shifted.bassIndex, wrapHistoryIndex (none.bassIndex + 6, 2048));
        }

        beginTest ("Envelope peak delta ignores a single-sample broadband spike");
        {
            const int nn = 512;
            std::vector<float> bass ((size_t) nn, 0.0f), kick ((size_t) nn, 0.0f);

            // Kick: a low-frequency body burst centred near index 120.
            // Bass: same body near index 180, PLUS a lone huge spike at index 5
            // (the kind of broadband glitch that fools a raw-peak reader).
            for (int i = 0; i < nn; ++i)
            {
                const double t = (double) i / kSampleRate;
                kick[(size_t) i] = (float) (std::sin (kTwoPi * 60.0 * t) * std::exp (-std::abs (i - 120) / 40.0));
                bass[(size_t) i] = (float) (std::sin (kTwoPi * 60.0 * t) * std::exp (-std::abs (i - 180) / 40.0));
            }
            bass[5] = 8.0f; // lone spike, away from the musical body

            const auto envMarkers = findEnvelopePeakMarkers (bass.data(), kick.data(), nn, kSampleRate, 12);
            const auto rawMarkers = findScopePeakMarkers (bass.data(), kick.data(), nn, kSampleRate);

            expect (envMarkers.valid);
            // Raw reader is captured by the spike; envelope reader sits on the body.
            expectEquals (rawMarkers.bassPeakIndex, 5);
            expect (envMarkers.bassPeakIndex > 120,
                    "env bass peak = " + juce::String (envMarkers.bassPeakIndex));
            expectGreaterThan (envMarkers.deltaMs, 0.0f); // bass body later than kick body
        }

        beginTest ("Symmetric smoothing preserves a centred peak position");
        {
            const int nn = 256;
            std::vector<float> in ((size_t) nn, 0.0f), out ((size_t) nn, 0.0f);
            for (int i = 0; i < nn; ++i)
                in[(size_t) i] = (float) std::exp (-std::pow ((i - 128) / 12.0, 2.0)); // gaussian at 128

            smoothScopeSignal (in.data(), out.data(), nn, 5);

            int peak = 0;
            float best = 0.0f;
            for (int i = 0; i < nn; ++i)
                if (out[(size_t) i] > best) { best = out[(size_t) i]; peak = i; }

            expect (std::abs (peak - 128) <= 1, "smoothed peak drifted to " + juce::String (peak));
        }

        beginTest ("Envelope smoothing window scales with scope sample rate");
        {
            const int nn = 400;
            std::vector<float> in ((size_t) nn, 0.0f), out ((size_t) nn, 0.0f);
            for (int i = 0; i < nn; ++i)
                in[(size_t) i] = (i % 2 == 0) ? 1.0f : -1.0f; // pure jitter

            smoothScopeEnvelope (in.data(), out.data(), nn, 2000.0);

            // A ~180 Hz half-period window at 2 kHz averages the alternating
            // signal toward zero (jitter removed).
            float maxAbs = 0.0f;
            for (int i = 50; i < nn - 50; ++i)
                maxAbs = std::max (maxAbs, std::abs (out[(size_t) i]));

            expectLessThan (maxAbs, 0.5f);
        }
    }
};

static PhaseDeltaHelperTests phaseDeltaHelperTestsInstance;

