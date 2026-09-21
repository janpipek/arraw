#include "Photo.h"

using namespace arraw;

Photo arraw::openPhoto(const std::filesystem::path& path, DiagnosticLog& log) {
    return {path, readImageMetadata(path, log)};
}
