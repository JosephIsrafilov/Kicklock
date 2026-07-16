#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include "ConflictFingerprint.h"
#include "NotePhaseMap.h"
#include "NoteQuantizer.h"

// This is the frozen KLNoteMap runtime. Do not use it as a template for the
// new DynamicStateMap runtime.
namespace LegacyDynamicCompatibility
{
    enum class CorrectionMode
    {
        Static = 0,
        Dynamic = 1
    };

    struct RuntimeBaseSettings
    {
        float delayMs = 0.0f;
        bool polarityInvert = false;
        bool crossoverEnabled = false;
        float crossoverHz = 150.0f;
        int allpassStages = 2;
        int delayInterpolationIndex = 0;
    };

    struct DynamicNoteState
    {
        int lastMidi = -1;
        int silentSamples = 0;
        int fallbackSamples = 0;
        bool hasStableEntry = false;
        float stableFreqHz = 50.0f;
        float stableQ = 0.7f;
        float stableDelayMs = 0.0f;
        bool stablePolarityInvert = false;
        int stableStages = 2;
        int stableMidiLabel = -1;

        void reset() noexcept
        {
            lastMidi = -1;
            silentSamples = 0;
            fallbackSamples = 0;
            hasStableEntry = false;
        }
    };

    struct DynamicRuntimeSelection
    {
        float targetFreqHz = 50.0f;
        float targetQ = 0.7f;
        float targetDelayMs = 0.0f;
        bool targetPolarityInvert = false;
        int targetStages = 2;
        int selectedMidi = -1;
        int selectedState = -1;
        float fingerprintDistance = std::numeric_limits<float>::infinity();
        bool mapUsable = false;
        bool usingLearnedNote = false;
        bool fallbackActive = false;
        bool mapStale = false;
    };

    inline bool isStructurallyValidRuntimeMap (const NotePhaseMapSnapshot& map) noexcept
    {
        return NoteMap::isValidNoteMap (map)
            && map.global.allpassFreqHz >= NoteMap::kAllpassFreqMinHz
            && map.global.allpassFreqHz <= NoteMap::kAllpassFreqMaxHz
            && map.global.allpassQ >= NoteMap::kAllpassQMin
            && map.global.allpassQ <= NoteMap::kAllpassQMax;
    }

    inline bool isUsableMap (const NotePhaseMapSnapshot& map) noexcept
    {
        return isStructurallyValidRuntimeMap (map);
    }

    inline bool mapContextMatchesCurrentParameters (const NotePhaseMapSnapshot& map,
                                                     const RuntimeBaseSettings& current) noexcept
    {
        if (! isStructurallyValidRuntimeMap (map))
            return false;

        return std::abs (map.base.delayMs - current.delayMs) <= 0.05f
            && map.base.polarityInvert == current.polarityInvert
            && map.base.crossoverEnabled == current.crossoverEnabled
            && std::abs (map.base.crossoverHz - current.crossoverHz) <= 1.0f
            && map.base.allpassStages == current.allpassStages
            && map.base.delayInterpolationIndex == current.delayInterpolationIndex;
    }

    inline DynamicRuntimeSelection selectDynamicRuntime (
        const NotePhaseMapSnapshot& map,
        const RuntimeBaseSettings& current,
        float manualFreqHz,
        float manualQ,
        float trackedHz,
        float dynamicStrength,
        int blockSamples,
        int silenceResetSamples,
        DynamicNoteState& noteState,
        const ConflictFingerprint* freshFingerprint = nullptr) noexcept
    {
        DynamicRuntimeSelection selection;
        selection.targetFreqHz = std::clamp (std::isfinite (manualFreqHz) ? manualFreqHz : 50.0f,
                                             NoteMap::kAllpassFreqMinHz, NoteMap::kAllpassFreqMaxHz);
        selection.targetQ = std::clamp (std::isfinite (manualQ) ? manualQ : 0.7f,
                                        NoteMap::kAllpassQMin, NoteMap::kAllpassQMax);
        selection.targetDelayMs = current.delayMs;
        selection.targetPolarityInvert = current.polarityInvert;
        selection.targetStages = current.allpassStages;

        if (! isStructurallyValidRuntimeMap (map))
        {
            selection.fallbackActive = true;
            noteState.reset();
            return selection;
        }

        if (! mapContextMatchesCurrentParameters (map, current))
        {
            selection.fallbackActive = true;
            selection.mapStale = true;
            noteState.reset();
            return selection;
        }

        selection.mapUsable = true;
        const float globalF = std::clamp (map.global.allpassFreqHz,
                                          NoteMap::kAllpassFreqMinHz, NoteMap::kAllpassFreqMaxHz);
        const float globalQ = std::clamp (map.global.allpassQ,
                                          NoteMap::kAllpassQMin, NoteMap::kAllpassQMax);
        selection.targetFreqHz = globalF;
        selection.targetQ = globalQ;

        bool hasStateEntries = false;
        for (const auto& state : map.states)
            hasStateEntries = hasStateEntries || state.applied;

        if (hasStateEntries)
        {
            const float strength = std::clamp (std::isfinite (dynamicStrength) ? dynamicStrength : 1.0f,
                                               0.0f, 1.0f);
            auto applyStable = [&]
            {
                selection.targetFreqHz = noteState.stableFreqHz;
                selection.targetQ = noteState.stableQ;
                selection.targetDelayMs = noteState.stableDelayMs;
                selection.targetPolarityInvert = noteState.stablePolarityInvert;
                selection.targetStages = noteState.stableStages;
                selection.selectedMidi = noteState.stableMidiLabel;
                selection.usingLearnedNote = true;
            };

            if (freshFingerprint != nullptr && freshFingerprint->valid)
            {
                int nearest = -1;
                float nearestDistance = std::numeric_limits<float>::infinity();
                const uint64_t packed = freshFingerprint->pack();
                for (int i = 0; i < NotePhaseMapSnapshot::kMaxStates; ++i)
                {
                    const auto& state = map.states[(size_t) i];
                    if (! NoteMap::isValidConflictStateEntry (state) || state.fingerprint == 0)
                        continue;
                    const float distance = ConflictFingerprint::distance (packed, state.fingerprint);
                    if (distance < nearestDistance)
                    {
                        nearestDistance = distance;
                        nearest = i;
                    }
                }

                constexpr float kMaxFingerprintDistance = 0.18f;
                selection.fingerprintDistance = nearestDistance;
                if (nearest >= 0 && nearestDistance <= kMaxFingerprintDistance)
                {
                    const auto& state = map.states[(size_t) nearest];
                    selection.targetFreqHz = std::exp (std::log (globalF)
                        + (std::log (state.allpassFreqHz) - std::log (globalF)) * strength);
                    selection.targetQ = globalQ + (state.allpassQ - globalQ) * strength;
                    selection.targetDelayMs = current.delayMs + (state.delayMs - current.delayMs) * strength;
                    selection.targetPolarityInvert = strength > 0.0f ? state.polarityInvert : current.polarityInvert;
                    selection.targetStages = strength > 0.0f ? state.stages : current.allpassStages;
                    selection.selectedMidi = state.noteLabel;
                    selection.selectedState = nearest;
                    selection.usingLearnedNote = true;
                    noteState.hasStableEntry = true;
                    noteState.fallbackSamples = 0;
                    noteState.silentSamples = 0;
                    noteState.stableFreqHz = selection.targetFreqHz;
                    noteState.stableQ = selection.targetQ;
                    noteState.stableDelayMs = selection.targetDelayMs;
                    noteState.stablePolarityInvert = selection.targetPolarityInvert;
                    noteState.stableStages = selection.targetStages;
                    noteState.stableMidiLabel = selection.selectedMidi;
                    return selection;
                }

                selection.fallbackActive = true;
                const int fallbackHoldSamples = std::max (1, silenceResetSamples / 3);
                if (noteState.hasStableEntry && noteState.fallbackSamples < fallbackHoldSamples)
                {
                    applyStable();
                    noteState.fallbackSamples += std::max (0, blockSamples);
                }
                return selection;
            }

            noteState.silentSamples += std::max (0, blockSamples);
            if (noteState.hasStableEntry && noteState.silentSamples < std::max (1, silenceResetSamples))
            {
                applyStable();
                return selection;
            }

            noteState.reset();
            selection.fallbackActive = true;
            return selection;
        }

        if (! std::isfinite (trackedHz) || trackedHz <= 0.0f)
        {
            noteState.silentSamples += std::max (0, blockSamples);
            if (noteState.silentSamples >= std::max (1, silenceResetSamples))
                noteState.reset();
            selection.fallbackActive = true;
            return selection;
        }

        noteState.silentSamples = 0;
        const int midi = NoteQuantizer::updateWithHysteresis (trackedHz, noteState.lastMidi);
        selection.selectedMidi = midi;
        const NoteEntry* note = map.lookup (midi);
        if (note == nullptr || ! note->rotatorHelps
            || note->confidence < NoteMap::kMinRuntimeConfidence
            || note->timingConfidence < NoteMap::kMinRuntimeConfidence
            || ! NoteMap::isValidNoteEntry (*note) || ! NoteMap::allFieldsFinite (*note))
        {
            selection.fallbackActive = true;
            const int fallbackHoldSamples = std::max (1, silenceResetSamples / 3);
            if (noteState.hasStableEntry && noteState.fallbackSamples < fallbackHoldSamples)
            {
                selection.targetFreqHz = noteState.stableFreqHz;
                selection.targetQ = noteState.stableQ;
                noteState.fallbackSamples += std::max (0, blockSamples);
            }
            return selection;
        }

        const float strength = std::clamp (std::isfinite (dynamicStrength) ? dynamicStrength : 1.0f,
                                           0.0f, 1.0f);
        selection.targetFreqHz = std::exp (std::log (globalF)
                                         + (std::log (note->allpassFreqHz) - std::log (globalF)) * strength);
        selection.targetQ = globalQ + (note->allpassQ - globalQ) * strength;
        selection.usingLearnedNote = true;
        noteState.hasStableEntry = true;
        noteState.fallbackSamples = 0;
        noteState.stableFreqHz = selection.targetFreqHz;
        noteState.stableQ = selection.targetQ;
        return selection;
    }
}
