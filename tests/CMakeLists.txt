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
        test_Diagnostics.cpp
        test_ImageBuffer.cpp
        test_ImageExport.cpp
        test_Photo.cpp
        test_ProcessingPlan.cpp
        test_RawImport.cpp
        test_RoundTrip.cpp
        test_WhiteBalance.cpp)
arraw_enable_warnings(arraw-tests)

# The processing plan is engine-private: the order of the pointwise stages is
# not something a caller sets, but it is the thing most worth testing directly
# (ADR 011), so the tests see the engine's own headers.
target_include_directories(arraw-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/core)

# The integration tests read committed fixtures (see tests/fixtures/README.md).
# Baking the directory in beats resolving it against the working directory, so
# the suite behaves the same from ctest, from an IDE, and from a shell.
target_compile_definitions(arraw-tests
        PRIVATE ARRAW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# Catch2 (not Catch2WithMain): main.cpp runs the session inside a QCoreApplication.
target_link_libraries(arraw-tests PRIVATE arraw arraw-cli-core Catch2::Catch2 Qt6::Gui)
set_target_properties(arraw-tests PROPERTIES AUTOMOC OFF)

# Registers each TEST_CASE with CTest individually, so `ctest -R` can select one.
catch_discover_tests(arraw-tests)
