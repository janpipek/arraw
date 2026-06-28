#pragma once
#include "core/ImageBuffer.h"
#include "core/ImageMetadata.h"
#include "core/Orientation.h"
#include "develop/UserMetadata.h"
#include "pipeline/LensCorrection.h"
#include <QRectF>
#include <QString>

// Returned by the background load task.
struct LoadResult {
    ImageBuffer fullRes; // stored for export only
    ImageBuffer preview; // 1/4-res (half W, half H) — used for viewport + histogram
    // Sensor Clipping mask from RAW mosaic values, before demosaic,
    // normalisation, and develop settings. Invalid for standard images.
    ImageBuffer sensorClipFullRes;
    ImageBuffer sensorClipPreview;
    ImageMetadata metadata;
    UserMetadata embeddedMetadata; // descriptive User Metadata from embedded XMP, if any
    UserMetadataPresence embeddedMetadataPresence;
    QString error; // non-empty on failure
    QRectF defaultCrop = {0.0, 0.0, 1.0, 1.0};
    // Lens profile resolved at decode (docs/adr/0032). Empty has* flags = no
    // profile matched; correction is applied (toggle-gated) downstream.
    LensCorrectionModel lensModel{};
    // Coarse Orientation read from the file's EXIF/camera flag, used to seed the
    // develop edit when the sidecar has none (docs/adr/0029). The decoded buffer
    // stays in native orientation — this only records what the camera intended.
    orient::Orientation seededOrientation = {};
    // libraw's `imgdata.idata.filters` mosaic mask, surfaced so the UI can gate
    // the demosaic control to Bayer sensors (ADR 0036). 0 for non-RAW/standard
    // images and non-mosaic sensors; see sensorSupportsDemosaicSelection.
    unsigned filters = 0;
};
