# Downloads a pinned pluginval binary for the host platform and registers a
# CTest test that validates the built VST3. Skipped cleanly if the platform is
# unsupported. Override the binary with -DPLUGINVAL_EXECUTABLE=/path/to/pluginval
# to use a locally-installed copy instead of downloading.

set(PLUGINVAL_VERSION "v1.0.4" CACHE STRING "pluginval release tag to download")
set(PLUGINVAL_EXECUTABLE "" CACHE FILEPATH "Path to an existing pluginval binary (skips download)")
set(PLUGINVAL_STRICTNESS "10" CACHE STRING "pluginval strictness level 1-10")

# Resolve the platform-specific release asset and the path to the executable
# inside the extracted archive.
if(NOT PLUGINVAL_EXECUTABLE)
    if(WIN32)
        set(_pv_asset "pluginval_Windows.zip")
        set(_pv_relpath "pluginval.exe")
    elseif(APPLE)
        set(_pv_asset "pluginval_macOS.zip")
        set(_pv_relpath "pluginval.app/Contents/MacOS/pluginval")
    elseif(UNIX)
        set(_pv_asset "pluginval_Linux.zip")
        set(_pv_relpath "pluginval")
    else()
        message(STATUS "pluginval: unsupported platform, skipping validation target")
        return()
    endif()

    include(FetchContent)
    FetchContent_Declare(
        pluginval
        URL "https://github.com/Tracktion/pluginval/releases/download/${PLUGINVAL_VERSION}/${_pv_asset}"
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    FetchContent_MakeAvailable(pluginval)

    set(PLUGINVAL_EXECUTABLE "${pluginval_SOURCE_DIR}/${_pv_relpath}")

    # The macOS/Linux binaries ship without an executable bit after extraction.
    if(UNIX)
        file(CHMOD "${PLUGINVAL_EXECUTABLE}"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE
                        WORLD_READ WORLD_EXECUTE)
    endif()
endif()

# Validate the VST3. $<TARGET_FILE:KickLock_VST3> resolves to the built module
# inside the .vst3 bundle for the active configuration.
add_test(
    NAME pluginval_vst3
    COMMAND "${PLUGINVAL_EXECUTABLE}"
            --strictness-level ${PLUGINVAL_STRICTNESS}
            --validate-in-process
            --skip-gui-tests
            --timeout-ms 300000
            --validate "$<TARGET_FILE:KickLock_VST3>"
)

# pluginval returns non-zero on validation failure; make that a test failure.
set_tests_properties(pluginval_vst3 PROPERTIES
    FAIL_REGULAR_EXPRESSION "\\*\\*\\* FAILED"
    TIMEOUT 360
)

message(STATUS "pluginval: validation test registered (${PLUGINVAL_EXECUTABLE})")
