add_executable(KickLockDspTests
    DspTests.cpp
)

target_include_directories(KickLockDspTests
    PRIVATE
        ../src/dsp
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
        juce::juce_core
        juce::juce_dsp
)

add_test(NAME KickLockDspTests COMMAND KickLockDspTests)
