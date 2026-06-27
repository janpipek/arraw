#pragma once

#include "ui/ImageViewport.h"

ImageViewport::ActiveTool toolForAdjustmentTab(
    int currentTab, int masksTab, int spotsTab, ImageViewport::ActiveTool currentTool, bool enabled);
