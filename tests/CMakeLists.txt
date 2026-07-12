add_executable(KickLockDspTests
    DspCoreTests.cpp
    DspTestMain.cpp
    DynamicModeTests.cpp
    LearnCoreTests.cpp
    LearnPipelineTests.cpp
    OfflineReliabilityTests.cpp
    P10Tests.cpp
    ProcessorTests.cpp
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

add_test(NAME KickLockDspTests COMMAND KickLockDspTests)
add_test(NAME KickLockDspTests.DSP COMMAND KickLockDspTests DSP)
add_test(NAME KickLockDspTests.Processor COMMAND KickLockDspTests Processor)
add_test(NAME KickLockDspTests.P10 COMMAND KickLockDspTests P10)
add_test(NAME KickLockDspTests.Stabilization COMMAND KickLockDspTests Stabilization)
add_test(NAME KickLockDspTests.Phase1 COMMAND KickLockDspTests Phase1)
add_test(NAME KickLockDspTests.Phase2 COMMAND KickLockDspTests Phase2)
add_test(NAME KickLockDspTests.Phase3 COMMAND KickLockDspTests Phase3)
add_test(NAME KickLockDspTests.SyntheticMaterialValidation COMMAND KickLockDspTests SyntheticMaterialValidation)
add_test(NAME KickLockDspTests.UIHelpers COMMAND KickLockDspTests "UI Helpers")
