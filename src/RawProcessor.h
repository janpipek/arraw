#pragma once
#include "ImagePipeline.h"
#include <QString>

// Thin wrapper around libraw. Call from a background thread.
// Returns a LoadResult with fullRes + preview (1/4-res) on success,
// or LoadResult::error non-empty on failure.
class RawProcessor {
public:
    static LoadResult load(const QString& path);
};
