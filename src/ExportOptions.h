#pragma once

#include "pipeline/ColorManagement.h"

struct ExportOptions {
    enum class Format { JPEG, PNG, TIFF };
    Format format = Format::JPEG;
    int width = 0;
    int height = 0;
    int quality = 90;
    int sharpening = 0;
    OutputProfile profile = OutputProfile::SRgb;
    int bitDepth = 8; // 16 only for TIFF
};
