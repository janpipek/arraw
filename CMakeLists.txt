cmake_minimum_required(VERSION 3.21)
project(arraw VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets Concurrent ShaderTools Xml)

qt_standard_project_setup()

add_executable(arraw-ui src/app/main.cpp
        src/app/ui/MainWindow.cpp)
target_include_directories(arraw-ui PRIVATE src/app)
target_link_libraries(arraw-ui PRIVATE Qt6::Widgets)

add_executable(arraw-cli src/cli/main.cpp)
