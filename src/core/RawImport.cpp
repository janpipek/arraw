#include "RawImport.h"

#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace arraw;

namespace {

/// @brief Extensions ::arraw::loadImage routes to LibRaw.
///
/// The set published in docs/desired-features.md, no wider: an extension here
/// means "decode this with LibRaw and report a failure", so adding one that
/// LibRaw cannot read would turn a fallback into an error.
constexpr std::array<std::string_view, 10> rawExtensions = {".cr2", ".cr3", ".nef", ".arw", ".dng",
                                                            ".raf", ".orf", ".rw2", ".pef", ".srw"};

/// @brief LibRaw's `output_color` value for linear Rec.2020 primaries.
///
/// LibRaw inherits dcraw's bare integers here and validates nothing — an
/// out-of-range value is silently treated as sRGB rather than reported — so the
/// one value arraw uses is named once, next to the reason it is that value:
/// it is ::arraw::workingEncoding.
constexpr int outputColorRec2020 = 8;

/// @brief LibRaw's `user_qual` value for the AHD demosaic.
constexpr int demosaicAhd = 3;

/// @brief Releases an image allocated by `dcraw_make_mem_image`.
struct ProcessedImageDeleter {
    void operator()(libraw_processed_image_t* image) const {
        LibRaw::dcraw_clear_mem(image);
    }
};

using ProcessedImage = std::unique_ptr<libraw_processed_image_t, ProcessedImageDeleter>;

/// @brief Opens a file with LibRaw, without unpacking its pixels.
/// @return LibRaw's result code.
int openFile(LibRaw& raw, const std::filesystem::path& path) {
#ifdef _WIN32
    // std::filesystem::path holds wchar_t here, and LibRaw's wide overload is
    // the only one that can express a path outside the active code page.
    return raw.open_file(path.wstring().c_str());
#else
    return raw.open_file(path.c_str());
#endif
}

/// @brief Builds the message for a failed LibRaw call.
std::string describe(const std::filesystem::path& path, int code) {
    return path.string() + ": " + LibRaw::strerror(code);
}

/// @brief Applies arraw's decode settings to an opened handle.
///
/// Every value here is a decision rather than a default; ADR 005 records them.
void applyDecodeSettings(LibRaw& raw) {
    auto& params = raw.imgdata.params;

    // A content-dependent brightness stretch would make one develop setting
    // render differently from frame to frame, which a non-destructive editor
    // cannot have.
    params.no_auto_bright = 1;

    // Linear light: the working space carries no transfer function, and the
    // shadows need the sixteen bits to be linear ones.
    params.gamm[0] = 1.0;
    params.gamm[1] = 1.0;
    params.output_bps = 16;
    params.output_color = outputColorRec2020;

    // The camera's as-shot neutral, never LibRaw's guess from the histogram.
    params.use_camera_wb = 1;
    params.use_auto_wb = 0;

    params.user_qual = demosaicAhd;

    // LibRaw defaults to -1, "rotate to the camera's orientation". arraw keeps
    // rotation a develop setting, so the buffer stays in sensor geometry --
    // which is also the frame lens-correction profiles are described in.
    params.user_flip = 0;

    // Clip highlights rather than reconstructing them. Recovery is a develop
    // decision, and a sixteen-bit integer output has no headroom to carry the
    // unclipped values into anyway.
    params.highlight = 0;
}

/// @brief Copies LibRaw's interleaved output into a buffer, adding opaque alpha.
///
/// LibRaw emits three (or, for a monochrome sensor, one) channels; arraw's
/// wide layouts are RGBA, so the alpha channel is synthesised here rather than
/// leaving the buffer in a layout ::arraw::exportImage cannot write back.
ImageBuffer toBuffer(const libraw_processed_image_t& image) {
    if (image.type != LIBRAW_IMAGE_BITMAP) {
        throw std::runtime_error("LibRaw returned a thumbnail rather than an image");
    }
    if (image.bits != 16) {
        throw std::runtime_error("LibRaw returned " + std::to_string(image.bits) +
                                 " bits per channel, expected 16");
    }
    if (image.colors != 3 && image.colors != 1) {
        throw std::runtime_error("LibRaw returned " + std::to_string(image.colors) +
                                 " channels, expected 1 or 3");
    }

    ImageBuffer buffer({image.width, image.height}, PixelFormat::RgbaU16, workingEncoding);
    const auto source =
        std::span(reinterpret_cast<const std::uint16_t*>(image.data),
                  static_cast<std::size_t>(image.data_size) / sizeof(std::uint16_t));
    const auto destination = buffer.samples<std::uint16_t>();

    const auto pixels = static_cast<std::size_t>(image.width) * image.height;
    const auto channels = static_cast<std::size_t>(image.colors);
    if (source.size() < pixels * channels) {
        throw std::runtime_error("LibRaw returned fewer samples than its dimensions imply");
    }

    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const auto* in = &source[pixel * channels];
        auto* out = &destination[pixel * 4];
        // A monochrome sensor's single channel becomes neutral RGB.
        out[0] = in[0];
        out[1] = channels == 3 ? in[1] : in[0];
        out[2] = channels == 3 ? in[2] : in[0];
        out[3] = 65535;
    }
    return buffer;
}

} // namespace

bool arraw::rawimport::namesRawFormat(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return std::ranges::find(rawExtensions, extension) != rawExtensions.end();
}

bool arraw::rawimport::holdsRawImage(const std::filesystem::path& path) {
    LibRaw raw;
    return openFile(raw, path) == LIBRAW_SUCCESS;
}

ImageBuffer arraw::rawimport::load(const std::filesystem::path& path) {
    LibRaw raw;
    if (const int code = openFile(raw, path); code != LIBRAW_SUCCESS) {
        throw std::runtime_error(describe(path, code));
    }

    applyDecodeSettings(raw);

    if (const int code = raw.unpack(); code != LIBRAW_SUCCESS) {
        throw std::runtime_error(describe(path, code));
    }
    if (const int code = raw.dcraw_process(); code != LIBRAW_SUCCESS) {
        throw std::runtime_error(describe(path, code));
    }

    int code = LIBRAW_SUCCESS;
    const ProcessedImage image(raw.dcraw_make_mem_image(&code));
    if (!image) {
        throw std::runtime_error(describe(path, code));
    }
    return toBuffer(*image);
}
