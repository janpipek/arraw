#pragma once
#include "pipeline/LoadResult.h"
#include <atomic>
#include <memory>
#include <QString>

// Loads standard image formats (JPEG, PNG, TIFF, etc.) via QImage.
// Complements RawProcessor for non-RAW files — same LoadResult contract,
// but no embedded-preview callback (standard formats load fast as a whole).
class StandardImageLoader {
public:
    // Returns true when path has a supported standard image extension.
    static bool canLoad(const QString& path);

    static LoadResult load(const QString& path, std::shared_ptr<std::atomic<bool>> cancel = nullptr);
};
