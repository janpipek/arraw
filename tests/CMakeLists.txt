# Catch2 v3: prefer a system install, fall back to a pinned source build so the
# suite still configures on a machine (or runner) without one.
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.13.0
            GIT_SHALLOW ON)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

include(Catch)

add_executable(arraw-tests
        main.cpp
        support/ImageCompare.cpp
        support/TestImages.cpp
        test_Cli.cpp
        test_ColorEncoding.cpp
        test_Develop.cpp
        test_Geometry.cpp
        test_Diagnostics.cpp
        test_GpuContext.cpp
        test_GpuDevice.cpp
        test_ImageBuffer.cpp
        test_ImageExport.cpp
        test_Photo.cpp
        test_ProcessingPlan.cpp
        test_RenderCheckpoint.cpp
        test_RawImport.cpp
        test_RoundTrip.cpp
        test_Tone.cpp
        test_WhiteBalance.cpp)
arraw_enable_warnings(arraw-tests)

# The processing plan is engine-private: the order of the pointwise stages is
# not something a caller sets, but it is the thing most worth testing directly
# (ADR 011), so the tests see the engine's own headers.
target_include_directories(arraw-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu)

# The integration tests read committed fixtures (see tests/fixtures/README.md).
# Baking the directory in beats resolving it against the working directory, so
# the suite behaves the same from ctest, from an IDE, and from a shell.
target_compile_definitions(arraw-tests
        PRIVATE ARRAW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# Catch2 (not Catch2WithMain): main.cpp runs the session inside a QCoreApplication.
# GuiPrivate for QRhi, which test_GpuDevice drives directly on its Null backend:
# the one place outside arraw-gpu that names it, because the failures it covers
# cannot be caused through GpuContext.
target_link_libraries(arraw-tests
        PRIVATE arraw arraw-gpu arraw-cli-core Catch2::Catch2 Qt6::Gui Qt6::GuiPrivate)
set_target_properties(arraw-tests PROPERTIES AUTOMOC OFF)

# Registers each TEST_CASE with CTest individually, so `ctest -R` can select one.
catch_discover_tests(arraw-tests)

# arraw's headless platform, in its own executable: it needs a QGuiApplication
# on that platform, and the suite above needs its QCoreApplication. Linux only,
# as the platform is. A runner without a Vulkan driver skips the device cases,
# and a software one (lavapipe) passes them: the platform is under test, not
# the hardware.
if(TARGET arraw-headless-platform)
    add_executable(arraw-headless-tests
            headless/main.cpp
            headless/test_Headless.cpp)
    arraw_enable_warnings(arraw-headless-tests)
    target_link_libraries(arraw-headless-tests
            PRIVATE arraw arraw-gpu arraw-headless-platform Catch2::Catch2 Qt6::Gui)
    set_target_properties(arraw-headless-tests PROPERTIES AUTOMOC OFF)
    # Catch2 exits 4 for a skipped test. Recent Catch.cmake tells CTest so
    # itself; older ones, such as Ubuntu 24.04's 3.4, would report a failure.
    catch_discover_tests(arraw-headless-tests PROPERTIES SKIP_RETURN_CODE 4)
endif()

# The real arraw-cli binary, for what only main() decides: which Qt application
# is made, when, and from which arguments. A platform that does not exist makes
# Qt abort the moment one is loaded, so a command that exits cleanly under it
# never loaded one.
set(arraw_expect_exit ${CMAKE_CURRENT_SOURCE_DIR}/cli/ExpectExit.cmake)
set(arraw_no_platform "QT_QPA_PLATFORM=arraw-no-such-platform")
add_test(NAME arraw-cli.help-loads-no-platform
        COMMAND ${CMAKE_COMMAND} -DEXPECTED_EXIT=0 -P ${arraw_expect_exit}
        -- $<TARGET_FILE:arraw-cli> --help)
add_test(NAME arraw-cli.usage-error-loads-no-platform
        COMMAND ${CMAKE_COMMAND} -DEXPECTED_EXIT=2 -DEXPECTED_ERR=--size -P ${arraw_expect_exit}
        -- $<TARGET_FILE:arraw-cli> gpu-test --size 0)
set_tests_properties(arraw-cli.help-loads-no-platform arraw-cli.usage-error-loads-no-platform
        PROPERTIES ENVIRONMENT ${arraw_no_platform})
# An operand after `--` is an input, even one Qt would take for its own option.
add_test(NAME arraw-cli.escaped-operand-is-an-input
        COMMAND ${CMAKE_COMMAND} -DEXPECTED_EXIT=1 -DEXPECTED_ERR=-reverse -P ${arraw_expect_exit}
        -- $<TARGET_FILE:arraw-cli> export -o ${CMAKE_CURRENT_BINARY_DIR} -- -reverse)

# Every executable CTest runs, so that one build step brings them all up
# to date: `just test` builds this, and a target CTest registers but nothing
# builds would fail as "not built", or run stale.
add_custom_target(arraw-test-binaries)
add_dependencies(arraw-test-binaries arraw-tests arraw-cli)
if(TARGET arraw-headless-tests)
    add_dependencies(arraw-test-binaries arraw-headless-tests)
endif()
