#pragma once
#include "core/DisplayLut.h"
#include "core/ImageBuffer.h"
#include <QImage>
#include <QList>

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

// ── Soft-proofing / monitor profiles (CONTEXT.md, DESIGN.md ColorManagement) ─

enum class ProofIntent { Perceptual, RelativeColorimetric };

// DisplayLut (the 3D display LUT this builds) now lives in core/ so the renderer
// can consume the type without a render→pipeline edge.

// Build the working→[proof→]display LUT. Empty proofIcc = no proofing (plain
// working→display, gamut alpha all 1); empty monitorIcc = sRGB display.
// Returns an invalid LUT if a profile fails to load.
DisplayLut buildDisplayLut(
    const QString& proofIcc,
    ProofIntent intent,
    bool blackPointCompensation,
    const QString& monitorIcc);

struct IccProfileInfo {
    QString path;
    QString description;
    bool isDisplayClass = false; // RGB display-class — usable as monitor profile
};

// Scan the OS color-profile directories for usable proof/monitor profiles
// (display- and output-class).
QList<IccProfileInfo> scanSystemProfiles();
