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

qt_standard_project_setup()

# The engine library. Everything photographic lives here; the executables are
# thin front ends over it. Headers under include/ are the public API and are
# propagated to consumers through target_link_libraries.
add_library(arraw STATIC
        include/DevelopSettings.h
        include/ImageBuffer.h
        include/ImageExport.h
        include/Photo.h
        src/core/ImageBuffer.cpp
        src/core/ImageExport.cpp
        include/ImageImport.h
        src/core/ImageImport.cpp
)
add_library(arraw::arraw ALIAS arraw)
arraw_enable_warnings(arraw)
target_include_directories(arraw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Qt GUI holds QImage and the image codecs (QImageReader/QImageWriter); it needs
# no display and no QGuiApplication, only a QCoreApplication for plugin paths.
# Private: nothing in include/ mentions Qt, and CMake still propagates the link
# requirement to consumers of this static library.
target_link_libraries(arraw PRIVATE Qt6::Gui)
# qt_standard_project_setup() turns AUTOMOC on globally; the engine has no Qt
# types, so opting out keeps that honest (and saves a moc pass).
set_target_properties(arraw PROPERTIES AUTOMOC OFF)

add_executable(arraw-ui src/app/main.cpp
        src/app/ui/MainWindow.cpp
        src/app/ui/MainWindow.h)
arraw_enable_warnings(arraw-ui)
target_include_directories(arraw-ui PRIVATE src/app)
target_link_libraries(arraw-ui PRIVATE arraw Qt6::Widgets)

add_executable(arraw-cli src/cli/main.cpp)
arraw_enable_warnings(arraw-cli)
target_link_libraries(arraw-cli PRIVATE arraw)

if(ARRAW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
