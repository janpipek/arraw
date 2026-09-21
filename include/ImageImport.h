#pragma once

#include <filesystem>
#include <ImageBuffer.h>

namespace arraw {
ImageBuffer loadImage(const std::filesystem::path& path);

}