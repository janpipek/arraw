cmake_minimum_required(VERSION 3.21)
project(arraw VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)
find_package(OpenGL REQUIRED)

# libraw: try pkg-config first (IMPORTED_TARGET wires up lib dirs automatically),
# fall back to manual find_library when pkg-config is unavailable.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBRAW IMPORTED_TARGET libraw)
endif()
if(NOT TARGET PkgConfig::LIBRAW)
    find_library(LIBRAW_LIB NAMES raw raw_r REQUIRED)
    find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h REQUIRED)
    add_library(PkgConfig::LIBRAW INTERFACE IMPORTED)
    set_target_properties(PkgConfig::LIBRAW PROPERTIES
        INTERFACE_LINK_LIBRARIES "${LIBRAW_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBRAW_INCLUDE_DIR}"
    )
endif()

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/ImageViewport.cpp
    src/AdjustmentPanel.cpp
    src/ToneCurveWidget.cpp
    src/ExifPanel.cpp
    src/ImageMetadata.cpp
    src/Histogram.cpp
    src/RawProcessor.cpp
    src/StandardImageLoader.cpp
    src/ImagePipeline.cpp
    src/XmpSidecar.cpp
    src/FileBrowser.cpp
    src/ThumbnailCache.cpp
    src/ExportDialog.cpp
)


add_executable(arraw ${SOURCES})

target_include_directories(arraw PRIVATE src)

target_link_libraries(arraw PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
    OpenGL::GL
    PkgConfig::LIBRAW
)

# Copy shaders next to the binary
add_custom_command(TARGET arraw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:arraw>/shaders
)
