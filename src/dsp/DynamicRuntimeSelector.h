#pragma once

#include <algorithm>
#include <cmath>

#include "NotePhaseMap.h"
#include "NoteQuantizer.h"

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
    int selectedMidi = -1;
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
    DynamicNoteState& noteState) noexcept
{
    DynamicRuntimeSelection selection;
    selection.targetFreqHz = std::clamp (std::isfinite (manualFreqHz) ? manualFreqHz : 50.0f,
                                         NoteMap::kAllpassFreqMinHz, NoteMap::kAllpassFreqMaxHz);
    selection.targetQ = std::clamp (std::isfinite (manualQ) ? manualQ : 0.7f,
                                    NoteMap::kAllpassQMin, NoteMap::kAllpassQMax);
    selection.targetDelayMs = current.delayMs;
    selection.targetPolarityInvert = current.polarityInvert;

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
        // Keep one known-good note briefly while tracker confidence flickers;
        // this avoids hopping between notes during a weak transient.
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
