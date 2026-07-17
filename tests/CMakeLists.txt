add_executable(KickLockDspTests
    DspCoreTests.cpp
    DspTestMain.cpp
    DynamicModeTests.cpp
    DynamicStateMapTests.cpp
    DynamicFingerprintTests.cpp
    DynamicPackageMorpherTests.cpp
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
