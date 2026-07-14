#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <juce_core/juce_core.h>

#include "HitConsensus.h"    // HitObservation (Pass A container in LearnHitAnalysis)
#include "PhaseFixEngine.h"  // PhaseFixResult  (global fix in LearnFinalizeResult)

// =============================================================================
// Phase 1 pure data models for the Static/Dynamic note-phase map (plan v1.2).
//
// This header holds ONLY data contracts, pure validation/sanitisation helpers,
// and pure ValueTree serialization. It does not touch APVTS, the processor, the
// audio thread, or the live PitchTracker, and nothing here is wired into
// getStateInformation()/setStateInformation() or runtime map selection yet.
// =============================================================================

// -----------------------------------------------------------------------------
// Per-note learned entry.
//
// fundamentalHz  : the estimated physical bass fundamental of the note.
// allpassFreqHz  : the selected allpass CENTRE frequency for the correction.
// These two are semantically distinct and must never be treated as
// interchangeable (a 55 Hz note can need an allpass centre of 80/100/140 Hz).
// -----------------------------------------------------------------------------
struct NoteEntry
{
    bool  learned = false;

    float fundamentalHz = 0.0f;
    float allpassFreqHz = 0.0f;
    float allpassQ = 0.7f;

    // Per-note timing is learned and stored to assess timing quality before
    // trusting this note's Freq/Q. Dynamic Delay/Polarity always use the
    // current global runtime settings.
    float delayMs = 0.0f;
    bool  polarityInvert = false;
    float timingConfidence = 0.0f;
    float timingSpreadMs = 0.0f;

    float confidence = 0.0f;
    float fundamentalSpreadRatio = 0.0f;
    int   hitCount = 0;

    bool  rotatorHelps = false;
};

// Base correction context the map was learned against. Changing any of these at
// runtime makes the map stale (guarded in a later phase).
struct NoteMapBaseContext
{
    float delayMs = 0.0f;
    bool  polarityInvert = false;

    bool  crossoverEnabled = true;
    float crossoverHz = 150.0f;

    int   allpassStages = 2;
    int   delayInterpolationIndex = 0;

    double learnedSampleRate = 0.0;
};

struct NotePhaseMapSnapshot
{
    static constexpr int minMidi = 16;  // ~20.6 Hz
    static constexpr int maxMidi = 67;  // ~392 Hz
    static constexpr int size = maxMidi - minMidi + 1;

    bool valid = false;
    uint32_t schemaVersion = 3;

    NoteMapBaseContext base;
    NoteEntry global;

    std::array<NoteEntry, size> notes {};

    // Fixed-size storage; index helpers never allocate or go out of bounds.
    static constexpr bool containsMidi (int midi) noexcept
    {
        return midi >= minMidi && midi <= maxMidi;
    }

    static constexpr int indexForMidi (int midi) noexcept
    {
        return containsMidi (midi) ? midi - minMidi : -1;
    }

    static constexpr int midiForIndex (int index) noexcept
    {
        return (index >= 0 && index < size) ? index + minMidi : -1;
    }

    // Returns nullptr for out-of-range MIDI (safe fallback path later).
    const NoteEntry* lookup (int midi) const noexcept
    {
        const int index = indexForMidi (midi);
        return index >= 0 ? &notes[(size_t) index] : nullptr;
    }
};

namespace NoteMap
{
    // ---- Learn / map tuning constants (plan v1.2 section 5) ----
    inline constexpr float kMinOfflinePitchConfidence = 0.55f;
    inline constexpr float kPitchAgreementCents = 65.0f;
    inline constexpr int   kMinHitsPerNote = 4;
    inline constexpr float kNoteSpreadMaxRatio = 0.15f;
    inline constexpr float kMinRuntimeConfidence = 0.35f;

    // ---- Valid-entry ranges (plan v1.2 section 4.1) ----
    inline constexpr float kFundamentalMinHz = 25.0f;
    inline constexpr float kFundamentalMaxHz = 300.0f;
    inline constexpr float kAllpassFreqMinHz = 20.0f;
    inline constexpr float kAllpassFreqMaxHz = 500.0f;
    inline constexpr float kAllpassQMin = 0.1f;
    inline constexpr float kAllpassQMax = 10.0f;

    inline constexpr uint32_t kSchemaVersion = 3;
    inline constexpr uint32_t kPreviousSchemaVersion = 2;
    inline constexpr float kMaxSupportedDelayMs = 20.0f;

    inline bool allFieldsFinite (const NoteEntry& e) noexcept
    {
        return std::isfinite (e.fundamentalHz) && std::isfinite (e.allpassFreqHz)
            && std::isfinite (e.allpassQ) && std::isfinite (e.confidence)
            && std::isfinite (e.fundamentalSpreadRatio) && std::isfinite (e.delayMs)
            && std::isfinite (e.timingConfidence) && std::isfinite (e.timingSpreadMs);
    }

    // Strict validity for a learned per-note entry (section 4.1 invariants).
    inline bool isValidNoteEntry (const NoteEntry& e) noexcept
    {
        return e.learned
            && allFieldsFinite (e)
            && e.fundamentalHz >= kFundamentalMinHz && e.fundamentalHz <= kFundamentalMaxHz
            && e.allpassFreqHz >= kAllpassFreqMinHz && e.allpassFreqHz <= kAllpassFreqMaxHz
            && e.allpassQ >= kAllpassQMin && e.allpassQ <= kAllpassQMax
            && e.hitCount >= kMinHitsPerNote
            && e.fundamentalSpreadRatio >= 0.0f
            && e.fundamentalSpreadRatio <= kNoteSpreadMaxRatio
            && std::abs (e.delayMs) <= kMaxSupportedDelayMs
            && e.timingConfidence >= 0.0f && e.timingConfidence <= 1.0f
            && e.timingSpreadMs >= 0.0f;
    }

    // Global fallback validity (section 4.3): finite, positive F/Q. learned and
    // rotatorHelps may be false as long as safe values are present.
    inline bool isValidGlobalEntry (const NoteEntry& g) noexcept
    {
        return allFieldsFinite (g)
            && g.allpassFreqHz > 0.0f
            && g.allpassQ > 0.0f;
    }

    inline bool isValidBaseContext (const NoteMapBaseContext& b) noexcept
    {
        return std::isfinite (b.delayMs)
            && std::isfinite (b.crossoverHz) && b.crossoverHz > 0.0f
            && b.allpassStages >= 2 && b.allpassStages <= 4
            && (b.delayInterpolationIndex == 0 || b.delayInterpolationIndex == 1)
            && std::isfinite (b.learnedSampleRate) && b.learnedSampleRate >= 0.0;
    }

    inline bool isValidNoteMap (const NotePhaseMapSnapshot& m) noexcept
    {
        return m.valid
            && m.schemaVersion == kSchemaVersion
            && isValidGlobalEntry (m.global)
            && isValidBaseContext (m.base);
    }

    // Replaces any non-finite field with a safe default and clamps confidence,
    // spread and Q into range so parsed values can never be NaN/Inf. Frequencies
    // are only NaN-guarded (not clamped) so out-of-range values fail validation
    // and get rejected deterministically rather than silently "fixed".
    inline NoteEntry sanitizeNoteEntry (const NoteEntry& in) noexcept
    {
        auto finite = [] (float v, float fallback) noexcept
        {
            return std::isfinite (v) ? v : fallback;
        };

        NoteEntry e = in;
        e.fundamentalHz = finite (e.fundamentalHz, 0.0f);
        e.allpassFreqHz = finite (e.allpassFreqHz, 0.0f);
        e.allpassQ = std::clamp (finite (e.allpassQ, 0.7f), kAllpassQMin, kAllpassQMax);
        e.delayMs = std::clamp (finite (e.delayMs, 0.0f), -kMaxSupportedDelayMs, kMaxSupportedDelayMs);
        e.timingConfidence = std::clamp (finite (e.timingConfidence, 0.0f), 0.0f, 1.0f);
        e.timingSpreadMs = std::max (0.0f, finite (e.timingSpreadMs, 0.0f));
        e.confidence = std::clamp (finite (e.confidence, 0.0f), 0.0f, 1.0f);
        e.fundamentalSpreadRatio = std::max (0.0f, finite (e.fundamentalSpreadRatio, 1.0f));
        e.hitCount = std::max (0, e.hitCount);
        return e;
    }

    // A safe, deterministic empty map (valid == false).
    inline NotePhaseMapSnapshot makeEmptyNoteMap() noexcept
    {
        return NotePhaseMapSnapshot {};
    }
}

// -----------------------------------------------------------------------------
// Serialization property identifiers. Centralised here so no other file carries
// a magic property-name string. Tree child type: "KLNoteMap"; note child: "Note".
// -----------------------------------------------------------------------------
namespace NoteMapKeys
{
    inline constexpr const char* tree = "KLNoteMap";
    inline constexpr const char* note = "Note";

    inline constexpr const char* schemaVersion = "schemaVersion";
    inline constexpr const char* valid = "valid";

    inline constexpr const char* baseDelayMs = "baseDelayMs";
    inline constexpr const char* basePolarity = "basePolarity";
    inline constexpr const char* baseCrossoverEnabled = "baseCrossoverEnabled";
    inline constexpr const char* baseCrossoverHz = "baseCrossoverHz";
    inline constexpr const char* baseStages = "baseStages";
    inline constexpr const char* baseDelayInterpolation = "baseDelayInterpolation";
    inline constexpr const char* learnedSampleRate = "learnedSampleRate";

    inline constexpr const char* globalFundamentalHz = "globalFundamentalHz";
    inline constexpr const char* globalAllpassFreqHz = "globalAllpassFreqHz";
    inline constexpr const char* globalQ = "globalQ";
    inline constexpr const char* globalConfidence = "globalConfidence";
    inline constexpr const char* globalHits = "globalHits";
    inline constexpr const char* globalRotatorHelps = "globalRotatorHelps";
    inline constexpr const char* globalLearned = "globalLearned";
    inline constexpr const char* globalSpread = "globalSpread";

    inline constexpr const char* noteMidi = "midi";
    inline constexpr const char* noteFundamentalHz = "fundamentalHz";
    inline constexpr const char* noteAllpassFreqHz = "allpassFreqHz";
    inline constexpr const char* noteQ = "q";
    inline constexpr const char* noteDelayMs = "delayMs";
    inline constexpr const char* notePolarity = "polarity";
    inline constexpr const char* noteTimingConfidence = "timingConfidence";
    inline constexpr const char* noteTimingSpreadMs = "timingSpreadMs";
    inline constexpr const char* noteConfidence = "confidence";
    inline constexpr const char* noteSpread = "spread";
    inline constexpr const char* noteHits = "hits";
    inline constexpr const char* noteHelps = "helps";
}

// Pure map -> ValueTree. Only learned notes are written; a note child's presence
// is what marks it learned on the way back in. All NoteEntry fields round-trip
// (the global fallback additionally stores learned + spread). This helper never
// mutates APVTS or processor state.
inline juce::ValueTree noteMapToValueTree (const NotePhaseMapSnapshot& map)
{
    juce::ValueTree tree { juce::Identifier (NoteMapKeys::tree) };

    tree.setProperty (NoteMapKeys::schemaVersion, (int) map.schemaVersion, nullptr);
    tree.setProperty (NoteMapKeys::valid, map.valid, nullptr);

    tree.setProperty (NoteMapKeys::baseDelayMs, map.base.delayMs, nullptr);
    tree.setProperty (NoteMapKeys::basePolarity, map.base.polarityInvert, nullptr);
    tree.setProperty (NoteMapKeys::baseCrossoverEnabled, map.base.crossoverEnabled, nullptr);
    tree.setProperty (NoteMapKeys::baseCrossoverHz, map.base.crossoverHz, nullptr);
    tree.setProperty (NoteMapKeys::baseStages, map.base.allpassStages, nullptr);
    tree.setProperty (NoteMapKeys::baseDelayInterpolation, map.base.delayInterpolationIndex, nullptr);
    tree.setProperty (NoteMapKeys::learnedSampleRate, map.base.learnedSampleRate, nullptr);

    tree.setProperty (NoteMapKeys::globalFundamentalHz, map.global.fundamentalHz, nullptr);
    tree.setProperty (NoteMapKeys::globalAllpassFreqHz, map.global.allpassFreqHz, nullptr);
    tree.setProperty (NoteMapKeys::globalQ, map.global.allpassQ, nullptr);
    tree.setProperty (NoteMapKeys::globalConfidence, map.global.confidence, nullptr);
    tree.setProperty (NoteMapKeys::globalHits, map.global.hitCount, nullptr);
    tree.setProperty (NoteMapKeys::globalRotatorHelps, map.global.rotatorHelps, nullptr);
    tree.setProperty (NoteMapKeys::globalLearned, map.global.learned, nullptr);
    tree.setProperty (NoteMapKeys::globalSpread, map.global.fundamentalSpreadRatio, nullptr);

    for (int i = 0; i < NotePhaseMapSnapshot::size; ++i)
    {
        const NoteEntry& e = map.notes[(size_t) i];
        if (! e.learned)
            continue;

        juce::ValueTree n { juce::Identifier (NoteMapKeys::note) };
        n.setProperty (NoteMapKeys::noteMidi, NotePhaseMapSnapshot::midiForIndex (i), nullptr);
        n.setProperty (NoteMapKeys::noteFundamentalHz, e.fundamentalHz, nullptr);
        n.setProperty (NoteMapKeys::noteAllpassFreqHz, e.allpassFreqHz, nullptr);
        n.setProperty (NoteMapKeys::noteQ, e.allpassQ, nullptr);
        n.setProperty (NoteMapKeys::noteDelayMs, e.delayMs, nullptr);
        n.setProperty (NoteMapKeys::notePolarity, e.polarityInvert, nullptr);
        n.setProperty (NoteMapKeys::noteTimingConfidence, e.timingConfidence, nullptr);
        n.setProperty (NoteMapKeys::noteTimingSpreadMs, e.timingSpreadMs, nullptr);
        n.setProperty (NoteMapKeys::noteConfidence, e.confidence, nullptr);
        n.setProperty (NoteMapKeys::noteSpread, e.fundamentalSpreadRatio, nullptr);
        n.setProperty (NoteMapKeys::noteHits, e.hitCount, nullptr);
        n.setProperty (NoteMapKeys::noteHelps, e.rotatorHelps, nullptr);
        tree.appendChild (n, nullptr);
    }

    return tree;
}

// Pure ValueTree -> map. Never throws. Rules:
//   - missing / foreign tree, or unknown schema version -> invalid empty map;
//   - all numeric values are finite-guarded (never NaN/Inf);
//   - out-of-range MIDI note children are ignored;
//   - a note whose sanitised entry fails validation is rejected (left unlearned);
//   - duplicate MIDI children: the LAST valid child in document order wins, and
//     an invalid duplicate never clobbers an earlier valid entry;
//   - a malformed global fallback invalidates the whole map;
//   - old projects without a KLNoteMap tree parse to an invalid empty map.
inline NotePhaseMapSnapshot noteMapFromValueTree (const juce::ValueTree& tree)
{
    NotePhaseMapSnapshot map = NoteMap::makeEmptyNoteMap();

    if (! tree.isValid() || ! tree.hasType (juce::Identifier (NoteMapKeys::tree)))
        return map;

    const int schema = (int) tree.getProperty (juce::Identifier (NoteMapKeys::schemaVersion), -1);
    if (schema != (int) NoteMap::kSchemaVersion
        && schema != (int) NoteMap::kPreviousSchemaVersion)
        return map;

    map.schemaVersion = NoteMap::kSchemaVersion;
    const bool legacySchema = schema == (int) NoteMap::kPreviousSchemaVersion;

    auto readFinite = [] (const juce::ValueTree& t, const char* key, float fallback) noexcept
    {
        const float v = (float) (double) t.getProperty (juce::Identifier (key), (double) fallback);
        return std::isfinite (v) ? v : fallback;
    };

    map.base.delayMs = readFinite (tree, NoteMapKeys::baseDelayMs, 0.0f);
    map.base.polarityInvert = (bool) tree.getProperty (juce::Identifier (NoteMapKeys::basePolarity), false);
    map.base.crossoverEnabled = (bool) tree.getProperty (juce::Identifier (NoteMapKeys::baseCrossoverEnabled), true);
    map.base.crossoverHz = std::clamp (readFinite (tree, NoteMapKeys::baseCrossoverHz, 150.0f), 20.0f, 500.0f);
    map.base.allpassStages = std::clamp ((int) tree.getProperty (juce::Identifier (NoteMapKeys::baseStages), 2), 2, 4);
    map.base.delayInterpolationIndex = std::clamp ((int) tree.getProperty (juce::Identifier (NoteMapKeys::baseDelayInterpolation), 0), 0, 1);
    const double sr = (double) tree.getProperty (juce::Identifier (NoteMapKeys::learnedSampleRate), 0.0);
    map.base.learnedSampleRate = (std::isfinite (sr) && sr >= 0.0) ? sr : 0.0;

    NoteEntry g;
    g.learned = (bool) tree.getProperty (juce::Identifier (NoteMapKeys::globalLearned), false);
    g.fundamentalHz = readFinite (tree, NoteMapKeys::globalFundamentalHz, 0.0f);
    g.allpassFreqHz = readFinite (tree, NoteMapKeys::globalAllpassFreqHz, 0.0f);
    g.allpassQ = readFinite (tree, NoteMapKeys::globalQ, 0.7f);
    g.confidence = readFinite (tree, NoteMapKeys::globalConfidence, 0.0f);
    g.fundamentalSpreadRatio = readFinite (tree, NoteMapKeys::globalSpread, 0.0f);
    g.hitCount = std::max (0, (int) tree.getProperty (juce::Identifier (NoteMapKeys::globalHits), 0));
    g.rotatorHelps = (bool) tree.getProperty (juce::Identifier (NoteMapKeys::globalRotatorHelps), false);
    g.delayMs = map.base.delayMs;
    g.polarityInvert = map.base.polarityInvert;
    g.timingConfidence = g.confidence;
    map.global = NoteMap::sanitizeNoteEntry (g);

    for (int c = 0; c < tree.getNumChildren(); ++c)
    {
        const juce::ValueTree child = tree.getChild (c);
        if (! child.hasType (juce::Identifier (NoteMapKeys::note)))
            continue;

        const int midi = (int) child.getProperty (juce::Identifier (NoteMapKeys::noteMidi), -1000);
        const int index = NotePhaseMapSnapshot::indexForMidi (midi);
        if (index < 0)
            continue;

        NoteEntry e;
        e.learned = true;
        e.fundamentalHz = readFinite (child, NoteMapKeys::noteFundamentalHz, 0.0f);
        e.allpassFreqHz = readFinite (child, NoteMapKeys::noteAllpassFreqHz, 0.0f);
        e.allpassQ = readFinite (child, NoteMapKeys::noteQ, 0.7f);
        e.delayMs = readFinite (child, NoteMapKeys::noteDelayMs, map.base.delayMs);
        e.polarityInvert = (bool) child.getProperty (juce::Identifier (NoteMapKeys::notePolarity), map.base.polarityInvert);
        e.timingConfidence = readFinite (child, NoteMapKeys::noteTimingConfidence,
                                         legacySchema ? 1.0f : 0.0f);
        e.timingSpreadMs = readFinite (child, NoteMapKeys::noteTimingSpreadMs, 0.0f);
        e.confidence = readFinite (child, NoteMapKeys::noteConfidence, 0.0f);
        // Optional field: a note missing only spread still loads (spread 0 is in
        // range); the serializer always writes it, so round-trips stay exact.
        e.fundamentalSpreadRatio = readFinite (child, NoteMapKeys::noteSpread, 0.0f);
        e.hitCount = std::max (0, (int) child.getProperty (juce::Identifier (NoteMapKeys::noteHits), 0));
        e.rotatorHelps = (bool) child.getProperty (juce::Identifier (NoteMapKeys::noteHelps), false);

        e = NoteMap::sanitizeNoteEntry (e);
        e.learned = true;

        if (NoteMap::isValidNoteEntry (e))
            map.notes[(size_t) index] = e;   // last valid child for this MIDI wins
    }

    const bool treeValid = (bool) tree.getProperty (juce::Identifier (NoteMapKeys::valid), false);
    map.valid = treeValid
             && NoteMap::isValidGlobalEntry (map.global)
             && NoteMap::isValidBaseContext (map.base);

    return map;
}

// =============================================================================
// Pure Learn result models (data only; no orchestration).
//
// LearnHitAnalysis keeps the live tracked pitch and the offline cross-check in
// their OWN fields; the timing observation's dominantFrequencyHz retains its
// existing HitConsensus meaning and must never be overwritten with the tracked
// bass pitch.
// =============================================================================
struct LearnHitAnalysis
{
    int sequence = -1;

    float trackedFundamentalHz = 0.0f;
    float offlineFundamentalHz = 0.0f;
    float offlinePitchConfidence = 0.0f;

    float lowBandEnergy = 0.0f;
    float coarseLagMs = 0.0f;
    float fractionalLagMs = 0.0f;
    float timingConfidence = 0.0f;

    bool signalUsable = false;
    bool timingUsable = false;
    bool timingAmbiguous = false;
    bool dominantTimingClusterMember = false;
    bool pitchAccepted = false;
    bool pitchInvalid = false;
    bool pitchDisagrees = false;
    bool octaveAmbiguous = false;
    float pitchCentsDifference = 0.0f;

    // Pass A: timing/polarity observation for HitConsensus.
    HitObservation timingObservation;

    // Pass B: fixed-base rotator search result.
    bool  rotatorHelps = false;
    float allpassFreqHz = 0.0f;
    float allpassQ = 0.7f;
    float rotatorGainPoints = 0.0f;
    float rotatorConfidence = 0.0f;
};

struct LearnDiagnostics
{
    int capturedHits = 0;
    int analyzedHits = 0;
    int globalOnlyHits = 0;
    int rejectedPitchHits = 0;
    int unusableSignalHits = 0;
    int pitchInvalidHits = 0;
    int pitchDisagreementHits = 0;
    int octaveAmbiguousHits = 0;
    int timingOutlierHits = 0;
    int dominantClusterHitCount = 0;
    int droppedQueueHits = 0;
    int ignoredOverlappingTriggers = 0;

    bool multipleTimingFamilies = false;
    float dominantClusterShare = 0.0f;

    juce::String warning;
};

struct LearnFinalizeResult
{
    bool valid = false;

    NotePhaseMapSnapshot map;
    PhaseFixResult globalFix;
    LearnDiagnostics diagnostics;
    std::vector<LearnHitAnalysis> hitAnalyses;

    juce::String message;
};
