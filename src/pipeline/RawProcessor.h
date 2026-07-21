#pragma once
#include "develop/DemosaicAlgorithm.h"
#include "pipeline/LoadResult.h"
#include <atomic>
#include <functional>
#include <memory>
#include <QString>

// Thin wrapper around libraw. Call from a background thread.
// Returns a LoadResult with fullRes + preview (1/4-res) on success,
// or LoadResult::error non-empty on failure.
//
// If onEmbeddedPreview is provided, it is called synchronously (on the
// calling thread) with the embedded JPEG preview converted to a linear
// ImageBuffer, before the slower full demosaic begins.
class LibRaw;
class QImage;

class RawProcessor {
public:
    // Returns true when path has a supported RAW extension. Mirrors
    // StandardImageLoader::canLoad; the two together are "is this a
    // supported image" (docs/adr/0051).
    static bool canLoad(const QString& path);

    static LoadResult load(
        const QString& path,
        std::function<void(ImageBuffer)> onEmbeddedPreview = nullptr,
        std::shared_ptr<std::atomic<bool>> cancel = nullptr,
        DemosaicAlgorithm algo = kDefaultDemosaic);

    // Standalone fast path: extracts the embedded preview without demosaicing.
    static ImageBuffer loadEmbeddedPreview(const QString& path);

    // Decode the embedded JPEG/bitmap thumbnail of an already-open LibRaw
    // handle into an sRGB QImage. Null image if there is no usable thumbnail.
    static QImage extractThumbImage(LibRaw& raw);
};
