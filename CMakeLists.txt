
cmake_minimum_required(VERSION 3.22)

# Avoid requiring a specific VS platform toolset which may not be installed on the system.
# Instruct MSBuild to use the host toolset for the generator instead of forcing v143.
if(MSVC AND NOT DEFINED CMAKE_GENERATOR_TOOLSET)
    set(CMAKE_GENERATOR_TOOLSET "host=x64")
endif()

project(KickLock VERSION 0.3.1)

# Only meaningful for Visual Studio generators; setting it under Makefiles/
# Ninja (Linux CI) makes every sub-configure — including JUCE's FetchContent
# population — fail with "generator platform not supported".
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_GENERATOR_PLATFORM x64)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.13
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(KickLock
    COMPANY_NAME "OpenSource"
    FORMATS VST3 AU Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Oskl
    PLUGIN_CODE Klck
    PRODUCT_NAME "KickLock"
)

target_sources(KickLock
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
        src/ui/Oscilloscope.cpp
        src/ui/SpectrumAnalyzer.cpp
)

juce_generate_juce_header(KickLock)

target_compile_definitions(KickLock
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(KickLock
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

enable_testing()
add_subdirectory(tests)

option(KICKLOCK_ENABLE_PLUGINVAL "Download pluginval and register a VST3 validation test" ON)
if(KICKLOCK_ENABLE_PLUGINVAL)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Pluginval.cmake)
endif()
