#include "RawImport.h"

#include "ColorSpaces.h"
#include "ProgressScope.h"

#include <Progress.h>

#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

using namespace arraw;

namespace {

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
std::string failureMessage(const std::filesystem::path& path, int code) {
    return path.string() + ": " + LibRaw::strerror(code);
}

/// @brief Opens a file with LibRaw, or throws saying why it could not.
///
/// LibRaw describes a file that does not exist as a failure of its own, such
/// as "Image too big for processing", so absence is checked first and worded
/// the way the Qt path words it.
/// @throws std::runtime_error naming the file and the reason.
void openOrThrow(LibRaw& raw, const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        const std::error_code reason =
            error ? error : std::make_error_code(std::errc::no_such_file_or_directory);
        throw std::runtime_error(path.string() + ": " + reason.message());
    }
    if (const int code = openFile(raw, path); code != LIBRAW_SUCCESS) {
        throw std::runtime_error(failureMessage(path, code));
    }
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

void applyDecodeSettings(LibRaw& raw, DecodeOptions options) {
    auto& params = raw.imgdata.params;

    // Each 2x2 sensor block becomes a pixel, so nothing is demosaiced; the
    // colour and white balance are those of a full decode (ADR 031).
    params.half_size = options.halfSize ? 1 : 0;

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
    // It is also *said*: the import path reports the substitution, because the
    // frame will not look as its camera intended and the temperature a
    // photographer is shown is an estimate rather than a reading.
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

/// @brief Describes an opened file, for a plan to be resolved against.
///
/// The dimensions are the visible frame's rather than `iwidth`/`iheight`,
/// which LibRaw swaps when it applies an orientation; ::applyDecodeSettings
/// tells it not to, so these are the ones the decode produces.
/// @param raw Opened handle, before any processing.
/// @return What the file declares about itself.
ImageMetadata metadataOf(const LibRaw& raw) {
    const auto& sizes = raw.imgdata.sizes;
    // LibRaw's flip bits are not EXIF orientation numbers.
    constexpr ImageOrientation orientations[]{
        ImageOrientation::Normal,         ImageOrientation::MirrorHorizontal,
        ImageOrientation::MirrorVertical, ImageOrientation::Rotate180,
        ImageOrientation::Transpose,      ImageOrientation::Rotate270,
        ImageOrientation::Rotate90,       ImageOrientation::Transverse};
    const auto orientation =
        sizes.flip >= 0 && sizes.flip < 8 ? orientations[sizes.flip] : ImageOrientation::Normal;
    return {.size = {sizes.width, sizes.height},
            .encoding = cameraColour(raw),
            .orientation = orientation};
}

/// @brief Reports a white balance the camera did not record.
///
/// The frame will not look as its camera intended, and the temperature a
/// photographer is shown is an estimate rather than a reading, so the
/// substitution is said rather than left to be noticed (ADR 005).
/// @param raw Opened handle.
/// @param path File being read, to name in the diagnostic.
/// @param log Where to report it.
void reportSubstitutedWhiteBalance(const LibRaw& raw, const std::filesystem::path& path,
                                   DiagnosticLog& log) {
    if (!hasCameraNeutral(raw)) {
        log.record({.notice = Notice::SubstitutedWhiteBalance,
                    .severity = Severity::Warning,
                    .subject = path});
    }
}

ImageBuffer toBuffer(const libraw_processed_image_t& image, ColorEncoding encoding,
                     ImageOrientation orientation) {
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

    ImageBuffer buffer({image.width, image.height}, PixelFormat::RgbaU16, std::move(encoding),
                       orientation);
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

/// @brief Halves a decoded sixteen-bit buffer by averaging 2x2 blocks.
///
/// What LibRaw's half-size mode does not do for a file that is already
/// demosaiced (a linear DNG): it halves only what it would have demosaiced, so
/// the buffer comes back whole. Halving it here keeps the promise of
/// ::arraw::DecodeOptions::halfSize whatever the file holds, at the cost of
/// the speed-up, which there was none to be had from. The size is the integer
/// quotient, as LibRaw's is; the colour is a plain mean, as the samples are
/// linear and opaque.
/// @param image Buffer in RgbaU16, at least 2x2.
ImageBuffer halvedRgbaU16(const ImageBuffer& image) {
    const ImageSize in = image.size();
    ImageBuffer result({in.width / 2, in.height / 2}, image.format(), image.encoding(),
                       image.orientation());
    const auto input = image.samples<std::uint16_t>();
    auto output = result.samples<std::uint16_t>();
    const auto out = result.size();
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                unsigned sum = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        sum +=
                            input[((2 * y + dy) * static_cast<std::size_t>(in.width) + 2 * x + dx) *
                                      4 +
                                  channel];
                    }
                }
                output[(y * static_cast<std::size_t>(out.width) + x) * 4 + channel] =
                    static_cast<std::uint16_t>((sum + 2) / 4);
            }
        }
    }
    return result;
}

} // namespace

namespace {

/// @brief Gives a path's extension, lower-case and without the dot.
std::string lowerExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    if (!extension.empty()) {
        extension.erase(0, 1);
    }
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

} // namespace

bool arraw::rawimport::namesRawFormat(const std::filesystem::path& path) {
    return std::ranges::find(rawExtensions, lowerExtension(path)) != rawExtensions.end();
}

bool arraw::rawimport::hasRawExtension(const std::filesystem::path& path) {
    return std::ranges::find(openedRawExtensions, lowerExtension(path)) !=
           openedRawExtensions.end();
}

bool arraw::rawimport::holdsRawImage(const std::filesystem::path& path) {
    LibRaw raw;
    return openFile(raw, path) == LIBRAW_SUCCESS;
}

ImageMetadata arraw::rawimport::readMetadata(const std::filesystem::path& path,
                                             DiagnosticLog& log) {
    LibRaw raw;
    openOrThrow(raw, path);

    // No unpack and no processing: the colour description comes out of the
    // headers, and asking for it must not cost a demosaic.
    reportSubstitutedWhiteBalance(raw, path, log);
    return metadataOf(raw);
}

ImageBuffer arraw::rawimport::load(const std::filesystem::path& path, DiagnosticLog& log,
                                   DecodeOptions options) {
    // Three units: the unpack, the processing (the demosaic) and the copy out.
    // LibRaw reports its own stages without a measure of how far each has
    // got, so only the units report; its callback is where it looks for a
    // cancellation, which it may do from its own threads (ADR 042).
    detail::ProgressSpan progress(ProgressStep::Decode, 3);
    const auto failed = [&path](int code) -> std::exception_ptr {
        if (code == LIBRAW_CANCELLED_BY_CALLBACK) {
            return std::make_exception_ptr(Cancelled());
        }
        return std::make_exception_ptr(std::runtime_error(failureMessage(path, code)));
    };
    LibRaw raw;
    openOrThrow(raw, path);
    if (progress.active()) {
        raw.set_progress_handler(
            [](void* span, LibRaw_progress /*stage*/, int /*iteration*/, int /*expected*/) {
                return static_cast<const detail::ProgressSpan*>(span)->cancelled() ? 1 : 0;
            },
            &progress);
    }

    applyDecodeSettings(raw, options);

    if (const int code = raw.unpack(); code != LIBRAW_SUCCESS) {
        std::rethrow_exception(failed(code));
    }
    detail::completeUnit();

    // The same description a caller can ask for on its own, read here before
    // processing, which rewrites part of what it reads.
    const ImageMetadata metadata = metadataOf(raw);
    reportSubstitutedWhiteBalance(raw, path, log);

    if (const int code = raw.dcraw_process(); code != LIBRAW_SUCCESS) {
        std::rethrow_exception(failed(code));
    }
    detail::completeUnit();

    int code = LIBRAW_SUCCESS;
    const ProcessedImage image(raw.dcraw_make_mem_image(&code));
    if (!image) {
        throw std::runtime_error(failureMessage(path, code));
    }
    ImageBuffer decoded = toBuffer(*image, metadata.encoding, metadata.orientation);
    if (options.halfSize && decoded.size() == metadata.size && decoded.size().width >= 2 &&
        decoded.size().height >= 2) {
        decoded = halvedRgbaU16(decoded);
    }
    if (options.halfSize && decoded.size() != metadata.size) {
        // Each pixel is a 2x2 block of the sensor, whoever combined it; a
        // stage measured in sensor pixels reads this (ADR 039).
        decoded.setPixelScale(2.0);
    }
    detail::completeUnit();
    return decoded;
}
