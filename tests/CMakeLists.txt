add_executable(KickLockDspTests
    DspCoreTests.cpp
    DspTestMain.cpp
    DynamicModeTests.cpp
    DynamicStateMapTests.cpp
    DynamicFingerprintTests.cpp
    DynamicPackageMorpherTests.cpp
    DynamicHotBranchEngineTests.cpp
    DynamicSelectorSchedulerTests.cpp
    DynamicProductionRuntimeTests.cpp
    DynamicLearnFormationTests.cpp
    DynamicStateMeasurementTests.cpp
    DynamicNeutralSafeTests.cpp
    LegacyDynamicCompatibilityTests.cpp
    DynamicRuntimeTests.cpp
    LearnCoreTests.cpp
    ConflictLocalizationTests.cpp
    ConflictStateTests.cpp
    RuntimeFingerprintTests.cpp
    LearnDiagnosticRootCauseTests.cpp
    LearnOrchestrationTests.cpp
    LearnPipelineTests.cpp
    OfflineReliabilityTests.cpp
    PitchOctaveRegressionTests.cpp
    P10Tests.cpp
    ProcessorTests.cpp
    RealAudioPluginIntegrationTests.cpp
    RuntimeTrustTests.cpp
    SyntheticMaterialValidationTests.cpp
    TransportQueueTests.cpp
    UiHelperTests.cpp
    DynamicWorkspaceTests.cpp
    DynamicReleaseFixture.cpp
    DynamicReleaseGateTests.cpp
    DynamicPerformanceGateTests.cpp
    DynamicStateEditTransactionTests.cpp
    DynamicStateEditProcessorTests.cpp
    DynamicStateEditAuditionTests.cpp
    DynamicRecentUnknownEventsTests.cpp
    DynamicManualTrimConversionTests.cpp
    DynamicStateEditThrottleTests.cpp
    DynamicStateInspectorHelpersTests.cpp
    DynamicFocusAndRecentUnknownsTests.cpp
    DynamicStateEditPersistenceTests.cpp
    DynamicStateEditRevertTests.cpp
    DynamicManualWorkflowE2ETests.cpp
    TestAllocationCounter.cpp
)

target_include_directories(KickLockDspTests
    PRIVATE
        ../src
        ../src/dsp
        ../src/util
        ${CMAKE_BINARY_DIR}/KickLock_artefacts/JuceLibraryCode
)

target_compile_features(KickLockDspTests
    PRIVATE
        cxx_std_20
)

set_target_properties(KickLockDspTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(KickLockDspTests
    PRIVATE
        KickLock
        juce::juce_core
        juce::juce_dsp
        juce::juce_audio_utils
)

if(MSVC)
    # LearnOrchestration intentionally keeps many lifecycle fixtures in one
    # JUCE UnitTest method; Debug builds retain those scoped locals in one frame.
    target_link_options(KickLockDspTests PRIVATE "/STACK:8388608")
    # The DSP reliability/performance fixtures exercise header-only analysis
    # code. Keep their Debug runtime representative without changing the
    # plugin's Debug optimization level or its debug information.
    target_compile_options(KickLockDspTests PRIVATE "$<$<CONFIG:Debug>:/O2>")
endif()

add_test(NAME KickLockDspTests COMMAND KickLockDspTests)

add_executable(KickLockFastTests
    DspTestMain.cpp
    DynamicStateMapTests.cpp
    DynamicFingerprintTests.cpp
    DynamicPackageMorpherTests.cpp
    DynamicHotBranchEngineTests.cpp
    DynamicSelectorSchedulerTests.cpp
    DynamicProductionRuntimeTests.cpp
    DynamicLearnFormationTests.cpp
    DynamicStateMeasurementTests.cpp
    DynamicNeutralSafeTests.cpp
    LegacyDynamicCompatibilityTests.cpp
    ProcessorTests.cpp
    UiHelperTests.cpp
    DynamicWorkspaceTests.cpp
    DynamicReleaseFixture.cpp
    DynamicReleaseGateTests.cpp
    DynamicStateEditTransactionTests.cpp
    DynamicStateEditProcessorTests.cpp
    DynamicStateEditAuditionTests.cpp
    DynamicRecentUnknownEventsTests.cpp
    DynamicManualTrimConversionTests.cpp
    DynamicStateEditThrottleTests.cpp
    DynamicStateInspectorHelpersTests.cpp
    DynamicFocusAndRecentUnknownsTests.cpp
    DynamicStateEditPersistenceTests.cpp
    DynamicStateEditRevertTests.cpp
    DynamicManualWorkflowE2ETests.cpp
    TestAllocationCounter.cpp
)

target_include_directories(KickLockFastTests
    PRIVATE
        ../src
        ../src/dsp
        ../src/util
        ${CMAKE_BINARY_DIR}/KickLock_artefacts/JuceLibraryCode
)

target_compile_features(KickLockFastTests PRIVATE cxx_std_20)
set_target_properties(KickLockFastTests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(KickLockFastTests
    PRIVATE
        KickLock
        juce::juce_core
        juce::juce_dsp
        juce::juce_audio_utils
)

if(MSVC)
    target_link_options(KickLockFastTests PRIVATE "/STACK:8388608")
    target_compile_options(KickLockFastTests PRIVATE "$<$<CONFIG:Debug>:/O2>")
endif()

# =============================================================================
# Real-audio acceptance harness: three dedicated executables (Smoke / Acceptance
# / Stress). These are deliberately NOT part of KickLockFastTests / the fast
# checkpoint suite - the full sample-rate/block-size stress matrix in particular
# is long-running. Each drives the actual production processor over WAV fixtures
# through a real injected AudioPlayHead; nothing about the selector or DSP is
# mocked.
# =============================================================================

set(KICKLOCK_AUDIO_HARNESS_SOURCES
    DspTestMain.cpp
    audio/AudioTestHarness.cpp
    audio/AudioFixtureGenerators.cpp
    DynamicReleaseFixture.cpp
    TestAllocationCounter.cpp
)

function(kicklock_add_audio_test target)
    add_executable(${target} ${KICKLOCK_AUDIO_HARNESS_SOURCES} ${ARGN})
    target_include_directories(${target}
        PRIVATE
            .
            ../src
            ../src/dsp
            ../src/util
            ${CMAKE_BINARY_DIR}/KickLock_artefacts/JuceLibraryCode
    )
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(${target}
        PRIVATE
            KickLock
            juce::juce_core
            juce::juce_dsp
            juce::juce_audio_utils
            juce::juce_audio_formats
    )
    if(MSVC)
        target_link_options(${target} PRIVATE "/STACK:8388608")
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Debug>:/O2>")
    endif()
endfunction()

kicklock_add_audio_test(KickLockAudioSmokeTests AudioSmokeTests.cpp)
kicklock_add_audio_test(KickLockAudioAcceptanceTests AudioAcceptanceTests.cpp)
kicklock_add_audio_test(KickLockAudioStressTests AudioStressTests.cpp)

# Compiled-plugin acceptance: this executable loads the generated VST3 through
# JUCE's VST3 host implementation. It must never substitute a direct processor.
add_executable(KickLockHostAcceptanceTests
    DspTestMain.cpp
    HostAcceptanceTests.cpp
    audio/AudioTestHarness.cpp
    audio/AudioFixtureGenerators.cpp
    DynamicReleaseFixture.cpp
    TestAllocationCounter.cpp
)
target_include_directories(KickLockHostAcceptanceTests PRIVATE . ../src ../src/dsp ../src/util
    ${CMAKE_BINARY_DIR}/KickLock_artefacts/JuceLibraryCode)
target_compile_features(KickLockHostAcceptanceTests PRIVATE cxx_std_20)
target_compile_definitions(KickLockHostAcceptanceTests PRIVATE JUCE_PLUGINHOST_VST3=1
    KICKLOCK_HOST_VST3_PATH="$<TARGET_FILE_DIR:KickLock_VST3>")
target_link_libraries(KickLockHostAcceptanceTests PRIVATE KickLock juce::juce_core juce::juce_dsp
    juce::juce_audio_utils juce::juce_audio_formats juce::juce_audio_processors)
if(MSVC)
    target_link_options(KickLockHostAcceptanceTests PRIVATE "/STACK:8388608")
    target_compile_options(KickLockHostAcceptanceTests PRIVATE "$<$<CONFIG:Debug>:/O2>")
endif()

# Deterministic editor/component acceptance. This is intentionally separate
# from the broad DSP suite so CI reports GUI lifecycle failures as their own
# release gate rather than burying them in a Phase category.
add_executable(KickLockGuiAcceptanceTests
    DspTestMain.cpp
    GuiAcceptanceTests.cpp
)
target_include_directories(KickLockGuiAcceptanceTests
    PRIVATE
        ../src
        ../src/dsp
        ../src/util
        ${CMAKE_BINARY_DIR}/KickLock_artefacts/JuceLibraryCode
)
target_compile_features(KickLockGuiAcceptanceTests PRIVATE cxx_std_20)
set_target_properties(KickLockGuiAcceptanceTests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(KickLockGuiAcceptanceTests
    PRIVATE
        KickLock
        juce::juce_core
        juce::juce_dsp
        juce::juce_audio_utils
)
if(MSVC)
    target_link_options(KickLockGuiAcceptanceTests PRIVATE "/STACK:8388608")
    target_compile_options(KickLockGuiAcceptanceTests PRIVATE "$<$<CONFIG:Debug>:/O2>")
endif()

add_test(NAME KickLockAudioSmokeTests COMMAND KickLockAudioSmokeTests)
add_test(NAME KickLockAudioAcceptanceTests COMMAND KickLockAudioAcceptanceTests)
add_test(NAME KickLockGuiAcceptanceTests COMMAND KickLockGuiAcceptanceTests)

# State input is supplied by arbitrary hosts and project files. Keep malformed
# input coverage in its own deterministic executable so its seed and failure
# diagnostics remain visible in release validation.
add_executable(KickLockMalformedStateTests
    DspTestMain.cpp
    MalformedStateTests.cpp
)
target_include_directories(KickLockMalformedStateTests
    PRIVATE
        ../src
        ../src/dsp
        ../src/util
        ${CMAKE_BINARY_DIR}/KickLock_artefacts/JuceLibraryCode
)
target_compile_features(KickLockMalformedStateTests PRIVATE cxx_std_20)
set_target_properties(KickLockMalformedStateTests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(KickLockMalformedStateTests
    PRIVATE
        KickLock
        juce::juce_core
        juce::juce_dsp
        juce::juce_audio_utils
)
if(MSVC)
    target_link_options(KickLockMalformedStateTests PRIVATE "/STACK:8388608")
    target_compile_options(KickLockMalformedStateTests PRIVATE "$<$<CONFIG:Debug>:/O2>")
endif()
add_test(NAME KickLockMalformedStateTests COMMAND KickLockMalformedStateTests)
add_test(NAME KickLockAudioStressTests COMMAND KickLockAudioStressTests)
