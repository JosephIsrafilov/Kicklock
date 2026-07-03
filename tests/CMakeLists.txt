add_executable(KickLockDspTests
    DspTests.cpp
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
