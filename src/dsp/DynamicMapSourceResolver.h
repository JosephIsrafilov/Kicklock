#pragma once

#include <type_traits>

#include "DynamicStateMap.h"
#include "LegacyDynamicCompatibilityRuntime.h"

enum class DynamicMapSource
{
    NewDynamicStateMap,
    LegacyDynamicCompatibility,
    None
};

static_assert (std::is_trivially_copyable_v<DynamicMapSource>);

// Pure priority selection only. It does not render audio, mutate either map,
// acquire locks, or convert legacy KLNoteMap data.
inline DynamicMapSource resolveDynamicMapSource (const DynamicStateMap& dynamicStateMap,
                                                 const NotePhaseMapSnapshot& legacyMap) noexcept
{
    if (isRuntimeEligibleDynamicStateMap (dynamicStateMap))
        return DynamicMapSource::NewDynamicStateMap;
    if (LegacyDynamicCompatibility::isUsableMap (legacyMap))
        return DynamicMapSource::LegacyDynamicCompatibility;
    return DynamicMapSource::None;
}
