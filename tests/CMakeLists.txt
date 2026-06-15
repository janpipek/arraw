# Catch2 v3: prefer a system install, fall back to FetchContent
# (see DESIGN.md "Testing Strategy").
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

add_executable(arraw_tests
    test_ImagePipeline.cpp
    test_XmpSidecar.cpp
    test_ColorManagement.cpp
    test_RawProcessor.cpp
    test_GoldenImages.cpp
    test_FilmStripModel.cpp
    test_FilmStripLayout.cpp
    test_FieldSpec.cpp
    test_LocalAdjustment.cpp
    test_LocalAdjustmentPanel.cpp
)

target_link_libraries(arraw_tests PRIVATE arraw_core Catch2::Catch2WithMain)

target_compile_definitions(arraw_tests PRIVATE
    ARRAW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ARRAW_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

include(Catch)
catch_discover_tests(arraw_tests)
