#pragma once

#include "LegacyDynamicCompatibilityRuntime.h"

// Transitional API facade. New DynamicStateMap runtime code must include its
// own contracts rather than this legacy compatibility header.
using CorrectionMode = LegacyDynamicCompatibility::CorrectionMode;
using RuntimeBaseSettings = LegacyDynamicCompatibility::RuntimeBaseSettings;
using DynamicNoteState = LegacyDynamicCompatibility::DynamicNoteState;
using DynamicRuntimeSelection = LegacyDynamicCompatibility::DynamicRuntimeSelection;

using LegacyDynamicCompatibility::isStructurallyValidRuntimeMap;
using LegacyDynamicCompatibility::mapContextMatchesCurrentParameters;
using LegacyDynamicCompatibility::selectDynamicRuntime;
