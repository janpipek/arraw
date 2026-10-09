#pragma once

#include "Fixtures.h"
#include "TestImages.h"

#include <ColorEncoding.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <RenderCheckpoint.h>

#include <QByteArray>
#include <QCryptographicHash>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// Helpers for the render digest, a tool for refactors that must not change a pixel.
///
/// Both digest cases (`tests/test_RenderDigest.cpp`, `tests/gpu/test_GpuRenderDigest.cpp`)
/// render the same fixed matrix of sources, states and requests down every public path,
/// and write one hash per render to the file `ARRAW_DIGEST_OUT` names. Run before and
/// after a change and diff the files: an empty diff is bit-identical output.

namespace arraw::test {

/// @brief Source of the digest matrix, with the name its lines carry.
struct DigestSource {
    std::string name;   ///< Name in a digest line.
    ImageBuffer buffer; ///< Decoded pixels.
};

/// @brief State of the digest matrix, with the name its lines carry.
struct DigestState {
    std::string name;   ///< Name in a digest line.
    DevelopState state; ///< Settings to render.
};

/// @brief Request of the digest matrix, with the name its lines carry.
struct DigestRequest {
    std::string name;      ///< Name in a digest line.
    RenderRequest request; ///< What to render.
};

/// @brief Names the file the digest is appended to.
/// @return The value of `ARRAW_DIGEST_OUT`, or an empty string when it is not set.
[[nodiscard]] inline std::string digestOutputPath() {
    const char* path = std::getenv("ARRAW_DIGEST_OUT");
    return path != nullptr ? path : "";
}

/// @brief Loads the sources of the matrix.
[[nodiscard]] inline std::vector<DigestSource> digestSources() {
    std::vector<DigestSource> sources;
    for (const char* name : {"linear-32x24-warmwb.dng", "bayer-32x24.dng",
                             "testcard-61x41-srgb8.png", "testcard-61x41-alpha8.png"}) {
        sources.push_back({name, loadImage(fixture(name))});
    }
    sources.push_back({"rainbow-97x61", rainbow({97, 61}, PixelFormat::RgbaF32, workingEncoding)});
    return sources;
}

/// @brief Builds the states of the matrix.
[[nodiscard]] inline std::vector<DigestState> digestStates() {
    std::vector<DigestState> states;
    const auto add = [&](const char* name, const std::function<void(DevelopSettings&)>& edit) {
        DevelopSettings settings;
        edit(settings);
        states.push_back({name, DevelopState{settings}});
    };
    const auto tone = [](DevelopSettings& settings) {
        settings.tone.exposure = 0.6F;
        settings.tone.contrast = 35.0F;
        settings.tone.shadows = 45.0F;
        settings.tone.highlights = -40.0F;
        settings.tone.blacks = -25.0F;
        settings.tone.whites = 20.0F;
    };
    const auto curves = [](DevelopSettings& settings) {
        settings.toneCurve.luma.points = {{0.0F, 0.1F}, {0.5F, 0.55F}, {1.0F, 1.0F}};
        settings.toneCurve.red.points = {{0.0F, 0.0F}, {0.5F, 0.4F}, {1.0F, 1.0F}};
    };
    const auto hsl = [](DevelopSettings& settings) {
        settings.hsl.red = {10.0F, 20.0F, -10.0F};
        settings.hsl.blue = {-15.0F, 30.0F, 15.0F};
    };
    const auto blackAndWhite = [](DevelopSettings& settings) {
        settings.blackAndWhite.convertToGrayscale = true;
        settings.blackAndWhite.red = 20.0F;
        settings.blackAndWhite.blue = -30.0F;
    };
    const auto grading = [](DevelopSettings& settings) {
        settings.colorGrading.shadows = {220.0F, 30.0F};
        settings.colorGrading.midtones = {0.0F, 10.0F};
        settings.colorGrading.highlights = {40.0F, 25.0F};
        settings.colorGrading.balance = 10.0F;
    };
    const auto chroma = [](DevelopSettings& settings) {
        settings.color.saturation = 30.0F;
        settings.color.vibrance = 40.0F;
    };
    const auto noise = [](DevelopSettings& settings) {
        settings.noiseReduction.luminance = 40.0F;
        settings.noiseReduction.color = 50.0F;
    };
    const auto straighten = [](DevelopSettings& settings) {
        settings.geometry.straighten = 7.5;
        settings.geometry.crop.rectangle = UprightCropRect{0.1, 0.1, 0.9, 0.85};
    };
    const auto orientation = [](DevelopSettings& settings) {
        settings.geometry.rotation = QuarterTurn::Clockwise90;
        settings.geometry.flipHorizontal = true;
    };
    const auto effects = [](DevelopSettings& settings) {
        settings.effects.vignette = {-40.0F, 50.0F, 50.0F};
        settings.effects.grain = {30.0F, 40.0F, 50.0F, GrainModel::ValueNoise, 1234U};
    };

    add("default", [](DevelopSettings&) {});
    add("tone", tone);
    add("tone-filmic", [&](DevelopSettings& settings) {
        tone(settings);
        settings.tone.filmicHighlights = 60.0F;
    });
    add("curves", curves);
    add("hsl", hsl);
    add("black-and-white", blackAndWhite);
    add("colour-grading", grading);
    add("chroma", chroma);
    add("texture", [](DevelopSettings& settings) { settings.presence.texture = 50.0F; });
    add("clarity", [](DevelopSettings& settings) { settings.presence.clarity = 40.0F; });
    add("dehaze-plus", [](DevelopSettings& settings) { settings.presence.dehaze = 60.0F; });
    add("dehaze-minus", [](DevelopSettings& settings) { settings.presence.dehaze = -60.0F; });
    add("noise-reduction", noise);
    add("straighten-crop", straighten);
    add("orientation-flip", orientation);
    add("effects", effects);
    add("everything", [&](DevelopSettings& settings) {
        tone(settings);
        settings.tone.filmicHighlights = 60.0F;
        curves(settings);
        hsl(settings);
        grading(settings);
        chroma(settings);
        settings.presence = {50.0F, 40.0F, 30.0F};
        noise(settings);
        straighten(settings);
        orientation(settings);
        effects(settings);
    });
    return states;
}

/// @brief Builds the requests of the matrix.
[[nodiscard]] inline std::vector<DigestRequest> digestRequests() {
    std::vector<DigestRequest> requests;
    requests.push_back({"full", {}});
    requests.push_back(
        {"fit-40", {.size = RenderRequest::FitInside{40, 40}, .filter = ResizeFilter::Lanczos3}});
    requests.push_back(
        {"scale-half", {.size = RenderRequest::Scale{0.5}, .filter = ResizeFilter::Bilinear}});
    requests.push_back({"region-fit-20",
                        {.size = RenderRequest::FitInside{20, 20},
                         .region = RenderRequest::Region{0.2, 0.1, 0.7, 0.9}}});
    return requests;
}

/// @brief Names the stops of a render, for a digest line.
/// @param stage Boundary to name.
[[nodiscard]] inline const char* digestStageName(Stage stage) {
    switch (stage) {
    case Stage::Denoise:
        return "denoise";
    case Stage::Pointwise:
        return "pointwise";
    case Stage::Geometry:
        return "geometry";
    case Stage::Resize:
        return "resize";
    case Stage::Effects:
        return "effects";
    }
    return "unknown";
}

/// @brief Every boundary, in pipeline order.
inline constexpr std::array<Stage, 5> digestStages{Stage::Denoise, Stage::Pointwise,
                                                   Stage::Geometry, Stage::Resize, Stage::Effects};

/// @brief Appends digest lines to the output file.
class DigestWriter {
public:
    /// @brief Opens the file for appending.
    /// @param path File to append to.
    explicit DigestWriter(const std::string& path) : out_(path, std::ios::app) {}

    /// @brief Hashes a render and appends its line.
    /// @param name Name of the case.
    /// @param image Rendered pixels.
    void add(const std::string& name, const ImageBuffer& image) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const std::size_t rowBytes =
            static_cast<std::size_t>(image.size().width) * bytesPerPixel(image.format());
        const auto bytes = image.bytes();
        for (std::uint32_t row = 0; row < image.size().height; ++row) {
            hash.addData(QByteArrayView(
                reinterpret_cast<const char*>(bytes.data() + row * image.rowStride()), rowBytes));
        }
        out_ << name << ' ' << image.size().width << 'x' << image.size().height << ' '
             << static_cast<int>(image.format()) << ' ' << hash.result().toHex().toStdString()
             << '\n';
    }

    /// @brief Appends a line for a render that threw.
    /// @param name Name of the case.
    /// @param message What the exception said.
    void addFailure(const std::string& name, const std::string& message) {
        out_ << name << " threw " << message << '\n';
    }

    /// @brief Renders one case and appends its line, whatever it throws.
    /// @param name Name of the case.
    /// @param render Produces the pixels.
    void render(const std::string& name, const std::function<ImageBuffer()>& render) {
        try {
            add(name, render());
        } catch (const std::exception& error) {
            addFailure(name, error.what());
        }
    }

private:
    std::ofstream out_; ///< File being appended to.
};

} // namespace arraw::test
