#pragma once

#include <filesystem>
#include <utility>

#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <ImageImport.h>

namespace arraw {

/// @brief One photograph as a document: what it is, and how it is developed.
///
/// A photograph exists as a document only once its parts are coherent, so it
/// cannot be half a photograph: there is no default, and no setter that could
/// leave it describing one file while pointing at another. Its pixels are not
/// part of it — decoding is the renderer's business (ADR 001) — and what a
/// render is planned against is this, not a buffer somebody else loaded and a
/// settings struct that travelled separately (ADR 012).
///
/// It is a value. Developing a photograph differently makes another document
/// rather than changing this one, which is how a preset, a before-and-after,
/// an export while the edit continues, and the command line's `--exposure` are
/// all expressed. That stays cheap because a document holds no pixels.
class Photo {
public:
    /// @brief Builds a document from parts that already agree.
    ///
    /// ::arraw::openPhoto is the usual way in; this is for a caller that has
    /// already read the file, such as one restoring a document it stored.
    /// @param path File the photograph was read from.
    /// @param metadata What that file declares about itself.
    /// @param settings How it is developed.
    Photo(std::filesystem::path path, ImageMetadata metadata, DevelopSettings settings = {})
        : path_(std::move(path)), metadata_(std::move(metadata)), settings_(settings) {}

    /// @brief File the photograph was read from.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// @brief What that file declares about itself.
    [[nodiscard]] const ImageMetadata& metadata() const noexcept {
        return metadata_;
    }

    /// @brief How the photograph is developed.
    [[nodiscard]] const DevelopSettings& settings() const noexcept {
        return settings_;
    }

    /// @brief Makes the same photograph, developed differently.
    /// @param settings Settings the new document carries.
    /// @return A document over the same file, leaving this one alone.
    [[nodiscard]] Photo with(DevelopSettings settings) const {
        return {path_, metadata_, settings};
    }

    friend bool operator==(const Photo&, const Photo&) = default;

private:
    std::filesystem::path path_;
    ImageMetadata metadata_;
    DevelopSettings settings_;
};

/// @brief Opens a photograph as a document, reading no pixels.
///
/// Reads what the file declares — its dimensions and the encoding its pixels
/// will arrive in — and pairs it with default develop settings. Decoding
/// happens when something asks for pixels, which a document never does.
///
/// @param path File to open.
/// @param log Where to report what a photographer should know about the file,
/// such as a white balance it did not record.
/// @return The document.
/// @throws std::runtime_error if the file cannot be opened or is not an image
/// arraw recognises.
[[nodiscard]] Photo openPhoto(const std::filesystem::path& path,
                              DiagnosticLog& log = discardedDiagnostics());

} // namespace arraw
