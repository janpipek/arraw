cmake_minimum_required(VERSION 3.21)
project(arraw VERSION 0.2.4 LANGUAGES CXX)

option(ARRAW_BUILD_TESTS "Build arraw tests" ON)
if(ARRAW_BUILD_TESTS)
    enable_testing()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(arraw_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# 6.8: QRhiWidget (6.7) + LTS. ShaderTools provides qt6_add_shaders (qsb).
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Concurrent ShaderTools Xml)

# Qt6::GuiPrivate exposes the semi-public RHI headers (rhi/qrhi.h) that ADR 0006's
# QRhiWidget pipeline needs. Packagings disagree on how they expose it: official Qt
# binaries (aqtinstall, used by the Linux release CI) and vcpkg ship the target
# inside the Gui package, pulled in transitively by Widgets above; Fedora's system
# Qt splits it into a separate GuiPrivate *component* with its own config file that
# must be requested explicitly, and does NOT define the target via Gui. Requesting
# GuiPrivate-as-component unconditionally breaks on official Qt (no such config);
# relying on the transitive target breaks on Fedora. So take the target if it is
# already here, and only request the component when it is not.
if(NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6 6.8 REQUIRED COMPONENTS GuiPrivate)
endif()

# On Windows (vcpkg) the Qt DLLs are deployed next to the binary by vcpkg's
# applocal step, but the Qt plugins are not. Without the platform plugin Qt aborts
# at startup ("no Qt platform plugin could be initialized"); without the
# imageformats plugins QImage silently fails to decode JPEG/GIF/ICO and the app
# reports "Failed to load" (PNG/BMP are built into Qt Gui, so they still work).
# Copy each needed plugin into its plugin-type subdir beside the target. Offscreen
# is included so headless test runs work via QT_QPA_PLATFORM=offscreen.
if(WIN32)
    # vcpkg installed tree, used to fetch plugin codec DLLs (e.g. jpeg62.dll) that
    # applocal does not deploy. Fixed default for this machine; override if needed.
    set(ARRAW_VCPKG_INSTALLED "C:/dev/vcpkg/installed/x64-windows"
        CACHE PATH "vcpkg installed tree (Windows runtime-dependency deploy)")
endif()
function(arraw_deploy_qt_plugins target)
    if(NOT WIN32)
        return()
    endif()
    # plugin imported target -> destination subdir next to the binary
    set(plugins
        Qt6::QWindowsIntegrationPlugin    platforms
        Qt6::QOffscreenIntegrationPlugin  platforms
        Qt6::QJpegPlugin                  imageformats
        Qt6::QGifPlugin                   imageformats
        Qt6::QICOPlugin                   imageformats
    )
    while(plugins)
        list(POP_FRONT plugins plugin subdir)
        if(TARGET ${plugin})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${target}>/${subdir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${plugin}>"
                    "$<TARGET_FILE_DIR:${target}>/${subdir}/"
                VERBATIM)
        endif()
    endwhile()

    # The JPEG plugin links the external libjpeg-turbo codec (jpeg62.dll), which
    # applocal does not deploy (the exe never imports it directly, only the plugin
    # does). Windows resolves a plugin's imports from the EXE's directory, so place
    # it there, taking the debug build's codec from the debug bin.
    if(TARGET Qt6::QJpegPlugin)
        set(jpeg_dll "$<IF:$<CONFIG:Debug>,${ARRAW_VCPKG_INSTALLED}/debug/bin/jpeg62.dll,${ARRAW_VCPKG_INSTALLED}/bin/jpeg62.dll>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${jpeg_dll}"
                "$<TARGET_FILE_DIR:${target}>/"
            VERBATIM)
    endif()
endfunction()

# libraw: try pkg-config first (IMPORTED_TARGET wires up lib dirs automatically),
# fall back to manual find_library when pkg-config is unavailable.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBRAW IMPORTED_TARGET libraw)
endif()
if(NOT TARGET PkgConfig::LIBRAW)
    # On vcpkg the debug import library is named differently (rawd) and points at
    # rawd.dll in debug/bin. Pick per-config so debug builds deploy the right DLL.
    find_library(LIBRAW_LIB_RELEASE NAMES raw raw_r REQUIRED)
    find_library(LIBRAW_LIB_DEBUG NAMES rawd raw_rd)
    find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h REQUIRED)
    add_library(PkgConfig::LIBRAW INTERFACE IMPORTED)
    if(LIBRAW_LIB_DEBUG)
        set(LIBRAW_LINK "$<IF:$<CONFIG:Debug>,${LIBRAW_LIB_DEBUG},${LIBRAW_LIB_RELEASE}>")
    else()
        set(LIBRAW_LINK "${LIBRAW_LIB_RELEASE}")
    endif()
    set_target_properties(PkgConfig::LIBRAW PROPERTIES
        INTERFACE_LINK_LIBRARIES "${LIBRAW_LINK}"
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

# lensfun: optional lens-correction source (docs/adr/0032). ARRAW_WITH_LENSFUN is a
# tri-state that decides whether it is compiled in:
#   AUTO (default) — build it when lensfun is found, silently degrade when it is not
#   ON             — require lensfun; hard-error if it is missing (use for release
#                    builds so a distributed binary never silently ships without it)
#   OFF            — compile lens correction out entirely
# When enabled, ARRAW_HAS_LENSFUN is defined and the lensfun source links the library;
# otherwise lens correction degrades to "no profile available". Detection is
# pkg-config-first, same strategy as libraw/lcms2.
set(ARRAW_WITH_LENSFUN "AUTO" CACHE STRING
    "Lens-correction lensfun support: AUTO (use if found), ON (require), OFF (disable)")
set_property(CACHE ARRAW_WITH_LENSFUN PROPERTY STRINGS AUTO ON OFF)

if(NOT ARRAW_WITH_LENSFUN STREQUAL "OFF")
    if(PkgConfig_FOUND)
        pkg_check_modules(LENSFUN IMPORTED_TARGET lensfun)
    endif()
    if(NOT TARGET PkgConfig::LENSFUN)
        find_library(LENSFUN_LIB NAMES lensfun)
        find_path(LENSFUN_INCLUDE_DIR lensfun.h PATH_SUFFIXES lensfun)
        if(LENSFUN_LIB AND LENSFUN_INCLUDE_DIR)
            find_library(GLIB2_LIB NAMES glib-2.0)
            find_library(GOBJECT2_LIB NAMES gobject-2.0)
            find_library(INTL_LIB NAMES intl)
            find_library(ICONV_LIB NAMES iconv)
            add_library(PkgConfig::LENSFUN INTERFACE IMPORTED)
            set(LENSFUN_LINK_LIBS "${LENSFUN_LIB}")
            foreach(dep GLIB2_LIB GOBJECT2_LIB INTL_LIB ICONV_LIB)
                if(${dep})
                    list(APPEND LENSFUN_LINK_LIBS "${${dep}}")
                endif()
            endforeach()
            set_target_properties(PkgConfig::LENSFUN PROPERTIES
                INTERFACE_LINK_LIBRARIES "${LENSFUN_LINK_LIBS}"
                INTERFACE_INCLUDE_DIRECTORIES "${LENSFUN_INCLUDE_DIR}"
            )
        endif()
    endif()
endif()
if(TARGET PkgConfig::LENSFUN)
    set(ARRAW_HAVE_LENSFUN ON)
    message(STATUS "lensfun found — building lens-correction lensfun source")
elseif(ARRAW_WITH_LENSFUN STREQUAL "ON")
    message(FATAL_ERROR "ARRAW_WITH_LENSFUN=ON but lensfun was not found "
        "(install lensfun's development package, e.g. lensfun-devel / liblensfun-dev)")
else()
    set(ARRAW_HAVE_LENSFUN OFF)
    message(STATUS "lensfun NOT found — lens correction will report no profile")
endif()

# exiv2: optional exported-metadata writer (docs/adr/0043). Same tri-state shape as
# lensfun: AUTO builds metadata embedding when available, ON requires it for
# release/package builds, OFF compiles the best-effort no-backend path.
set(ARRAW_WITH_EXIV2 "AUTO" CACHE STRING
    "Exported-metadata exiv2 support: AUTO (use if found), ON (require), OFF (disable)")
set_property(CACHE ARRAW_WITH_EXIV2 PROPERTY STRINGS AUTO ON OFF)

if(NOT ARRAW_WITH_EXIV2 STREQUAL "OFF")
    if(PkgConfig_FOUND)
        pkg_check_modules(EXIV2 IMPORTED_TARGET exiv2)
    endif()
    if(NOT TARGET PkgConfig::EXIV2)
        find_library(EXIV2_LIB NAMES exiv2)
        find_path(EXIV2_INCLUDE_DIR exiv2/exiv2.hpp)
        if(EXIV2_LIB AND EXIV2_INCLUDE_DIR)
            add_library(PkgConfig::EXIV2 INTERFACE IMPORTED)
            set_target_properties(PkgConfig::EXIV2 PROPERTIES
                INTERFACE_LINK_LIBRARIES "${EXIV2_LIB}"
                INTERFACE_INCLUDE_DIRECTORIES "${EXIV2_INCLUDE_DIR}"
            )
        endif()
    endif()
endif()
if(TARGET PkgConfig::EXIV2)
    set(ARRAW_HAVE_EXIV2 ON)
    message(STATUS "exiv2 found — building exported-metadata writer")
elseif(ARRAW_WITH_EXIV2 STREQUAL "ON")
    message(FATAL_ERROR "ARRAW_WITH_EXIV2=ON but exiv2 was not found "
        "(install exiv2's development package, e.g. exiv2-devel / libexiv2-dev)")
else()
    set(ARRAW_HAVE_EXIV2 OFF)
    message(STATUS "exiv2 NOT found — exports will omit embedded metadata")
endif()

# Everything except main.cpp lives in a static library so test executables
# can link the real code (see DESIGN.md "Testing Strategy").
set(CORE_SOURCES
    src/develop/BasicTone.cpp
    src/develop/WhiteBalance.cpp
    src/pipeline/OkLab.cpp
    src/ui/Theme.cpp
    src/MainWindow.cpp
    src/ui/ImageViewport.cpp
    src/render/ViewportGeometry.cpp
    src/render/RendererCore.cpp
    src/ui/AdjustmentPanel.cpp
    src/ui/AdjustmentTabTool.cpp
    src/ui/LocalAdjustmentPanel.cpp
    src/ui/AdjustmentSpinBox.cpp
    src/ui/CollapsiblePane.cpp
    src/ui/ToneCurveWidget.cpp
    src/ui/InfoPanel.cpp
    src/ExportWorkflow.cpp
    src/ExportMetadata.cpp
    src/core/ImageMetadata.cpp
    src/ui/FilmStripTooltip.cpp
    src/ui/Histogram.cpp
    src/pipeline/RawProcessor.cpp
    src/pipeline/StandardImageLoader.cpp
    src/pipeline/ImagePipeline.cpp
    src/ImageLoadWorkflow.cpp
    src/develop/LocalAdjustment.cpp
    src/develop/FieldSpec.cpp
    src/pipeline/ColorManagement.cpp
    src/ui/ProofingPanel.cpp
    src/io/XmpSidecar.cpp
    src/ui/FilmStrip.cpp
    src/ui/FilmStripModel.cpp
    src/ui/FilmStripFilterModel.cpp
    src/ui/ImageGrouping.cpp
    src/ui/FilmStripLayout.cpp
    src/ThumbnailCache.cpp
    src/DecodeCache.cpp
    src/DevelopSession.cpp
    src/ui/ExportDialog.cpp
    src/ui/AboutDialog.cpp
    src/core/CropGeometry.cpp
    src/core/Orientation.cpp
    src/develop/DevelopGroup.cpp
    src/develop/DevelopParameter.cpp
    src/develop/DemosaicAlgorithm.cpp
    src/develop/DevelopPreset.cpp
    src/ui/GroupChecklistDialog.cpp
    src/io/PresetStore.cpp
    src/develop/Spot.cpp
    src/develop/BrushMask.cpp
    src/ui/SpotRemovalPanel.cpp
    src/ui/HistoryPanel.cpp
    src/core/NoiseReduction.cpp
    src/pipeline/LensCorrection.cpp
    src/pipeline/LensfunSource.cpp
    src/BatchPaste.cpp
    src/ui/BatchProgressDialog.cpp
    src/MainWindowStatus.cpp
    src/ChromeHider.cpp
)

add_library(arraw_core STATIC ${CORE_SOURCES})
arraw_enable_warnings(arraw_core)
if(ARRAW_BUILD_TESTS)
    target_compile_definitions(arraw_core PUBLIC ARRAW_TESTING)
endif()
if(ARRAW_HAVE_LENSFUN)
    target_compile_definitions(arraw_core PUBLIC ARRAW_HAS_LENSFUN)
    target_link_libraries(arraw_core PUBLIC PkgConfig::LENSFUN)
endif()
if(ARRAW_HAVE_EXIV2)
    target_compile_definitions(arraw_core PUBLIC ARRAW_HAS_EXIV2)
    target_link_libraries(arraw_core PUBLIC PkgConfig::EXIV2)
endif()

target_include_directories(arraw_core PUBLIC src)

target_link_libraries(arraw_core PUBLIC
    Qt6::Widgets
    Qt6::GuiPrivate
    Qt6::Concurrent
    Qt6::Xml
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
          shaders/nr.vert shaders/nr_extract.frag
          shaders/nr_blur_h.frag shaders/nr_blur_v.frag shaders/nr_recombine.frag
          shaders/lum_bilateral_h.frag shaders/lum_bilateral_v.frag
          shaders/spatial_extract.frag
)

# resources/icon.qrc bakes the window-icon PNGs (generated from resources/icon.svg
# by tools/render_icons.py — see ADR 0013) in via AUTORCC. On Windows, arraw.rc
# additionally embeds arraw.ico as the executable's Win32 icon (Explorer/taskbar).
set(arraw_platform_sources "")
if(WIN32)
    enable_language(RC)
    set(arraw_platform_sources resources/arraw.rc)
endif()
# WIN32: build as a GUI-subsystem app so double-clicking does not pop a console
# window. Qt6::EntryPoint (pulled in via Qt6::Core) supplies WinMain -> main. The
# keyword is ignored on non-Windows platforms. With no console attached, redirect
# ARRAW_TRACE output to a file when profiling: arraw.exe 2> trace.txt
add_executable(arraw WIN32 src/main.cpp resources/icon.qrc ${arraw_platform_sources})
arraw_enable_warnings(arraw)

target_link_libraries(arraw PRIVATE arraw_core)
arraw_deploy_qt_plugins(arraw)

# Single-source the version from project(VERSION) into the binary (ADR 0014):
# setApplicationVersion + the --version flag both read this. A CI guard fails the
# release if the pushed vX.Y.Z tag disagrees with it.
target_compile_definitions(arraw PRIVATE ARRAW_VERSION="${PROJECT_VERSION}")

# Linux desktop integration + AppDir staging. `DESTDIR=AppDir cmake --install`
# lays out a standard AppDir (usr/bin, usr/share/...) that linuxdeploy turns into
# the bundled AppImage in CI. The reverse-DNS id io.github.janpipek.arraw names the
# .desktop, the metainfo, and the installed icons. Desktop-only, so guarded off
# macOS (which gets a .app bundle later).
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS arraw RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/io.github.janpipek.arraw.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/linux/io.github.janpipek.arraw.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    foreach(icon_size 16 24 32 48 64 128 256)
        install(FILES resources/icons/arraw-${icon_size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
            RENAME io.github.janpipek.arraw.png)
    endforeach()
endif()

if(ARRAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
