#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

#include <juce_core/juce_core.h>

#include "DynamicStateMap.h"

namespace DynamicStateMapKeys
{
    inline constexpr const char* tree = "KLDynamicStateMap";
    inline constexpr const char* state = "State";
    inline constexpr const char* schemaVersion = "schemaVersion";
    inline constexpr const char* extractorVersion = "extractorVersion";
    inline constexpr const char* valid = "valid";
    inline constexpr const char* nextStateId = "nextStateId";

    inline constexpr const char* globalBaseDelayMs = "globalBaseDelayMs";
    inline constexpr const char* polarityInvert = "polarityInvert";
    inline constexpr const char* crossoverEnabled = "crossoverEnabled";
    inline constexpr const char* crossoverHz = "crossoverHz";
    inline constexpr const char* allpassEnabled = "allpassEnabled";
    inline constexpr const char* globalAllpassFreqHz = "globalAllpassFreqHz";
    inline constexpr const char* globalAllpassQ = "globalAllpassQ";
    inline constexpr const char* allpassStages = "allpassStages";
    inline constexpr const char* delayInterpolationIndex = "delayInterpolationIndex";
    inline constexpr const char* learnedSampleRate = "learnedSampleRate";

    inline constexpr const char* calibrationValid = "calibrationValid";
    inline constexpr const char* absoluteDistanceThreshold = "absoluteDistanceThreshold";
    inline constexpr const char* ambiguityMargin = "ambiguityMargin";

    inline constexpr const char* diagnostic = "diagnostic";
    inline constexpr const char* analyzedHitCount = "analyzedHitCount";
    inline constexpr const char* rejectedHitCount = "rejectedHitCount";
    inline constexpr const char* unstableHitCount = "unstableHitCount";
    inline constexpr const char* repeatableClusterCount = "repeatableClusterCount";

    inline constexpr const char* stateId = "stateId";
    inline constexpr const char* fingerprintValid = "fingerprintValid";
    inline constexpr const char* fingerprintFeatureCount = "fingerprintFeatureCount";
    inline constexpr const char* fingerprintFeaturePrefix = "fingerprintFeature";
    inline constexpr const char* hasLearnedPackage = "hasLearnedPackage";
    inline constexpr const char* learnedDelayDeltaMs = "learnedDelayDeltaMs";
    inline constexpr const char* learnedAllpassFreqHz = "learnedAllpassFreqHz";
    inline constexpr const char* learnedAllpassQ = "learnedAllpassQ";
    inline constexpr const char* hasManualBasePackage = "hasManualBasePackage";
    inline constexpr const char* manualBaseDelayDeltaMs = "manualBaseDelayDeltaMs";
    inline constexpr const char* manualBaseAllpassFreqHz = "manualBaseAllpassFreqHz";
    inline constexpr const char* manualBaseAllpassQ = "manualBaseAllpassQ";
    inline constexpr const char* delayTrimMs = "delayTrimMs";
    inline constexpr const char* frequencyTrimSemitones = "frequencyTrimSemitones";
    inline constexpr const char* logPoleDampingTrim = "logPoleDampingTrim";
    inline constexpr const char* origin = "origin";
    inline constexpr const char* evidence = "evidence";
    inline constexpr const char* enabled = "enabled";
    inline constexpr const char* bypassed = "bypassed";
    inline constexpr const char* hitCount = "hitCount";
    inline constexpr const char* repeatability = "repeatability";
    inline constexpr const char* ambiguity = "ambiguity";
    inline constexpr const char* hasLikelyMidi = "hasLikelyMidi";
    inline constexpr const char* likelyMidi = "likelyMidi";
    inline constexpr const char* hasLikelyPitchHz = "hasLikelyPitchHz";
    inline constexpr const char* likelyPitchHz = "likelyPitchHz";
    inline constexpr const char* correctionPolicy = "correctionPolicy";
    inline constexpr const char* policyRejectionReason = "policyRejectionReason";
}

namespace DynamicStateSerializationDetail
{
    inline juce::Identifier id (const char* name)
    {
        return juce::Identifier (name);
    }

    inline juce::Identifier id (const juce::String& name)
    {
        return juce::Identifier (name);
    }

    inline juce::String fingerprintFeatureKey (int feature)
    {
        return juce::String (DynamicStateMapKeys::fingerprintFeaturePrefix) + juce::String (feature);
    }

    inline bool isAsciiDigit (char character) noexcept
    {
        return character >= '0' && character <= '9';
    }

    inline bool getUtf8Range (const juce::String& text, const char*& begin, const char*& end) noexcept
    {
        begin = text.toRawUTF8();
        const auto byteCount = text.getNumBytesAsUTF8();
        end = begin + byteCount;
        return byteCount > 0;
    }

    inline bool hasStrictUnsignedDecimalSyntax (const char* begin, const char* end) noexcept
    {
        if (begin == end)
            return false;

        for (auto position = begin; position != end; ++position)
            if (! isAsciiDigit (*position))
                return false;

        return true;
    }

    inline bool hasStrictSignedDecimalSyntax (const char* begin, const char* end) noexcept
    {
        if (begin == end)
            return false;

        if (*begin == '-')
            ++begin;
        else if (*begin == '+')
            return false;

        return hasStrictUnsignedDecimalSyntax (begin, end);
    }

    inline bool hasStrictFiniteDecimalSyntax (const char* begin, const char* end) noexcept
    {
        if (begin == end)
            return false;

        if (*begin == '-' || *begin == '+')
            ++begin;
        if (begin == end)
            return false;

        bool hasDigits = false;
        while (begin != end && isAsciiDigit (*begin))
        {
            hasDigits = true;
            ++begin;
        }

        if (begin != end && *begin == '.')
        {
            ++begin;
            while (begin != end && isAsciiDigit (*begin))
            {
                hasDigits = true;
                ++begin;
            }
        }

        if (! hasDigits)
            return false;

        if (begin != end && (*begin == 'e' || *begin == 'E'))
        {
            ++begin;
            if (begin != end && (*begin == '-' || *begin == '+'))
                ++begin;

            const auto exponentBegin = begin;
            int exponentMagnitude = 0;
            while (begin != end && isAsciiDigit (*begin))
            {
                const int digit = *begin - '0';
                if (exponentMagnitude > (999 - digit) / 10)
                    return false;
                exponentMagnitude = exponentMagnitude * 10 + digit;
                ++begin;
            }
            if (begin == exponentBegin)
                return false;
        }

        return begin == end;
    }

    inline bool parseIntDecimal (const juce::String& text, int& value) noexcept
    {
        const char* begin = nullptr;
        const char* end = nullptr;
        if (! getUtf8Range (text, begin, end) || ! hasStrictSignedDecimalSyntax (begin, end))
            return false;

        const auto result = std::from_chars (begin, end, value, 10);
        return result.ec == std::errc {} && result.ptr == end;
    }

    inline bool parseUint32Decimal (const juce::String& text, uint32_t& value) noexcept
    {
        const char* begin = nullptr;
        const char* end = nullptr;
        if (! getUtf8Range (text, begin, end) || ! hasStrictUnsignedDecimalSyntax (begin, end))
            return false;

        const auto result = std::from_chars (begin, end, value, 10);
        return result.ec == std::errc {} && result.ptr == end;
    }

    template <typename Float>
    inline bool parseFiniteDecimal (const juce::String& text, Float& value) noexcept
    {
        const char* begin = nullptr;
        const char* end = nullptr;
        if (! getUtf8Range (text, begin, end) || ! hasStrictFiniteDecimalSyntax (begin, end))
            return false;

        const double parsed = text.getDoubleValue();
        if (! std::isfinite (parsed)
            || parsed < -(double) std::numeric_limits<Float>::max()
            || parsed > (double) std::numeric_limits<Float>::max())
            return false;

        value = (Float) parsed;
        return std::isfinite (value);
    }

    inline bool readBool (const juce::ValueTree& tree, const char* key, bool& value)
    {
        const auto property = id (key);
        if (! tree.hasProperty (property))
            return false;
        const auto raw = tree.getProperty (property);
        if (raw.isBool())
        {
            value = (bool) raw;
            return true;
        }
        if (! raw.isString())
            return false;

        const auto text = raw.toString();
        if (text == "1" || text == "true")
        {
            value = true;
            return true;
        }
        if (text == "0" || text == "false")
        {
            value = false;
            return true;
        }
        return false;
    }

    inline bool readFiniteFloat (const juce::ValueTree& tree, const char* key, float& value)
    {
        const auto property = id (key);
        if (! tree.hasProperty (property))
            return false;
        const auto raw = tree.getProperty (property);
        if (raw.isInt() || raw.isInt64() || raw.isDouble())
        {
            const double number = (double) raw;
            if (! std::isfinite (number)
                || number < -(double) std::numeric_limits<float>::max()
                || number > (double) std::numeric_limits<float>::max())
                return false;
            value = (float) number;
            return std::isfinite (value);
        }
        return raw.isString() && parseFiniteDecimal (raw.toString(), value);
    }

    inline bool readFiniteDouble (const juce::ValueTree& tree, const char* key, double& value)
    {
        const auto property = id (key);
        if (! tree.hasProperty (property))
            return false;
        const auto raw = tree.getProperty (property);
        if (raw.isInt() || raw.isInt64() || raw.isDouble())
        {
            value = (double) raw;
            return std::isfinite (value);
        }
        return raw.isString() && parseFiniteDecimal (raw.toString(), value);
    }

    inline bool readInt (const juce::ValueTree& tree, const char* key, int& value)
    {
        const auto property = id (key);
        if (! tree.hasProperty (property))
            return false;
        const auto raw = tree.getProperty (property);
        if (raw.isInt() || raw.isInt64())
        {
            const juce::int64 number = (juce::int64) raw;
            if (number < (juce::int64) std::numeric_limits<int>::min()
                || number > (juce::int64) std::numeric_limits<int>::max())
                return false;
            value = (int) number;
            return true;
        }
        return raw.isString() && parseIntDecimal (raw.toString(), value);
    }

    inline bool readUint32 (const juce::ValueTree& tree, const char* key, uint32_t& value)
    {
        const auto property = id (key);
        if (! tree.hasProperty (property))
            return false;
        const auto raw = tree.getProperty (property);
        if (raw.isInt() || raw.isInt64())
        {
            const juce::int64 number = (juce::int64) raw;
            if (number < 0 || (uint64_t) number > (uint64_t) std::numeric_limits<uint32_t>::max())
                return false;
            value = (uint32_t) number;
            return true;
        }
        return raw.isString() && parseUint32Decimal (raw.toString(), value);
    }

    inline bool parseUint64Decimal (const juce::String& text, uint64_t& value) noexcept
    {
        if (text.isEmpty())
            return false;
        uint64_t parsed = 0;
        for (int i = 0; i < text.length(); ++i)
        {
            const auto character = text[i];
            if (character < '0' || character > '9')
                return false;
            const auto digit = (uint64_t) (character - '0');
            if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10u)
                return false;
            parsed = parsed * 10u + digit;
        }
        value = parsed;
        return true;
    }

    inline juce::String formatUint64Decimal (uint64_t value)
    {
        char digits[21] {};
        char* position = digits + 20;
        *position = '\0';

        do
        {
            *--position = (char) ('0' + value % 10u);
            value /= 10u;
        }
        while (value != 0u);

        return juce::String (position);
    }

    inline bool readUint64String (const juce::ValueTree& tree, const char* key, uint64_t& value)
    {
        const auto property = id (key);
        if (! tree.hasProperty (property))
            return false;
        const auto raw = tree.getProperty (property);
        return raw.isString() && parseUint64Decimal (raw.toString(), value);
    }

    inline void writePackage (juce::ValueTree& tree,
                              const char* delayKey,
                              const char* frequencyKey,
                              const char* qKey,
                              const DynamicZonePackage& package)
    {
        tree.setProperty (id (delayKey), package.delayDeltaMs, nullptr);
        tree.setProperty (id (frequencyKey), package.allpassFreqHz, nullptr);
        tree.setProperty (id (qKey), package.allpassQ, nullptr);
    }

    inline bool readPackage (const juce::ValueTree& tree,
                             const char* delayKey,
                             const char* frequencyKey,
                             const char* qKey,
                             DynamicZonePackage& package)
    {
        return readFiniteFloat (tree, delayKey, package.delayDeltaMs)
            && readFiniteFloat (tree, frequencyKey, package.allpassFreqHz)
            && readFiniteFloat (tree, qKey, package.allpassQ);
    }
}

inline juce::ValueTree dynamicStateMapToValueTree (const DynamicStateMap& map)
{
    using namespace DynamicStateSerializationDetail;

    juce::ValueTree valueTree { id (DynamicStateMapKeys::tree) };
    valueTree.setProperty (id (DynamicStateMapKeys::schemaVersion), (int) map.schemaVersion, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::extractorVersion), (int) map.fingerprintExtractorVersion, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::valid), map.valid, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::nextStateId),
                           formatUint64Decimal (map.nextStateId), nullptr);

    valueTree.setProperty (id (DynamicStateMapKeys::globalBaseDelayMs), map.globalBase.globalBaseDelayMs, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::polarityInvert), map.globalBase.polarityInvert, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::crossoverEnabled), map.globalBase.crossoverEnabled, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::crossoverHz), map.globalBase.crossoverHz, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::allpassEnabled), map.globalBase.allpassEnabled, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::globalAllpassFreqHz), map.globalBase.globalAllpassFreqHz, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::globalAllpassQ), map.globalBase.globalAllpassQ, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::allpassStages), map.globalBase.allpassStages, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::delayInterpolationIndex), map.globalBase.delayInterpolationIndex, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::learnedSampleRate), map.globalBase.learnedSampleRate, nullptr);

    valueTree.setProperty (id (DynamicStateMapKeys::calibrationValid), map.calibration.valid, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::absoluteDistanceThreshold), map.calibration.absoluteDistanceThreshold, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::ambiguityMargin), map.calibration.ambiguityMargin, nullptr);

    valueTree.setProperty (id (DynamicStateMapKeys::diagnostic), (int) map.diagnostics.diagnostic, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::analyzedHitCount), (juce::int64) map.diagnostics.analyzedHitCount, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::rejectedHitCount), (juce::int64) map.diagnostics.rejectedHitCount, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::unstableHitCount), (juce::int64) map.diagnostics.unstableHitCount, nullptr);
    valueTree.setProperty (id (DynamicStateMapKeys::repeatableClusterCount), (juce::int64) map.diagnostics.repeatableClusterCount, nullptr);

    std::array<bool, DynamicStateMapContract::kMaxPersistentStates> writtenStates {};
    for (int orderedIndex = 0; orderedIndex < DynamicStateMapContract::kMaxPersistentStates; ++orderedIndex)
    {
        int sourceIndex = -1;
        for (int candidateIndex = 0;
             candidateIndex < DynamicStateMapContract::kMaxPersistentStates;
             ++candidateIndex)
        {
            const auto& candidate = map.states[(size_t) candidateIndex];
            if (candidate.occupied && ! writtenStates[(size_t) candidateIndex]
                && (sourceIndex < 0
                    || candidate.stableStateId < map.states[(size_t) sourceIndex].stableStateId))
                sourceIndex = candidateIndex;
        }

        if (sourceIndex < 0)
            break;

        const auto& sourceState = map.states[(size_t) sourceIndex];

        juce::ValueTree child { id (DynamicStateMapKeys::state) };
        child.setProperty (id (DynamicStateMapKeys::stateId),
                           formatUint64Decimal (sourceState.stableStateId), nullptr);
        child.setProperty (id (DynamicStateMapKeys::fingerprintValid), sourceState.fingerprint.valid, nullptr);
        child.setProperty (id (DynamicStateMapKeys::fingerprintFeatureCount), (int) sourceState.fingerprint.featureCount, nullptr);
        for (int feature = 0; feature < DynamicStateMapContract::kMaxFingerprintFeatures; ++feature)
            child.setProperty (id (fingerprintFeatureKey (feature)),
                               sourceState.fingerprint.features[(size_t) feature], nullptr);

        child.setProperty (id (DynamicStateMapKeys::hasLearnedPackage), sourceState.hasLearnedPackage, nullptr);
        if (sourceState.hasLearnedPackage)
            writePackage (child, DynamicStateMapKeys::learnedDelayDeltaMs,
                          DynamicStateMapKeys::learnedAllpassFreqHz, DynamicStateMapKeys::learnedAllpassQ,
                          sourceState.learnedPackage);
        child.setProperty (id (DynamicStateMapKeys::hasManualBasePackage), sourceState.hasManualBasePackage, nullptr);
        if (sourceState.hasManualBasePackage)
            writePackage (child, DynamicStateMapKeys::manualBaseDelayDeltaMs,
                          DynamicStateMapKeys::manualBaseAllpassFreqHz, DynamicStateMapKeys::manualBaseAllpassQ,
                          sourceState.manualBasePackage);

        child.setProperty (id (DynamicStateMapKeys::delayTrimMs), sourceState.manualTrim.delayTrimMs, nullptr);
        child.setProperty (id (DynamicStateMapKeys::frequencyTrimSemitones), sourceState.manualTrim.frequencyTrimSemitones, nullptr);
        child.setProperty (id (DynamicStateMapKeys::logPoleDampingTrim), sourceState.manualTrim.logPoleDampingTrim, nullptr);
        child.setProperty (id (DynamicStateMapKeys::origin), (int) sourceState.origin, nullptr);
        child.setProperty (id (DynamicStateMapKeys::evidence), (int) sourceState.evidence, nullptr);
        child.setProperty (id (DynamicStateMapKeys::enabled), sourceState.enabled, nullptr);
        child.setProperty (id (DynamicStateMapKeys::bypassed), sourceState.bypassed, nullptr);
        child.setProperty (id (DynamicStateMapKeys::hitCount), (juce::int64) sourceState.hitCount, nullptr);
        child.setProperty (id (DynamicStateMapKeys::repeatability), sourceState.repeatability, nullptr);
        child.setProperty (id (DynamicStateMapKeys::ambiguity), sourceState.ambiguity, nullptr);
        child.setProperty (id (DynamicStateMapKeys::hasLikelyMidi), sourceState.hasLikelyMidi, nullptr);
        if (sourceState.hasLikelyMidi)
            child.setProperty (id (DynamicStateMapKeys::likelyMidi), sourceState.likelyMidi, nullptr);
        child.setProperty (id (DynamicStateMapKeys::hasLikelyPitchHz), sourceState.hasLikelyPitchHz, nullptr);
        if (sourceState.hasLikelyPitchHz)
            child.setProperty (id (DynamicStateMapKeys::likelyPitchHz), sourceState.likelyPitchHz, nullptr);
        child.setProperty (id (DynamicStateMapKeys::correctionPolicy), (int) sourceState.correctionPolicy, nullptr);
        child.setProperty (id (DynamicStateMapKeys::policyRejectionReason), (int) sourceState.policyRejectionReason, nullptr);
        valueTree.appendChild (child, nullptr);
        writtenStates[(size_t) sourceIndex] = true;
    }

    return valueTree;
}

inline DynamicStateMap dynamicStateMapFromValueTree (const juce::ValueTree& tree) noexcept
{
    using namespace DynamicStateSerializationDetail;

    try
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        if (! tree.isValid() || ! tree.hasType (id (DynamicStateMapKeys::tree)))
            return map;

        int schemaValue = 0;
        int extractorVersionValue = 0;
        if (! readInt (tree, DynamicStateMapKeys::schemaVersion, schemaValue)
            || ! readInt (tree, DynamicStateMapKeys::extractorVersion, extractorVersionValue)
            || schemaValue != (int) DynamicStateMapContract::kSchemaVersion
            || extractorVersionValue != (int) DynamicStateMapContract::kExtractorVersion)
            return map;

        map.schemaVersion = (uint32_t) schemaValue;
        map.fingerprintExtractorVersion = (uint32_t) extractorVersionValue;
        if (! readBool (tree, DynamicStateMapKeys::valid, map.valid)
            || ! readUint64String (tree, DynamicStateMapKeys::nextStateId, map.nextStateId)
            || ! readFiniteFloat (tree, DynamicStateMapKeys::globalBaseDelayMs, map.globalBase.globalBaseDelayMs)
            || ! readBool (tree, DynamicStateMapKeys::polarityInvert, map.globalBase.polarityInvert)
            || ! readBool (tree, DynamicStateMapKeys::crossoverEnabled, map.globalBase.crossoverEnabled)
            || ! readFiniteFloat (tree, DynamicStateMapKeys::crossoverHz, map.globalBase.crossoverHz)
            || ! readBool (tree, DynamicStateMapKeys::allpassEnabled, map.globalBase.allpassEnabled)
            || ! readFiniteFloat (tree, DynamicStateMapKeys::globalAllpassFreqHz, map.globalBase.globalAllpassFreqHz)
            || ! readFiniteFloat (tree, DynamicStateMapKeys::globalAllpassQ, map.globalBase.globalAllpassQ)
            || ! readInt (tree, DynamicStateMapKeys::allpassStages, map.globalBase.allpassStages)
            || ! readInt (tree, DynamicStateMapKeys::delayInterpolationIndex, map.globalBase.delayInterpolationIndex)
            || ! readFiniteDouble (tree, DynamicStateMapKeys::learnedSampleRate, map.globalBase.learnedSampleRate)
            || ! readBool (tree, DynamicStateMapKeys::calibrationValid, map.calibration.valid)
            || ! readFiniteFloat (tree, DynamicStateMapKeys::absoluteDistanceThreshold, map.calibration.absoluteDistanceThreshold)
            || ! readFiniteFloat (tree, DynamicStateMapKeys::ambiguityMargin, map.calibration.ambiguityMargin))
            return makeEmptyDynamicStateMap();

        int diagnosticValue = 0;
        if (! readInt (tree, DynamicStateMapKeys::diagnostic, diagnosticValue)
            || diagnosticValue < (int) DynamicMapDiagnostic::None
            || diagnosticValue > (int) DynamicMapDiagnostic::NoConfidentAutoFix
            || ! readUint32 (tree, DynamicStateMapKeys::analyzedHitCount, map.diagnostics.analyzedHitCount)
            || ! readUint32 (tree, DynamicStateMapKeys::rejectedHitCount, map.diagnostics.rejectedHitCount)
            || ! readUint32 (tree, DynamicStateMapKeys::unstableHitCount, map.diagnostics.unstableHitCount)
            || ! readUint32 (tree, DynamicStateMapKeys::repeatableClusterCount, map.diagnostics.repeatableClusterCount))
            return makeEmptyDynamicStateMap();
        map.diagnostics.diagnostic = (DynamicMapDiagnostic) diagnosticValue;

        int stateCount = 0;
        for (int childIndex = 0; childIndex < tree.getNumChildren(); ++childIndex)
        {
            const auto child = tree.getChild (childIndex);
            if (! child.hasType (id (DynamicStateMapKeys::state)))
                continue;
            if (++stateCount > DynamicStateMapContract::kMaxPersistentStates)
                return makeEmptyDynamicStateMap();

            DynamicState parsed;
            parsed.occupied = true;
            int originValue = 0;
            int evidenceValue = 0;
            int featureCount = 0;
            if (! readUint64String (child, DynamicStateMapKeys::stateId, parsed.stableStateId)
                || ! readBool (child, DynamicStateMapKeys::fingerprintValid, parsed.fingerprint.valid)
                || ! readInt (child, DynamicStateMapKeys::fingerprintFeatureCount, featureCount)
                || featureCount < 0 || featureCount > DynamicStateMapContract::kMaxFingerprintFeatures
                || ! readBool (child, DynamicStateMapKeys::hasLearnedPackage, parsed.hasLearnedPackage)
                || ! readBool (child, DynamicStateMapKeys::hasManualBasePackage, parsed.hasManualBasePackage)
                || ! readFiniteFloat (child, DynamicStateMapKeys::delayTrimMs, parsed.manualTrim.delayTrimMs)
                || ! readFiniteFloat (child, DynamicStateMapKeys::frequencyTrimSemitones, parsed.manualTrim.frequencyTrimSemitones)
                || ! readFiniteFloat (child, DynamicStateMapKeys::logPoleDampingTrim, parsed.manualTrim.logPoleDampingTrim)
                || ! readInt (child, DynamicStateMapKeys::origin, originValue)
                || ! readInt (child, DynamicStateMapKeys::evidence, evidenceValue)
                || ! readBool (child, DynamicStateMapKeys::enabled, parsed.enabled)
                || ! readBool (child, DynamicStateMapKeys::bypassed, parsed.bypassed)
                || ! readUint32 (child, DynamicStateMapKeys::hitCount, parsed.hitCount)
                || ! readFiniteFloat (child, DynamicStateMapKeys::repeatability, parsed.repeatability)
                || ! readFiniteFloat (child, DynamicStateMapKeys::ambiguity, parsed.ambiguity)
                || ! readBool (child, DynamicStateMapKeys::hasLikelyMidi, parsed.hasLikelyMidi)
                || ! readBool (child, DynamicStateMapKeys::hasLikelyPitchHz, parsed.hasLikelyPitchHz))
                return makeEmptyDynamicStateMap();

            parsed.fingerprint.featureCount = (uint8_t) featureCount;
            for (int feature = 0; feature < DynamicStateMapContract::kMaxFingerprintFeatures; ++feature)
            {
                const auto key = fingerprintFeatureKey (feature);
                if (! readFiniteFloat (child, key.toRawUTF8(), parsed.fingerprint.features[(size_t) feature]))
                    return makeEmptyDynamicStateMap();
            }

            if (originValue < (int) DynamicStateOrigin::Auto || originValue > (int) DynamicStateOrigin::Manual
                || evidenceValue < (int) DynamicStateEvidence::Candidate
                || evidenceValue > (int) DynamicStateEvidence::Stable)
                return makeEmptyDynamicStateMap();
            parsed.origin = (DynamicStateOrigin) originValue;
            parsed.evidence = (DynamicStateEvidence) evidenceValue;

            if (parsed.hasLearnedPackage
                && ! readPackage (child, DynamicStateMapKeys::learnedDelayDeltaMs,
                                  DynamicStateMapKeys::learnedAllpassFreqHz,
                                  DynamicStateMapKeys::learnedAllpassQ, parsed.learnedPackage))
                return makeEmptyDynamicStateMap();
            if (parsed.hasManualBasePackage
                && ! readPackage (child, DynamicStateMapKeys::manualBaseDelayDeltaMs,
                                  DynamicStateMapKeys::manualBaseAllpassFreqHz,
                                  DynamicStateMapKeys::manualBaseAllpassQ, parsed.manualBasePackage))
                return makeEmptyDynamicStateMap();
            if (parsed.hasLikelyMidi
                && ! readInt (child, DynamicStateMapKeys::likelyMidi, parsed.likelyMidi))
                return makeEmptyDynamicStateMap();
            if (parsed.hasLikelyPitchHz
                && ! readFiniteFloat (child, DynamicStateMapKeys::likelyPitchHz, parsed.likelyPitchHz))
                return makeEmptyDynamicStateMap();

            // Optional fields (absent in presets saved before this policy
            // existed): default to the historical always-Global behavior
            // rather than failing the whole map.
            int correctionPolicyValue = (int) DynamicCorrectionPolicy::GlobalFallback;
            if (child.hasProperty (id (DynamicStateMapKeys::correctionPolicy)))
            {
                if (! readInt (child, DynamicStateMapKeys::correctionPolicy, correctionPolicyValue)
                    || correctionPolicyValue < (int) DynamicCorrectionPolicy::LearnedState
                    || correctionPolicyValue > (int) DynamicCorrectionPolicy::NeutralSafe)
                    return makeEmptyDynamicStateMap();
            }
            parsed.correctionPolicy = (DynamicCorrectionPolicy) correctionPolicyValue;

            int policyRejectionReasonValue = (int) DynamicPolicyRejectionReason::None;
            if (child.hasProperty (id (DynamicStateMapKeys::policyRejectionReason)))
            {
                if (! readInt (child, DynamicStateMapKeys::policyRejectionReason, policyRejectionReasonValue)
                    || policyRejectionReasonValue < (int) DynamicPolicyRejectionReason::None
                    || policyRejectionReasonValue > (int) DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression)
                    return makeEmptyDynamicStateMap();
            }
            parsed.policyRejectionReason = (DynamicPolicyRejectionReason) policyRejectionReasonValue;

            map.states[(size_t) (stateCount - 1)] = parsed;
        }

        if (! map.valid)
            return makeEmptyDynamicStateMap();
        return isStructurallyValidDynamicStateMap (map) ? map : makeEmptyDynamicStateMap();
    }
    catch (...)
    {
        return makeEmptyDynamicStateMap();
    }
}
