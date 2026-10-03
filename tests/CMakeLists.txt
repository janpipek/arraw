# Catch2 v3: prefer a system install, fall back to a pinned source build so the
# suite still configures on a machine (or runner) without one.
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.13.0
            GIT_SHALLOW ON)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

include(Catch)

add_executable(arraw-tests
        main.cpp
        support/ImageCompare.cpp
        support/TestImages.cpp
        test_AppSettings.cpp
        test_Cli.cpp
        test_ColorEncoding.cpp
        test_Develop.cpp
        test_ToneCurve.cpp
        test_DevelopState.cpp
        test_DisplayImage.cpp
        test_EditSession.cpp
        test_EmbeddedPreview.cpp
        test_ExifInfo.cpp
        test_ExportMetadata.cpp
        test_ExportQueue.cpp
        test_ExportSettings.cpp
        test_FilmStripRules.cpp
        test_Geometry.cpp
        test_Diagnostics.cpp
        test_ColorAdjustments.cpp
        test_GpuContext.cpp
        test_GpuDevice.cpp
        test_GpuPlan.cpp
        test_ImageBuffer.cpp
        test_ImageExport.cpp
        test_ImagePyramid.cpp
        test_Photo.cpp
        test_PreviewRenderer.cpp
        test_ProcessingPlan.cpp
        test_Region.cpp
        test_RenderCheckpoint.cpp
        test_Resample.cpp
        test_RawImport.cpp
        test_SettingCodec.cpp
        test_SettingDescriptors.cpp
        test_SettingPresentation.cpp
        test_SettingsJson.cpp
        test_Shot.cpp
        test_ShotModel.cpp
        test_Sidecar.cpp
        test_SampleConversion.cpp
        test_RoundTrip.cpp
        test_ThumbnailCache.cpp
        test_ThumbnailWorker.cpp
        test_TimingTrace.cpp
        test_Tone.cpp
        test_ViewTransform.cpp
        test_WhiteBalance.cpp
        test_WhiteBalanceChoice.cpp)
arraw_enable_warnings(arraw-tests)

# The processing plan is engine-private: the order of the pointwise stages is
# not something a caller sets, but it is the thing most worth testing directly
# (ADR 011), so the tests see the engine's own headers.
target_include_directories(arraw-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu)

# The integration tests read committed fixtures (see tests/fixtures/README.md).
# Baking the directory in beats resolving it against the working directory, so
# the suite behaves the same from ctest, from an IDE, and from a shell.
target_compile_definitions(arraw-tests
        PRIVATE ARRAW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# Catch2 (not Catch2WithMain): main.cpp runs the session inside a QCoreApplication.
# GuiPrivate for QRhi, which test_GpuDevice drives directly on its Null backend:
# the one place outside arraw-gpu that names it, because the failures it covers
# cannot be caused through GpuContext.
target_link_libraries(arraw-tests
        PRIVATE arraw arraw-gpu arraw-app-core arraw-cli-core Catch2::Catch2 Qt6::Gui Qt6::GuiPrivate Qt6::Xml
        # exiv2 itself, to write the EXIF tags test_ExifInfo.cpp reads back.
        ${ARRAW_EXIV2})
set_target_properties(arraw-tests PROPERTIES AUTOMOC OFF)
arraw_deploy_qt_plugins(arraw-tests)

# Widget behavior runs separately because the core suite owns a QCoreApplication.
add_executable(arraw-widget-tests
        test_SettingsDialog.cpp
        ../src/app/ui/SettingsDialog.cpp
        ../src/app/ui/SettingsDialog.h)
arraw_enable_warnings(arraw-widget-tests)
target_link_libraries(arraw-widget-tests PRIVATE arraw-app-core Qt6::Widgets Catch2::Catch2)
set_target_properties(arraw-widget-tests PROPERTIES AUTOMOC ON)
arraw_deploy_qt_plugins(arraw-widget-tests)
catch_discover_tests(arraw-widget-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Registers each TEST_CASE with CTest individually, so `ctest -R` can select one.
catch_discover_tests(arraw-tests)

# arraw's headless platform, in its own executable: it needs a QGuiApplication
# on that platform, and the suite above needs its QCoreApplication. Linux only,
# as the platform is. A runner without a Vulkan driver skips the device cases,
# and a software one (lavapipe) passes them: the platform is under test, not
# the hardware.
if(TARGET arraw-headless-platform)
    add_executable(arraw-headless-tests
            headless/main.cpp
            headless/test_Headless.cpp)
    arraw_enable_warnings(arraw-headless-tests)
    target_link_libraries(arraw-headless-tests
            PRIVATE arraw arraw-gpu arraw-headless-platform Catch2::Catch2 Qt6::Gui)
    set_target_properties(arraw-headless-tests PROPERTIES AUTOMOC OFF)
    # Catch2 exits 4 for a skipped test. Recent Catch.cmake tells CTest so
    # itself; older ones, such as Ubuntu 24.04's 3.4, would report a failure.
    catch_discover_tests(arraw-headless-tests PROPERTIES SKIP_RETURN_CODE 4)
endif()

# The passes on a real device, in their own executable for the same reason as
# above: a QGuiApplication, which creates one shared device before the suite
# runs. Built everywhere; ARRAW_TEST_GPU_BACKEND picks the backend (default: the
# platform's own, as arraw-cli) and main.cpp the Qt platform. On Linux that is
# arraw's headless platform, except for OpenGL, which honours QT_QPA_PLATFORM;
# Windows and macOS use Qt's native one. Software rasterisers are accepted,
# since the passes' correctness is under test, not the hardware; a machine with
# no device for the backend skips every case (exit 4). Only Vulkan on lavapipe
# has been run: see ADR 017.
add_executable(arraw-gpu-tests
        gpu/main.cpp
        gpu/GpuTesting.cpp
        gpu/GpuTesting.h
        gpu/test_GpuAdapters.cpp
        gpu/test_GpuDevelop.cpp
        gpu/test_GpuExport.cpp
        gpu/test_GpuPasses.cpp
        gpu/test_GpuPointwise.cpp
        gpu/test_GpuGeometry.cpp
        gpu/test_GpuResize.cpp)
arraw_enable_warnings(arraw-gpu-tests)
target_include_directories(arraw-gpu-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu)
# Later steps load tests/fixtures/*.dng, as arraw-tests does.
target_compile_definitions(arraw-gpu-tests
        PRIVATE ARRAW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(arraw-gpu-tests
        PRIVATE arraw arraw-gpu arraw-cli-core Catch2::Catch2 Qt6::Gui)
# The headless platform exists on Linux only; its INTERFACE defines
# ARRAW_HEADLESS_PLATFORM, which main.cpp tests.
if(TARGET arraw-headless-platform)
    target_link_libraries(arraw-gpu-tests PRIVATE arraw-headless-platform)
endif()
set_target_properties(arraw-gpu-tests PROPERTIES AUTOMOC OFF)
arraw_deploy_qt_plugins(arraw-gpu-tests)
catch_discover_tests(arraw-gpu-tests PROPERTIES SKIP_RETURN_CODE 4)

# The real arraw-cli binary, for what only main() decides: which Qt application
# is made, when, and from which arguments. A platform that does not exist makes
# Qt abort the moment one is loaded, so a command that exits cleanly under it
# never loaded one.
set(arraw_expect_exit ${CMAKE_CURRENT_SOURCE_DIR}/cli/ExpectExit.cmake)
set(arraw_no_platform "QT_QPA_PLATFORM=arraw-no-such-platform")
add_test(NAME arraw-cli.help-loads-no-platform
        COMMAND ${CMAKE_COMMAND} -DEXPECTED_EXIT=0 -P ${arraw_expect_exit}
        -- $<TARGET_FILE:arraw-cli> --help)
add_test(NAME arraw-cli.usage-error-loads-no-platform
        COMMAND ${CMAKE_COMMAND} -DEXPECTED_EXIT=2 -DEXPECTED_ERR=--size -P ${arraw_expect_exit}
        -- $<TARGET_FILE:arraw-cli> gpu-test --size 0)
set_tests_properties(arraw-cli.help-loads-no-platform arraw-cli.usage-error-loads-no-platform
        PROPERTIES ENVIRONMENT ${arraw_no_platform})
# An operand after `--` is an input, even one Qt would take for its own option.
add_test(NAME arraw-cli.escaped-operand-is-an-input
        COMMAND ${CMAKE_COMMAND} -DEXPECTED_EXIT=1 -DEXPECTED_ERR=-reverse -P ${arraw_expect_exit}
        -- $<TARGET_FILE:arraw-cli> export -o ${CMAKE_CURRENT_BINARY_DIR} -- -reverse)

# An export never aborts for a platform it does not need: a platform Qt cannot
# start (xcb without a display) is bypassed for arraw's headless one, so the
# export runs on the GPU or falls back, and exits 0 either way.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME arraw-cli.export-ignores-unstartable-platform
            COMMAND ${CMAKE_COMMAND} -DEXPECTED_EXIT=0 -P ${arraw_expect_exit}
            -- $<TARGET_FILE:arraw-cli> export
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/testcard-61x41-srgb8.png
            -o ${CMAKE_CURRENT_BINARY_DIR} --overwrite)
    set_tests_properties(arraw-cli.export-ignores-unstartable-platform
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;DISPLAY=;WAYLAND_DISPLAY=")
endif()

# Every executable CTest runs, so that one build step brings them all up
# to date: `just test` builds this, and a target CTest registers but nothing
# builds would fail as "not built", or run stale.
add_custom_target(arraw-test-binaries)
add_dependencies(arraw-test-binaries arraw-widget-tests)
add_dependencies(arraw-test-binaries arraw-tests arraw-cli)
add_dependencies(arraw-test-binaries arraw-gpu-tests)
if(TARGET arraw-headless-tests)
    add_dependencies(arraw-test-binaries arraw-headless-tests)
endif()
