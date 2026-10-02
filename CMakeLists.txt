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
find_package(Qt6 6.10 REQUIRED COMPONENTS Core CorePrivate Gui GuiPrivate Widgets Concurrent
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

# The engine library. Everything photographic lives here; the executables are
# thin front ends over it. Headers under include/ are the public API and are
# propagated to consumers through target_link_libraries.
add_library(arraw STATIC
        include/ColorEncoding.h
        include/ColorSettings.h
        src/core/ColorSettings.cpp
        include/Develop.h
        include/Diagnostics.h
        include/WhiteBalance.h
        include/DevelopSettings.h
        include/GeometrySettings.h
        include/ImageOrientation.h
        src/core/GeometryPlan.h
        src/core/GeometryPlan.cpp
        src/core/Resample.h
        src/core/Resample.cpp
        include/ToneSettings.h
        src/core/ToneSettings.cpp
        src/core/ColorSpaces.h
        src/core/Develop.cpp
        src/core/Diagnostics.cpp
        src/core/ProcessingPlan.cpp
        src/core/ProcessingPlan.h
        include/RenderCheckpoint.h
        src/core/CheckpointState.h
        src/core/RenderCheckpoint.cpp
        src/gpu/DeviceImage.h
        src/gpu/DeviceImageState.h
        src/gpu/DeviceImage.cpp
        src/core/WhiteBalance.cpp
        include/ImageBuffer.h
        include/ImageExport.h
        include/EditSession.h
        include/Photo.h
        src/core/Photo.cpp
        include/SettingDescriptors.h
        src/core/SettingDescriptors.cpp
        src/core/SettingCodec.h
        src/core/SettingCodec.cpp
        include/SettingsJson.h
        src/core/SettingsJson.cpp
        include/PhotoMarks.h
        include/Sidecar.h
        src/core/Sidecar.cpp
        src/core/ImageBuffer.cpp
        src/core/ImageExport.cpp
        include/ImageImport.h
        src/core/ImageImport.cpp
        src/core/QtImage.cpp
        src/core/QtImage.h
        src/core/RawImport.cpp
        src/core/RawImport.h
        src/core/SampleConversion.h
        src/core/SampleConversion.cpp
)
add_library(arraw::arraw ALIAS arraw)
arraw_enable_warnings(arraw)
target_include_directories(arraw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The GPU module's own headers name no graphics type — the QRhi texture lives
# behind DeviceImageState, whose device-side subclass is arraw-gpu's — so core
# may hold a device image without Qt reaching either the engine's headers or the
# public API. Only arraw-gpu links Qt6::GuiPrivate, and the tests that drive its
# QRhi device directly (tests/test_GpuDevice.cpp).
target_include_directories(arraw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
# Qt GUI holds QImage and the image codecs (QImageReader/QImageWriter); it needs
# no display and no QGuiApplication, only a QCoreApplication for plugin paths.
# Private: nothing in include/ mentions Qt, and CMake still propagates the link
# requirement to consumers of this static library.
target_link_libraries(arraw PRIVATE Qt6::Core Qt6::Gui Qt6::Xml ${ARRAW_LIBRAW})
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
target_include_directories(arraw-gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
# The processing plan and checkpoint state are engine-private; the GPU packs its
# uniforms from the one and builds the other.
target_include_directories(arraw-gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
# The passes' shaders, compiled at build time into .qsb resources, so a shader
# that does not compile fails the build rather than the first render. GLSL
# 440 sources; qsb adds SPIR-V for Vulkan and translates for the rest. The GLSL
# targets start at ES 3.0 and 3.30, the first with texelFetch and uint.
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
        src/gpu/shaders/geometry.frag)
target_link_libraries(arraw-gpu PUBLIC arraw PRIVATE Qt6::Gui Qt6::GuiPrivate)
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

add_executable(arraw-ui src/app/main.cpp
        src/app/DebugDiagnostics.cpp
        src/app/DebugDiagnostics.h
        src/app/DisplayImage.cpp
        src/app/DisplayImage.h
        src/app/ui/MainWindow.cpp
        src/app/ui/MainWindow.h)
arraw_enable_warnings(arraw-ui)
target_include_directories(arraw-ui PRIVATE src/app)
target_link_libraries(arraw-ui PRIVATE arraw Qt6::Widgets)
arraw_deploy_qt_plugins(arraw-ui)

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
        src/cli/StreamDiagnostics.cpp
        src/cli/StreamDiagnostics.h)
arraw_enable_warnings(arraw-cli-core)
target_include_directories(arraw-cli-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
# Qt Core only: the command line needs QCommandLineParser and an application
# instance (QCoreApplication or QGuiApplication, see arraw-cli below) for codec
# plugin paths. The GPU probe reaches a device
# through arraw-gpu, which keeps QRhi to itself. Widgets must never appear here.
target_link_libraries(arraw-cli-core PUBLIC arraw PRIVATE arraw-gpu Qt6::Core)
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
