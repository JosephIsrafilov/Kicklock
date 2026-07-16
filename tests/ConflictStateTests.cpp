#include "TestCommon.h"
#include "ConflictStateClusterer.h"
#include "NotePhaseMap.h"
#include <chrono>
#include <iostream>

class ConflictStateAxisTests : public juce::UnitTest
{
public:
    ConflictStateAxisTests() : juce::UnitTest ("ConflictStateAxis", "K") {}

    void runTest() override
    {
        beginTest ("Tolerance bins keep natural correction variation in one state");
        {
            std::vector<ConflictStateSample> samples;
            for (int i = 0; i < 6; ++i)
                samples.push_back ({ i, 2.0f + 0.12f * (float) i, false,
                                     100.0f + 1.5f * (float) i, 1.0f + 0.03f * (float) i,
                                     2, ConflictRegion::body, 0.8f, 90.0f, 5.0f, 33 });
            const auto clusters = ConflictStateClusterer::cluster (samples, 16);
            expectEquals ((int) clusters.size(), 1);
            expectEquals ((int) clusters.front().samples.size(), 6);
        }

        beginTest ("State signature separates polarity and region, independent of MIDI");
        {
            std::vector<ConflictStateSample> samples {
                { 0, 1.0f, false, 90.0f, 1.0f, 2, ConflictRegion::attack, 0.8f, 90.0f, 4.0f, 33 },
                { 1, 1.1f, false, 91.0f, 1.0f, 2, ConflictRegion::attack, 0.8f, 90.0f, 4.0f, 45 },
                { 2, 1.0f, true, 90.0f, 1.0f, 2, ConflictRegion::attack, 0.8f, 90.0f, 4.0f, 36 },
                { 3, 1.0f, false, 90.0f, 1.0f, 2, ConflictRegion::tail, 0.8f, 90.0f, 4.0f, 40 }
            };
            const auto clusters = ConflictStateClusterer::cluster (samples, 16);
            expectEquals ((int) clusters.size(), 3);
        }

        beginTest ("Overflow merges nearest signatures to fixed state budget");
        {
            std::vector<ConflictStateSample> samples;
            for (int i = 0; i < 10; ++i)
                samples.push_back ({ i, (float) i * 4.0f, false, 80.0f + (float) i * 20.0f,
                                     0.7f, 2, ConflictRegion::body, 0.8f, 80.0f, 3.0f, 30 + i });
            const auto clusters = ConflictStateClusterer::cluster (samples, 3);
            expectEquals ((int) clusters.size(), 3);
            int total = 0;
            for (const auto& c : clusters) total += (int) c.samples.size();
            expectEquals (total, 10);
        }

        beginTest ("Schema v3 remains a note-path compatibility map; v4 states round-trip");
        {
            auto map = NoteMap::makeEmptyNoteMap();
            map.valid = true;
            map.global.allpassFreqHz = 100.0f;
            map.global.allpassQ = 1.0f;
            map.base.crossoverHz = 150.0f;
            map.base.allpassStages = 2;
            map.base.learnedSampleRate = 48000.0;
            map.states[0] = { 1.5f, true, 100.0f, 1.0f, 2, ConflictRegion::body,
                              0, 6, 0.8f, 88.0f, 5.0f, true, 36 };
            auto tree = noteMapToValueTree (map);
            auto parsed = noteMapFromValueTree (tree);
            expect (parsed.valid);
            expectEquals ((int) parsed.states[0].regionType, (int) ConflictRegion::body);
            expectWithinAbsoluteError (parsed.states[0].delayMs, 1.5f, 1.0e-6f);
            tree.setProperty (juce::Identifier (NoteMapKeys::schemaVersion), 3, nullptr);
            parsed = noteMapFromValueTree (tree);
            expect (parsed.valid);
            expect (! parsed.states[0].applied);
        }

        beginTest ("Worker-only state clustering cost is measured separately");
        {
            std::vector<ConflictStateSample> samples;
            for (int i = 0; i < 32; ++i)
                samples.push_back ({ i, 2.0f + 0.02f * (float) i, false, 100.0f + (float) (i % 3),
                                     1.0f, 2, ConflictRegion::body, 0.8f, 90.0f, 4.0f, 36 });
            const auto start = std::chrono::steady_clock::now();
            int total = 0;
            for (int i = 0; i < 1000; ++i)
                total += (int) ConflictStateClusterer::cluster (samples, NotePhaseMapSnapshot::kMaxStates).size();
            const auto elapsed = std::chrono::duration<double, std::milli> (
                std::chrono::steady_clock::now() - start).count();
            std::cout << "Layer B worker state-clustering benchmark: " << elapsed / 1000.0
                      << " ms/32-hit batch at 48 kHz" << std::endl;
            expectEquals (total, 1000);
        }
    }
};

static ConflictStateAxisTests conflictStateAxisTests;
