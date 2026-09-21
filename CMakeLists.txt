cmake_minimum_required(VERSION 3.21)
project(arraw VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ARRAW_BUILD_TESTS "Build the arraw test suite" ON)

function(arraw_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

find_package(Qt6 6.10 REQUIRED COMPONENTS Gui Widgets Concurrent ShaderTools Xml)

# RAW decoding, required rather than optional: an arraw built without it would
# decode the same file differently from one built with it, which is the failure
# mode ADR 005 exists to prevent. Prefer the reentrant pkg-config module when
# LibRaw ships it; some distributions expose only the generic libraw module.
find_package(PkgConfig REQUIRED)
pkg_search_module(LibRaw REQUIRED IMPORTED_TARGET libraw_r libraw)

qt_standard_project_setup()

# The engine library. Everything photographic lives here; the executables are
# thin front ends over it. Headers under include/ are the public API and are
# propagated to consumers through target_link_libraries.
add_library(arraw STATIC
        include/ColorEncoding.h
        include/Develop.h
        include/Diagnostics.h
        include/WhiteBalance.h
        include/DevelopSettings.h
        src/core/ColorSpaces.h
        src/core/Develop.cpp
        src/core/Diagnostics.cpp
        src/core/ProcessingPlan.cpp
        src/core/ProcessingPlan.h
        src/core/WhiteBalance.cpp
        include/ImageBuffer.h
        include/ImageExport.h
        include/Photo.h
        src/core/ImageBuffer.cpp
        src/core/ImageExport.cpp
        include/ImageImport.h
        src/core/ImageImport.cpp
        src/core/QtImage.cpp
        src/core/QtImage.h
        src/core/RawImport.cpp
        src/core/RawImport.h
)
add_library(arraw::arraw ALIAS arraw)
arraw_enable_warnings(arraw)
target_include_directories(arraw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Qt GUI holds QImage and the image codecs (QImageReader/QImageWriter); it needs
# no display and no QGuiApplication, only a QCoreApplication for plugin paths.
# Private: nothing in include/ mentions Qt, and CMake still propagates the link
# requirement to consumers of this static library.
target_link_libraries(arraw PRIVATE Qt6::Gui PkgConfig::LibRaw)
# qt_standard_project_setup() turns AUTOMOC on globally; the engine has no Qt
# types, so opting out keeps that honest (and saves a moc pass).
set_target_properties(arraw PROPERTIES AUTOMOC OFF)

add_executable(arraw-ui src/app/main.cpp
        src/app/ui/MainWindow.cpp
        src/app/ui/MainWindow.h)
arraw_enable_warnings(arraw-ui)
target_include_directories(arraw-ui PRIVATE src/app)
target_link_libraries(arraw-ui PRIVATE arraw Qt6::Widgets)

# The command line's behaviour, split from its main() so the tests can drive it
# in-process and assert on what reaches each stream. See ADR 006.
add_library(arraw-cli-core STATIC
        src/cli/Cli.cpp
        src/cli/Cli.h
        src/cli/Command.cpp
        src/cli/Command.h
        src/cli/ExportCommand.cpp
        src/cli/ExportCommand.h)
arraw_enable_warnings(arraw-cli-core)
target_include_directories(arraw-cli-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)
# Qt Core only: the command line needs QCommandLineParser and, through main(),
# a QCoreApplication for codec plugin paths. Widgets must never appear here.
target_link_libraries(arraw-cli-core PUBLIC arraw PRIVATE Qt6::Core)
target_compile_definitions(arraw-cli-core PRIVATE ARRAW_VERSION="${PROJECT_VERSION}")
set_target_properties(arraw-cli-core PROPERTIES AUTOMOC OFF)

add_executable(arraw-cli src/cli/main.cpp)
arraw_enable_warnings(arraw-cli)
target_link_libraries(arraw-cli PRIVATE arraw-cli-core Qt6::Core)

if(ARRAW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
