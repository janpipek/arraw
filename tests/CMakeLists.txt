# Catch2 v3: prefer a system install, fall back to FetchContent
# (see DESIGN.md "Testing Strategy").
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

add_executable(arraw_tests
    test_BasicTone.cpp
    test_OkLab.cpp
    test_ImagePipeline.cpp
    test_CurveLut.cpp
    test_ViewportGeometry.cpp
    test_XmpSidecar.cpp
    test_ColorManagement.cpp
    test_WhiteBalance.cpp
    test_RawProcessor.cpp
    test_StandardImageLoader.cpp
    test_GoldenImages.cpp
    test_HeadlessRenderContext.cpp
    test_ImageLoadWorkflow.cpp
    test_ImageMetadata.cpp
    test_FilmStripModel.cpp
    test_FilmStripFilter.cpp
    test_FilmStripFilterModel.cpp
    test_FilmStripLayout.cpp
    test_FilmStripTooltip.cpp
    test_ExportWorkflow.cpp
    test_ExportMetadata.cpp
    test_MainWindowStatus.cpp
    test_MainWindowZoom.cpp
    test_ThumbnailCache.cpp
    test_FieldSpec.cpp
    test_LocalAdjustment.cpp
    test_LocalAdjustmentPanel.cpp
    test_AdjustmentPanel.cpp
    test_InfoPanel.cpp
    test_AdjustmentTabTool.cpp
    test_CollapsiblePane.cpp
    test_ChromeHider.cpp
    test_CropGeometry.cpp
    test_DecodeCache.cpp
    test_DevelopSession.cpp
    test_DevelopGroup.cpp
    test_DevelopParameter.cpp
    test_DevelopPreset.cpp
    test_Orientation.cpp
    test_GroupChecklistDialog.cpp
    test_PresetStore.cpp
    test_Spot.cpp
    test_BrushMask.cpp
    test_NoiseReduction.cpp
    test_DemosaicAlgorithm.cpp
    test_LensCorrection.cpp
    test_LensfunSource.cpp
    test_BatchPaste.cpp
    test_FilmStrip.cpp
    test_ImageGrouping.cpp
    test_ToneCurveWidget.cpp
    test_Histogram.cpp
    test_ImageViewport.cpp
    test_PendingHistogram.cpp
    test_HistoryPanelLocal.cpp
    test_CliExportArgs.cpp
    test_CliExportPreflight.cpp
    test_CliExportCommand.cpp
    test_CliDispatch.cpp
    test_CliPresetArgs.cpp
    test_CliPresetCommand.cpp
    test_CliInfoCommand.cpp
    test_CliInfoArgs.cpp
)

target_link_libraries(arraw_tests PRIVATE arraw_cli arraw_ui Catch2::Catch2WithMain)

target_compile_definitions(arraw_tests PRIVATE
    ARRAW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ARRAW_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# Deploy Qt platform plugins before catch_discover_tests runs the exe at build time.
arraw_deploy_qt_plugins(arraw_tests)

include(Catch)
catch_discover_tests(arraw_tests
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
