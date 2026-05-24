#pragma once
#include "ImagePipeline.h"
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
class RawProcessor {
public:
    static LoadResult  load(const QString& path,
                            std::function<void(ImageBuffer)> onEmbeddedPreview = nullptr,
                            std::shared_ptr<std::atomic<bool>> cancel = nullptr);

    // Standalone fast path: extracts the embedded preview without demosaicing.
    static ImageBuffer loadEmbeddedPreview(const QString& path);
};
