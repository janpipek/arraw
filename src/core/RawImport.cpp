#include "RawImport.h"

#include "ColorSpaces.h"

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

/// @brief Extensions ::arraw::loadImage routes to LibRaw by name.
///
/// The set published in docs/desired-features.md, no wider -- and it does not
/// need to be. ::holdsRawImage asks LibRaw about everything else before Qt is
/// offered anything, so a .3fr, a .mrw or a renamed .dng reaches the same
/// decoder one file open later. Widening the list would only move where the
/// error for a non-RAW file comes from, and for an extension as generic as
/// .raw it would report a RAW failure for a file Qt can read.
constexpr std::array<std::string_view, 10> rawExtensions = {".cr2", ".cr3", ".nef", ".arw", ".dng",
                                                            ".raf", ".orf", ".rw2", ".pef", ".srw"};

/// @brief LibRaw's `output_color` value for "leave it in the camera's space".
///
/// LibRaw inherits dcraw's bare integers here and validates nothing — an
/// out-of-range value is silently treated as sRGB rather than reported — so the
/// one value arraw uses is named once, next to the reason it is that value:
/// a per-channel gain is only a white balance in the space the sensor
/// recorded, so the conversion out of it belongs to development (ADR 007).
constexpr int outputColorCameraNative = 0;

/// @brief LibRaw's `user_qual` value for the AHD demosaic.
constexpr int demosaicAhd = 3;

/// @brief Deleter for images allocated by `dcraw_make_mem_image`.
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
/// The handle must already be open: one of the decisions depends on what the
/// file turned out to declare.
/// @brief Checks whether the file declares an as-shot neutral LibRaw can use.
/// @param raw Opened handle.
/// @return `true` when the camera recorded a white balance.
bool hasCameraNeutral(const LibRaw& raw) {
    const auto& colour = raw.imgdata.color;
    return colour.cam_mul[0] > 0.0F && colour.cam_mul[2] > 0.0F;
}

/// @brief Normalises per-channel gains so green is 1.
/// @param gains Multipliers in camera channel order; green must be non-zero.
/// @return The same ratios, with green at 1.
Gains normalised(const float* gains) {
    const float green = gains[1] > 0.0F ? gains[1] : 1.0F;
    return {gains[0] / green, gains[1] / green, gains[2] / green};
}

void applyDecodeSettings(LibRaw& raw) {
    auto& params = raw.imgdata.params;

    // A content-dependent brightness stretch would make one develop setting
    // render differently from frame to frame, which a non-destructive editor
    // cannot have.
    params.no_auto_bright = 1;

    // The same stretch reaches the image by a second route, and disabling one
    // without the other leaves it there: LibRaw lowers the white level to the
    // frame's own brightest sample whenever that sample lands within
    // `adjust_maximum_thr` (0.75 by default) of the declared one. Two frames
    // of a bracket would then scale differently.
    params.adjust_maximum_thr = 0.0;

    // Linear light: the working space carries no transfer function, and the
    // shadows need the sixteen bits to be linear ones.
    params.gamm[0] = 1.0;
    params.gamm[1] = 1.0;
    params.output_bps = 16;
    params.output_color = outputColorCameraNative;

    // The camera's as-shot neutral, never LibRaw's guess from the histogram --
    // including when there is no as-shot neutral to apply. `use_camera_wb`
    // alone does not say that: a file that declares none falls through to
    // exactly the automatic white balance `use_auto_wb = 0` refuses, and a
    // flat colour field comes back grey. Switching it off instead leaves the
    // daylight multipliers the camera's colour matrix implies. A fixed wrong
    // white balance beats a plausible-looking one that moves with the scene,
    // because only the fixed one can be corrected once for a whole shoot.
    //
    // It should also be *said*: this is a silent substitution, and the frame
    // will not look as its camera intended. It becomes a warning on the
    // import path as soon as there is somewhere to put one.
    params.use_camera_wb = hasCameraNeutral(raw) ? 1 : 0;
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
/// @brief Describes the camera's colour, to travel with the decoded pixels.
///
/// Read after `unpack` and before any processing: `scale_colors` overwrites
/// `pre_mul` with the gains it applied, and its original meaning — the row
/// scales LibRaw normalised out of the camera matrix — is then gone. Those
/// scales are not recoverable from `rgb_cam`, which is invariant to them, and
/// without them a temperature cannot be resolved into channel gains (ADR 007).
/// @param raw Unpacked handle.
/// @return The sensor's encoding, including what the camera recorded and what
/// the decode will apply.
CameraNative cameraColour(const LibRaw& raw) {
    const auto& colour = raw.imgdata.color;

    Matrix3 cameraToSrgb;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            cameraToSrgb.values[row * 3 + column] = colour.rgb_cam[row][column];
        }
    }

    CameraNative camera;
    camera.toWorking = colorspaces::srgbToWorking * cameraToSrgb;
    camera.daylightScale = {colour.pre_mul[0], colour.pre_mul[1], colour.pre_mul[2]};
    camera.asShotMultipliers =
        hasCameraNeutral(raw) ? normalised(colour.cam_mul) : Gains{1.0F, 1.0F, 1.0F};
    // `use_camera_wb` is off for a file that declares no neutral, and LibRaw
    // then scales by the daylight multipliers its matrix implies (ADR 005).
    camera.appliedMultipliers =
        hasCameraNeutral(raw) ? camera.asShotMultipliers : normalised(colour.pre_mul);
    return camera;
}

ImageBuffer toBuffer(const libraw_processed_image_t& image, ColorEncoding encoding) {
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

    ImageBuffer buffer({image.width, image.height}, PixelFormat::RgbaU16, std::move(encoding));
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

    // Before processing, which rewrites part of what this reads.
    const CameraNative camera = cameraColour(raw);

    if (const int code = raw.dcraw_process(); code != LIBRAW_SUCCESS) {
        throw std::runtime_error(describe(path, code));
    }

    int code = LIBRAW_SUCCESS;
    const ProcessedImage image(raw.dcraw_make_mem_image(&code));
    if (!image) {
        throw std::runtime_error(describe(path, code));
    }
    return toBuffer(*image, camera);
}
