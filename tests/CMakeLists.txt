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
)

# ImageViewport loads shaders from applicationDirPath()/shaders — give the
# test binary the same layout the app gets.
add_custom_command(TARGET arraw_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:arraw_tests>/shaders
)

target_link_libraries(arraw_tests PRIVATE arraw_core Catch2::Catch2WithMain)

target_compile_definitions(arraw_tests PRIVATE
    ARRAW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ARRAW_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

include(Catch)
catch_discover_tests(arraw_tests)
