cmake_minimum_required(VERSION 3.21)
project(arraw VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(arraw_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets Concurrent ShaderTools Xml)

qt_standard_project_setup()

add_executable(arraw-ui src/app/main.cpp
        src/app/ui/MainWindow.cpp
        include/Photo.h
        include/DevelopSettings.h
        include/ImageBuffer.h
        src/core/ImageBuffer.cpp)
arraw_enable_warnings(arraw-ui)
target_include_directories(arraw-ui PRIVATE src/app include)
target_link_libraries(arraw-ui PRIVATE Qt6::Widgets)

add_executable(arraw-cli src/cli/main.cpp
        include/Photo.h
        include/DevelopSettings.h
        include/ImageBuffer.h
        src/core/ImageBuffer.cpp)
arraw_enable_warnings(arraw-cli)
target_include_directories(arraw-cli PRIVATE include)
