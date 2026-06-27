#include "ui/AdjustmentTabTool.h"

ImageViewport::ActiveTool toolForAdjustmentTab(
    int currentTab, int masksTab, int spotsTab, ImageViewport::ActiveTool currentTool, bool enabled) {
    if (enabled && currentTab == masksTab)
        return ImageViewport::ActiveTool::LocalMask;
    if (enabled && currentTab == spotsTab)
        return ImageViewport::ActiveTool::SpotTool;
    if (currentTool == ImageViewport::ActiveTool::LocalMask
        || currentTool == ImageViewport::ActiveTool::SpotTool)
        return ImageViewport::ActiveTool::None;
    return currentTool;
}
