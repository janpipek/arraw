#pragma once

#include <array>

/**
 * Zoom presets shared by the View > Zoom submenu and the status bar's zoom
 * dropdown (MainWindow::addZoomPresetActions builds both from this table),
 * plus the logic that maps a live pixel-zoom value back to a preset for menu
 * checkmarks. Kept Qt-widget-free so it's unit-testable on its own.
 */
constexpr std::array<float, 5> kZoomPresets = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

// Index into kZoomPresets whose value is within tolerance of `pixelZoom`, or
// -1 if pixelZoom doesn't correspond to any preset (e.g. a wheel zoom, or a
// Zoom In/Out step that landed off the preset chain).
int matchingZoomPresetIndex(float pixelZoom);
