#include "Photo.h"

#include <SettingDescriptors.h>

#include <utility>

using namespace arraw;

Photo::Photo(std::filesystem::path path, ImageMetadata metadata, DevelopSettings settings)
    : path_(std::move(path)), metadata_(std::move(metadata)), settings_(settings) {
    validate(settings_);
}

Photo arraw::openPhoto(const std::filesystem::path& path, DiagnosticLog& log) {
    return {path, readImageMetadata(path, log)};
}
