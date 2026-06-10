#pragma once
#include "ImagePipeline.h"
#include <QImage>

// Color profiles offered for export. The working space itself (linear Rec.2020)
// is deliberately not on the list — see docs/adr/0001 and 0002.
enum class OutputProfile { SRgb, DisplayP3, AdobeRgb };

// Convert any QImage into a linear-light Rec.2020 working-space buffer.
// Honours the image's embedded ICC profile; assumes sRGB when untagged.
// Safe to call off the main thread.
ImageBuffer toWorkingSpaceBuffer(const QImage& img);

// Encode a linear Rec.2020 float image (Format_RGBX32FPx4) into the given
// output profile. Returns Format_RGB888 (8-bit) or Format_RGBA64 (16-bit),
// tagged with the matching QColorSpace so the profile is embedded on save.
QImage toOutputImage(const QImage& linearWorking, OutputProfile profile, bool sixteenBit);
