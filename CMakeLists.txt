cmake_minimum_required(VERSION 3.21)
project(arraw VERSION 0.1.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 6.8: QRhiWidget (6.7) + LTS. ShaderTools provides qt6_add_shaders (qsb);
# GuiPrivate provides the semi-public RHI headers (rhi/qrhi.h).
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Concurrent ShaderTools GuiPrivate)

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

# lcms2: same pkg-config-first strategy as libraw.
if(PkgConfig_FOUND)
    pkg_check_modules(LCMS2 IMPORTED_TARGET lcms2)
endif()
if(NOT TARGET PkgConfig::LCMS2)
    find_library(LCMS2_LIB NAMES lcms2 REQUIRED)
    find_path(LCMS2_INCLUDE_DIR lcms2.h REQUIRED)
    add_library(PkgConfig::LCMS2 INTERFACE IMPORTED)
    set_target_properties(PkgConfig::LCMS2 PROPERTIES
        INTERFACE_LINK_LIBRARIES "${LCMS2_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LCMS2_INCLUDE_DIR}"
    )
endif()

# Everything except main.cpp lives in a static library so test executables
# can link the real code (see DESIGN.md "Testing Strategy").
set(CORE_SOURCES
    src/MainWindow.cpp
    src/ImageViewport.cpp
    src/RendererCore.cpp
    src/AdjustmentPanel.cpp
    src/ToneCurveWidget.cpp
    src/ExifPanel.cpp
    src/ImageMetadata.cpp
    src/Histogram.cpp
    src/RawProcessor.cpp
    src/StandardImageLoader.cpp
    src/ImagePipeline.cpp
    src/ColorManagement.cpp
    src/ProofingPanel.cpp
    src/XmpSidecar.cpp
    src/FilmStrip.cpp
    src/FilmStripModel.cpp
    src/FilmStripLayout.cpp
    src/ThumbnailCache.cpp
    src/ExportDialog.cpp
)

add_library(arraw_core STATIC ${CORE_SOURCES})

target_include_directories(arraw_core PUBLIC src)

target_link_libraries(arraw_core PUBLIC
    Qt6::Widgets
    Qt6::GuiPrivate
    Qt6::Concurrent
    PkgConfig::LIBRAW
    PkgConfig::LCMS2
)

# Shaders are compiled by qsb at build time (Vulkan GLSL → SPIR-V →
# MSL/HLSL/GLSL) and baked into resources as :/shaders/*.qsb (ADR 0006).
# GLSL floor is 330/300es: the fragment shader needs textureSize + sampler3D.
qt6_add_shaders(arraw_core "arraw_shaders"
    PREFIX "/shaders"
    BASE shaders
    GLSL "300es,330"
    HLSL 50
    MSL 12
    FILES shaders/image.vert shaders/image.frag
)

add_executable(arraw src/main.cpp)

target_link_libraries(arraw PRIVATE arraw_core)

add_subdirectory(tests)
