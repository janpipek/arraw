cmake_minimum_required(VERSION 3.21)
project(arraw VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ARRAW_BUILD_TESTS "Build the arraw test suite" ON)
option(ARRAW_BUILD_APPS "Build the GUI, the command line and the GPU module" ON)
option(ARRAW_BUILD_PYTHON "Build the Python extension module" OFF)

if(ARRAW_BUILD_TESTS AND NOT ARRAW_BUILD_APPS)
    message(FATAL_ERROR "ARRAW_BUILD_TESTS needs the applications (arraw-gpu, arraw-cli-core, "
            "arraw-cli, arraw-headless-platform): set ARRAW_BUILD_APPS=ON or ARRAW_BUILD_TESTS=OFF.")
endif()

function(arraw_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# GuiPrivate carries QRhi and the platform plugin API (QPA); CorePrivate the
# headers those include in turn. Since Qt 6.10 a private module is only defined
# when it is named here; linking Qt6::GuiPrivate after finding Gui alone no
# longer works.
find_package(Qt6 6.10 REQUIRED COMPONENTS Core CorePrivate Gui GuiPrivate Widgets
        ShaderTools Xml)

# RAW decoding, required rather than optional: an arraw built without it would
# decode the same file differently from one built with it, which is the failure
# mode ADR 005 exists to prevent. Prefer the reentrant pkg-config module when
# LibRaw ships it; some distributions expose only the generic libraw module.
# Windows has no pkg-config, so find vcpkg's reentrant build directly (its
# CMake package would drag in an OpenMP lookup that clang-cl-style setups fail).
if(WIN32)
    find_library(LIBRAW_LIB_RELEASE NAMES raw_r REQUIRED)
    find_library(LIBRAW_LIB_DEBUG NAMES raw_rd REQUIRED)
    find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h REQUIRED)
    add_library(arraw-libraw INTERFACE)
    target_link_libraries(arraw-libraw INTERFACE
            "$<IF:$<CONFIG:Debug>,${LIBRAW_LIB_DEBUG},${LIBRAW_LIB_RELEASE}>")
    target_include_directories(arraw-libraw INTERFACE ${LIBRAW_INCLUDE_DIR})
    set(ARRAW_LIBRAW arraw-libraw)
else()
    find_package(PkgConfig REQUIRED)
    pkg_search_module(LibRaw REQUIRED IMPORTED_TARGET libraw_r libraw)
    set(ARRAW_LIBRAW PkgConfig::LibRaw)
endif()

# EXIF reading, required for the same reason as LibRaw: capture information that
# is there on one build and missing on another would be a bug nobody could
# reproduce (ADR 028). exiv2 0.28 ships both a pkg-config file and a CMake
# package; pkg-config is what Linux and macOS (Homebrew) package managers
# provide, vcpkg provides the CMake package (exiv2::exiv2lib, with
# exiv2-xmp as its own target), so each platform uses what it has. Linked
# PRIVATE: no exiv2 type appears in include/. BMFF (CR3, HEIF) is on by
# default in 0.28.
if(WIN32)
    # exiv2's version file is ExactVersion, so `find_package(exiv2 0.28)`
    # would reject 0.28.8; ask for any version and check the floor here.
    find_package(exiv2 CONFIG REQUIRED)
    if(exiv2_VERSION VERSION_LESS 0.28)
        message(FATAL_ERROR "exiv2 >= 0.28 required, found ${exiv2_VERSION}")
    endif()
    set(ARRAW_EXIV2 Exiv2::exiv2lib)
else()
    pkg_check_modules(EXIV2 REQUIRED IMPORTED_TARGET exiv2>=0.28)
    set(ARRAW_EXIV2 PkgConfig::EXIV2)
endif()

# On Windows vcpkg deploys the Qt DLLs next to each executable, but not the
# plugins. Without a platform plugin a Q(Gui)Application aborts at startup;
# without the JPEG or TIFF plugin QImage silently cannot read or write that
# format. An image plugin's codec DLLs go beside the executable, where Windows
# looks for a plugin's imports. The TIFF plugin comes from vcpkg's
# qtimageformats port ("qtimageformats[core,tiff]"); without it TIFF stays
# unsupported rather than failing the build.
function(arraw_deploy_qt_plugins target)
    if(NOT WIN32)
        return()
    endif()
    set(plugins
            Qt6::QWindowsIntegrationPlugin platforms
            Qt6::QOffscreenIntegrationPlugin platforms
            Qt6::QJpegPlugin imageformats
            Qt6::QTiffPlugin imageformats)
    while(plugins)
        list(POP_FRONT plugins plugin subdir)
        if(TARGET ${plugin})
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/${subdir}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "$<TARGET_FILE:${plugin}>" "$<TARGET_FILE_DIR:${target}>/${subdir}/"
                    VERBATIM)
        endif()
    endwhile()
    set(codecs)
    if(TARGET Qt6::QJpegPlugin)
        list(APPEND codecs jpeg62.dll)
    endif()
    if(TARGET Qt6::QTiffPlugin)
        # libtiff links zlib too, which Qt Gui needs and vcpkg deploys anyway.
        list(APPEND codecs "tiff$<$<CONFIG:Debug>:d>.dll" liblzma.dll jpeg62.dll)
    endif()
    if(codecs)
        list(REMOVE_DUPLICATES codecs)
        set(vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/bin")
        list(TRANSFORM codecs PREPEND "${vcpkg_bin}/")
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${codecs} "$<TARGET_FILE_DIR:${target}>/"
                VERBATIM)
    endif()
endfunction()

qt_standard_project_setup()

# The opt-in timing trace (QT_LOGGING_RULES="arraw.timing.debug=true"), on its
# own so that the engine, the GPU module and the front ends can each record
# spans without reaching into src/core. Not public API: its header names a Qt
# logging category, which include/ must not, and Python has no use for it.
add_library(arraw-trace STATIC
        src/trace/TimingTrace.cpp
        src/trace/TimingTrace.h)
arraw_enable_warnings(arraw-trace)
target_include_directories(arraw-trace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/trace)
target_link_libraries(arraw-trace PUBLIC Qt6::Core)
set_target_properties(arraw-trace PROPERTIES AUTOMOC OFF)
if(ARRAW_BUILD_PYTHON)
    set_target_properties(arraw-trace PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# The engine library. Everything photographic lives here; the executables are
# thin front ends over it. Headers under include/ are the public API and are
# propagated to consumers through target_link_libraries.
add_library(arraw STATIC
        include/ColorEncoding.h
        include/BlackAndWhiteSettings.h
        include/ColorGradingSettings.h
        include/ColorSettings.h
        include/EffectsSettings.h
        include/HslSettings.h
        include/NoiseReductionSettings.h
        include/PresenceSettings.h
        src/core/ColorAdjustments.h
        src/core/ColorAdjustments.cpp
        src/core/ColorGrading.h
        src/core/ColorGrading.cpp
        src/core/ColorSettings.cpp
        src/core/Denoise.h
        src/core/Denoise.cpp
        src/core/Presence.h
        src/core/Presence.cpp
        src/core/ReducedGrid.h
        src/core/Effects.h
        src/core/Effects.cpp
        src/core/GrainModels.h
        src/core/GrainModels.cpp
        src/core/GrainSeed.cpp
        include/Develop.h
        include/Diagnostics.h
        include/WhiteBalance.h
        include/DevelopSettings.h
        include/DevelopState.h
        src/core/DevelopState.cpp
        include/GeometrySettings.h
        include/ImageOrientation.h
        include/ImagePyramid.h
        src/core/ImagePyramid.cpp
        src/core/GeometryPlan.h
        src/core/GeometryPlan.cpp
        src/core/Resample.h
        src/core/RowBands.cpp
        src/core/RowBands.h
        src/core/Resample.cpp
        src/core/ResampleWeights.h
        src/core/ResampleWeights.cpp
        include/ToneCurveSettings.h
        src/core/ToneCurveSettings.cpp
        src/core/ToneCurve.h
        src/core/ToneCurve.cpp
        include/ToneSettings.h
        src/core/ToneSettings.cpp
        src/core/ColorSpaces.h
        src/core/Develop.cpp
        include/CurveHistogram.h
        src/core/CurveHistogram.cpp
        src/core/Taps.h
        src/core/Taps.cpp
        src/core/Diagnostics.cpp
        include/Progress.h
        src/core/ProgressScope.h
        src/core/ProgressScope.cpp
        src/core/RenderProgress.h
        src/core/RenderProgress.cpp
        src/core/ProcessingPlan.cpp
        src/core/ProcessingPlan.h
        include/RenderCheckpoint.h
        src/core/CheckpointState.h
        src/core/RenderCheckpoint.cpp
        src/core/device/DeviceImage.h
        src/core/device/DeviceImageState.h
        src/core/device/DeviceImage.cpp
        src/core/WhiteBalance.cpp
        include/ImageBuffer.h
        include/ImageExport.h
        include/EditSession.h
        include/Photo.h
        src/core/EditSession.cpp
        src/core/Photo.cpp
        include/SettingDescriptors.h
        src/core/SettingDescriptors.cpp
        include/Edits.h
        src/core/Edits.cpp
        src/core/SettingCodec.h
        include/ShortestDecimal.h
        src/core/SettingCodec.cpp
        include/SettingsJson.h
        src/core/SettingsJson.cpp
        include/PhotoMarks.h
        include/Sidecar.h
        src/core/Sidecar.cpp
        src/core/ImageBuffer.cpp
        src/core/ImageExport.cpp
        include/ExifInfo.h
        src/core/ExifInfo.cpp
        src/core/Exiv2Support.h
        src/core/Exiv2Support.cpp
        src/core/MetadataEmbedding.h
        src/core/MetadataEmbedding.cpp
        src/core/EmbeddedPreview.cpp
        include/ImageImport.h
        src/core/ImageImport.cpp
        include/MarksFilter.h
        src/core/MarksFilter.cpp
        include/Shot.h
        src/core/Shot.cpp
        src/core/QtImage.cpp
        src/core/QtImage.h
        src/core/RawImport.cpp
        src/core/RawImport.h
        src/core/SampleConversion.h
        src/core/SampleConversion.cpp
)
add_library(arraw::arraw ALIAS arraw)
arraw_enable_warnings(arraw)
# The grain's lattice positions must round as the shader's do, one operation at
# a time (ADR 038); a fused multiply-add would find other cells near their edges.
# MSVC does not contract under its default /fp:precise.
set_source_files_properties(src/core/GrainModels.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>")
target_include_directories(arraw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# A checkpoint may hold its pixels on a device (ADR 015), so the device image
# handle is the engine's: src/core/device holds it and the Qt-free state behind
# it, whose device-side subclass is arraw-gpu's. Nothing there names a graphics
# type, so core holds a device image without Qt reaching either the engine's
# headers or the public API, and without seeing the rest of the GPU module.
# Only arraw-gpu links Qt6::GuiPrivate, and the tests that drive its QRhi
# device directly (tests/test_GpuDevice.cpp).
target_include_directories(arraw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core/device)
# Qt GUI holds QImage and the image codecs (QImageReader/QImageWriter); it needs
# no display and no QGuiApplication, only a QCoreApplication for plugin paths.
# Private: nothing in include/ mentions Qt, and CMake still propagates the link
# requirement to consumers of this static library.
target_link_libraries(arraw PRIVATE arraw-trace Qt6::Core Qt6::Gui Qt6::Xml ${ARRAW_LIBRAW} ${ARRAW_EXIV2})
# Stamped into exported files as their Software tag (ADR 032).
target_compile_definitions(arraw PRIVATE ARRAW_VERSION="${PROJECT_VERSION}")
# qt_standard_project_setup() turns AUTOMOC on globally; the engine has no Qt
# types, so opting out keeps that honest (and saves a moc pass).
set_target_properties(arraw PROPERTIES AUTOMOC OFF)
# A Python extension module is a shared object, and the static library goes into it.
if(ARRAW_BUILD_PYTHON)
    set_target_properties(arraw PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Everything from here to the tests is an application or belongs to one; the
# Python module needs only the engine above. (Not indented, to keep the diff
# and the blame of these blocks readable.)
if(ARRAW_BUILD_APPS)

# The one graphics device the library owns: an offscreen QRhi, and the textures
# it mints as DeviceImages (ADR 015). Separate from the engine so that the only
# translation units exposed to Qt6::GuiPrivate, whose compatibility guarantees
# are weaker than public Qt's, are the ones in this target. Its headers name no
# Qt type, so the dependency stays private.
add_library(arraw-gpu STATIC
        src/gpu/GpuContext.h
        src/gpu/GpuContext.cpp
        src/gpu/GpuDevice.h
        src/gpu/GpuDevice.cpp
        src/gpu/GpuDevelop.h
        src/gpu/GpuDevelop.cpp
        src/gpu/GpuPlan.h
        src/gpu/GpuPlan.cpp)
arraw_enable_warnings(arraw-gpu)
# src/core/device too, because GpuContext.h hands out DeviceImages: whoever can
# use the device can name what it makes.
target_include_directories(arraw-gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/device)
# The processing plan and checkpoint state are engine-private; the GPU packs its
# uniforms from the one and builds the other.
target_include_directories(arraw-gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
# The passes' shaders, compiled at build time into .qsb resources, so a shader
# that does not compile fails the build rather than the first render. GLSL
# 440 sources; qsb adds SPIR-V for Vulkan and translates for the rest. The GLSL
# targets start at ES 3.0 and 3.30, the first with texelFetch and uint. What
# several passes share is in src/gpu/shaders/common/*.glsl, pulled in with
# #include (GL_GOOGLE_include_directive) and not listed here: those files are
# never compiled on their own. qsb writes a depfile, so editing one rebuilds the
# shaders that include it.
qt_add_shaders(arraw-gpu "arraw-shaders"
        PREFIX "/arraw/shaders"
        BASE src/gpu/shaders
        GLSL "300es,330"
        HLSL 50
        MSL 12
        FILES
        src/gpu/shaders/fullscreen.vert
        src/gpu/shaders/copy.frag
        src/gpu/shaders/develop.frag
        src/gpu/shaders/geometry.frag
        src/gpu/shaders/resize_across.frag
        src/gpu/shaders/resize_across_opaque.frag
        src/gpu/shaders/resize_down.frag
        src/gpu/shaders/resize_down_opaque.frag
        src/gpu/shaders/effects.frag
        src/gpu/shaders/denoise_filter.frag
        src/gpu/shaders/denoise_combine.frag
        src/gpu/shaders/presence_filter.frag)
target_link_libraries(arraw-gpu PUBLIC arraw PRIVATE arraw-trace Qt6::Gui Qt6::GuiPrivate)
set_target_properties(arraw-gpu PROPERTIES AUTOMOC OFF)
# QRhi declares its Vulkan backend only where vulkan/vulkan.h is visible, so a
# machine without the headers builds arraw-gpu without Vulkan and nothing fails
# until `arraw-cli gpu-test` runs. On Linux that is the default backend, so the
# build says so. The headers are linked where found, for installs outside the
# compiler's default search path.
find_package(Vulkan QUIET)
if(TARGET Vulkan::Headers)
    target_link_libraries(arraw-gpu PRIVATE Vulkan::Headers)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT Vulkan_INCLUDE_DIR)
    message(WARNING "Vulkan headers (vulkan/vulkan.h) not found: arraw-gpu will be built "
            "without Vulkan, the default GPU backend on Linux. Install them (for example "
            "vulkan-headers or libvulkan-dev) and reconfigure.")
endif()

# arraw-cli's own Qt platform plugin, Linux only: Qt's offscreen platform cannot
# create a QVulkanInstance, and its xcb and wayland platforms need a display
# server, although Vulkan itself needs neither. This one exists to create the
# Vulkan instance with no display; it has no OpenGL. It is neither engine nor
# GPU code, so it lives apart from both, in src/platform.
#
# A plain static library rather than qt_add_plugin(): with a shared Qt,
# qt_add_plugin(STATIC) would also generate an initializer that imports the
# plugin into everything linking it, and deferred finalization with it. Here
# QT_STATICPLUGIN makes moc emit the static entry point instead of a shared
# plugin's, and each program that wants the platform says Q_IMPORT_PLUGIN
# itself, so the import is visible where it happens. Registered that way, Qt's
# plugin loader offers it by its key like any platform found on disk, and
# nothing is installed next to Qt's own plugins.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(arraw-headless-platform STATIC
            src/platform/headless/HeadlessPlatform.h
            src/platform/headless/HeadlessIntegration.h
            src/platform/headless/HeadlessIntegration.cpp
            src/platform/headless/HeadlessPlugin.cpp
            src/platform/headless/arraw-headless.json)
    arraw_enable_warnings(arraw-headless-platform)
    # For HeadlessPlatform.h, which names the key; it is Qt-free.
    target_include_directories(arraw-headless-platform
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/headless)
    target_compile_definitions(arraw-headless-platform PRIVATE QT_STATICPLUGIN)
    # Tells its consumers the plugin is there, so that the C++ guards follow
    # this CMake condition rather than Q_OS_LINUX, which Android defines too.
    target_compile_definitions(arraw-headless-platform INTERFACE ARRAW_HEADLESS_PLATFORM=1)
    # The QPA API, the generic Unix event dispatcher, the fontconfig database and
    # QBasicPlatformVulkanInstance are all private Qt GUI; the headers they
    # include are private Qt Core.
    target_link_libraries(arraw-headless-platform
            PRIVATE Qt6::Core Qt6::CorePrivate Qt6::Gui Qt6::GuiPrivate)
    if(TARGET Vulkan::Headers)
        target_link_libraries(arraw-headless-platform PRIVATE Vulkan::Headers)
    endif()
    # The one target that needs moc: the plugin class carries Q_OBJECT and
    # Q_PLUGIN_METADATA.
    set_target_properties(arraw-headless-platform PROPERTIES AUTOMOC ON)
endif()

# What the application does that needs no widgets, split from its main() so the
# tests can drive it, as arraw-cli-core does for the command line.
add_library(arraw-app-core STATIC
        src/app/AppSettings.cpp
        src/app/AppSettings.h
        src/app/CopySections.cpp
        src/app/CopySections.h
        src/app/CropEditing.cpp
        src/app/CropEditing.h
        src/app/CurveEditing.cpp
        src/app/CurveEditing.h
        src/app/CurveHistogramRefresh.cpp
        src/app/CurveHistogramRefresh.h
        src/app/DebugDiagnostics.cpp
        src/app/DebugDiagnostics.h
        src/app/DebugLog.cpp
        src/app/DebugLog.h
        src/app/DisplayImage.cpp
        src/app/DisplayImage.h
        src/app/ExportQueue.cpp
        src/app/ExportQueue.h
        src/app/ExportSettings.cpp
        src/app/ExportSettings.h
        src/app/PhotoLoader.cpp
        src/app/PhotoLoader.h
        src/app/PreviewRenderer.cpp
        src/app/PreviewRenderer.h
        src/app/RenderActivity.cpp
        src/app/RenderActivity.h
        src/app/RenderDelay.cpp
        src/app/RenderDelay.h
        src/app/RenderIndicator.cpp
        src/app/RenderIndicator.h
        src/app/QtMessageCapture.cpp
        src/app/QtMessageCapture.h
        src/app/FilmStripRules.cpp
        src/app/FilmStripRules.h
        src/app/FolderWatcher.cpp
        src/app/FolderWatcher.h
        src/app/OklabHue.cpp
        src/app/OklabHue.h
        src/app/ShotFilterModel.cpp
        src/app/ShotFilterModel.h
        src/app/ShotModel.cpp
        src/app/ShotModel.h
        src/app/SettingPresentation.cpp
        src/app/SettingPresentation.h
        src/app/ThumbnailCache.cpp
        src/app/ThumbnailCache.h
        src/app/ThumbnailWorker.cpp
        src/app/ThumbnailWorker.h
        src/app/ViewTransform.cpp
        src/app/ViewTransform.h
        src/app/WhiteBalanceChoice.cpp
        src/app/WhiteBalanceChoice.h)
arraw_enable_warnings(arraw-app-core)
target_include_directories(arraw-app-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(arraw-app-core PUBLIC arraw arraw-gpu Qt6::Core Qt6::Gui PRIVATE arraw-trace)
# The shot model, its filter and the folder watcher are QObjects with signals.
set_target_properties(arraw-app-core PROPERTIES AUTOMOC ON)

# The widgets, apart from main(), so the widget tests can drive the whole window.
add_library(arraw-app-ui STATIC
        src/app/ui/CopySettingsDialog.cpp
        src/app/ui/CopySettingsDialog.h
        src/app/ui/CropOverlay.cpp
        src/app/ui/CropOverlay.h
        src/app/ui/CullingActions.cpp
        src/app/ui/CullingActions.h
        src/app/ui/CurveEditor.cpp
        src/app/ui/CurveEditor.h
        src/app/ui/DebugWindow.cpp
        src/app/ui/DebugWindow.h
        src/app/ui/DevelopPanel.cpp
        src/app/ui/DevelopPanel.h
        src/app/ui/ExportDialog.cpp
        src/app/ui/ExportDialog.h
        src/app/ui/FilmStrip.cpp
        src/app/ui/FilmStrip.h
        src/app/ui/MainWindow.cpp
        src/app/ui/MainWindow.h
        src/app/ui/PhotoView.cpp
        src/app/ui/PhotoView.h
        src/app/ui/RenderProgressBar.cpp
        src/app/ui/RenderProgressBar.h
        src/app/ui/SettingsDialog.cpp
        src/app/ui/SettingsDialog.h
        src/app/ui/SettingSlider.cpp
        src/app/ui/SettingSlider.h
        src/app/ui/Theme.cpp
        src/app/ui/Theme.h
        src/app/ui/ThemeColors.h)
arraw_enable_warnings(arraw-app-ui)
target_include_directories(arraw-app-ui PUBLIC src/app)
target_link_libraries(arraw-app-ui PUBLIC arraw-app-core Qt6::Widgets PRIVATE arraw-trace)
set_target_properties(arraw-app-ui PROPERTIES AUTOMOC ON)

add_executable(arraw-ui src/app/main.cpp resources/icon.qrc)
arraw_enable_warnings(arraw-ui)
target_link_libraries(arraw-ui PRIVATE arraw-app-ui)
target_compile_definitions(arraw-ui PRIVATE ARRAW_VERSION="${PROJECT_VERSION}")
# qt_standard_project_setup() turns on AUTOMOC only; the window icon comes
# in through resources/icon.qrc.
set_target_properties(arraw-ui PROPERTIES AUTORCC ON)
arraw_deploy_qt_plugins(arraw-ui)
if(MSVC)
    # activeCodePage UTF-8: narrow argv and path conversions are UTF-8 (needs Windows 10 1903+).
    target_sources(arraw-ui PRIVATE src/platform/arraw.manifest)
endif()

# The command line's behaviour, split from its main() so the tests can drive it
# in-process and assert on what reaches each stream. See ADR 006.
add_library(arraw-cli-core STATIC
        src/cli/Cli.cpp
        src/cli/Cli.h
        src/cli/Command.cpp
        src/cli/Command.h
        src/cli/DeviceChoice.cpp
        src/cli/DeviceChoice.h
        src/cli/ExportCommand.cpp
        src/cli/ExportCommand.h
        src/cli/GpuTestCommand.cpp
        src/cli/GpuTestCommand.h
        src/cli/InfoCommand.cpp
        src/cli/InfoCommand.h
        src/cli/ShotInputs.cpp
        src/cli/ShotInputs.h
        src/cli/StreamDiagnostics.cpp
        src/cli/StreamDiagnostics.h
        src/cli/TerminalStyle.h
        src/cli/TerminalText.h
        src/cli/TerminalText.cpp)
arraw_enable_warnings(arraw-cli-core)
target_include_directories(arraw-cli-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
# Qt Core only: the command line needs QCommandLineParser and an application
# instance (QCoreApplication or QGuiApplication, see arraw-cli below) for codec
# plugin paths. The GPU probe reaches a device
# through arraw-gpu, which keeps QRhi to itself. Widgets must never appear here.
target_link_libraries(arraw-cli-core PUBLIC arraw PRIVATE arraw-gpu arraw-trace Qt6::Core)
target_compile_definitions(arraw-cli-core PRIVATE ARRAW_VERSION="${PROJECT_VERSION}")
set_target_properties(arraw-cli-core PROPERTIES AUTOMOC OFF)

add_executable(arraw-cli src/cli/main.cpp)
arraw_enable_warnings(arraw-cli)
# Qt GUI as well: main() makes a QGuiApplication when a command asks for one,
# as the GPU probe does, and a QCoreApplication otherwise, which loads no
# platform plugin at all. On Linux that QGuiApplication runs on arraw-headless
# unless QT_QPA_PLATFORM names another.
target_link_libraries(arraw-cli PRIVATE arraw-cli-core Qt6::Core Qt6::Gui)
arraw_deploy_qt_plugins(arraw-cli)
if(MSVC)
    target_sources(arraw-cli PRIVATE src/platform/arraw.manifest)
endif()
if(TARGET arraw-headless-platform)
    target_link_libraries(arraw-cli PRIVATE arraw-headless-platform)
endif()

endif() # ARRAW_BUILD_APPS

if(ARRAW_BUILD_PYTHON)
    add_subdirectory(src/python)
endif()

if(ARRAW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
