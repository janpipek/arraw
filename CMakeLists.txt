cmake_minimum_required(VERSION 3.21)
project(arraw VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)
find_package(OpenGL REQUIRED)

# libraw: try pkg-config first, fall back to manual find
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBRAW libraw)
endif()
if(NOT LIBRAW_FOUND)
    find_library(LIBRAW_LIBRARIES NAMES raw raw_r REQUIRED)
    find_path(LIBRAW_INCLUDE_DIRS libraw/libraw.h REQUIRED)
endif()

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/ImageViewport.cpp
    src/AdjustmentPanel.cpp
    src/ExifPanel.cpp
    src/ImageMetadata.cpp
    src/Histogram.cpp
    src/RawProcessor.cpp
    src/ImagePipeline.cpp
    src/XmpSidecar.cpp
    src/FileBrowser.cpp
    src/ThumbnailCache.cpp
    src/ExportDialog.cpp
)


add_executable(arraw ${SOURCES})

target_include_directories(arraw PRIVATE
    src
    ${LIBRAW_INCLUDE_DIRS}
)

target_link_libraries(arraw PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
    OpenGL::GL
    ${LIBRAW_LIBRARIES}
)

# Copy shaders next to the binary
add_custom_command(TARGET arraw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:arraw>/shaders
)
