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
        test_ImageBuffer.cpp)
arraw_enable_warnings(arraw-tests)
target_link_libraries(arraw-tests PRIVATE arraw Catch2::Catch2WithMain)
set_target_properties(arraw-tests PROPERTIES AUTOMOC OFF)

# Registers each TEST_CASE with CTest individually, so `ctest -R` can select one.
catch_discover_tests(arraw-tests)
