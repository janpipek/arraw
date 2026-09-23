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
target_link_libraries(arraw-tests PRIVATE arraw arraw-gpu arraw-cli-core Catch2::Catch2 Qt6::Gui)
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
