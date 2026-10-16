#pragma once

#include <DevelopState.h>
#include <Diagnostics.h>
#include <ImageImport.h>
#include <NegativeSettings.h>
#include <PhotoMarks.h>

#include <filesystem>
#include <utility>

namespace arraw {

/// @brief One photograph as a document: what it is, and how it is developed.
///
/// A photograph exists as a document only once its parts are coherent, so it
/// cannot be half a photograph: there is no default, and no setter that could
/// leave it describing one file while pointing at another. Its pixels are not
/// part of it — decoding is the renderer's business (ADR 001) — and what a
/// render is planned against is this, not a buffer somebody else loaded and a
/// state that travelled separately (ADR 012).
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
    /// @param state How it is developed.
    /// @param marks How it is culled.
    /// @details The negative settings are ::arraw::defaultNegativeFor @p metadata.
    /// @throws std::invalid_argument if @p state is not valid (see ::arraw::validate),
    /// or if the rating of @p marks is outside -1 to 5, so an invalid
    /// photograph cannot exist.
    Photo(std::filesystem::path path, ImageMetadata metadata, DevelopState state,
          PhotoMarks marks = {});

    /// @brief Builds a document from parts that already agree, negative settings included.
    /// @param path File the photograph was read from.
    /// @param metadata What that file declares about itself.
    /// @param negative How its negative is decoded.
    /// @param state How it is developed.
    /// @param marks How it is culled.
    /// @throws std::invalid_argument as the constructor without @p negative does, and if
    /// @p negative is not valid (see ::arraw::validate).
    Photo(std::filesystem::path path, ImageMetadata metadata, NegativeSettings negative,
          DevelopState state, PhotoMarks marks = {});

    /// @brief Builds a document nobody has edited: the state its kind starts from, no marks.
    ///
    /// The state is ::arraw::defaultStateFor the encoding @p metadata declares,
    /// so a RAW starts with its colour noise reduction (ADR 039).
    /// @param path File the photograph was read from.
    /// @param metadata What that file declares about itself.
    Photo(std::filesystem::path path, ImageMetadata metadata);

    /// @brief File the photograph was read from.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// @brief What that file declares about itself.
    [[nodiscard]] const ImageMetadata& metadata() const noexcept {
        return metadata_;
    }

    /// @brief How the photograph's negative is decoded.
    [[nodiscard]] const NegativeSettings& negative() const noexcept {
        return negative_;
    }

    /// @brief How the photograph is developed.
    [[nodiscard]] const DevelopState& state() const noexcept {
        return state_;
    }

    /// @brief How the photograph is culled.
    [[nodiscard]] const PhotoMarks& marks() const noexcept {
        return marks_;
    }

    /// @brief Makes the same photograph, developed differently.
    /// @param state State the new document carries.
    /// @return A document over the same file with the same marks, leaving this one alone.
    /// @throws std::invalid_argument as the constructor does.
    [[nodiscard]] Photo with(DevelopState state) const {
        return {path_, metadata_, negative_, std::move(state), marks_};
    }

    /// @brief Makes the same photograph, decoded differently.
    /// @param negative Negative settings the new document carries.
    /// @return A document over the same file with the same state and marks, leaving this one alone.
    /// @throws std::invalid_argument as the constructor does.
    [[nodiscard]] Photo with(NegativeSettings negative) const {
        return {path_, metadata_, negative, state_, marks_};
    }

    /// @brief Makes the same photograph, culled differently.
    /// @param marks Marks the new document carries.
    /// @return A document over the same file with the same state, leaving this one alone.
    /// @throws std::invalid_argument as the constructor does.
    [[nodiscard]] Photo with(PhotoMarks marks) const {
        return {path_, metadata_, negative_, state_, marks};
    }

    friend bool operator==(const Photo&, const Photo&) = default;

private:
    std::filesystem::path path_;
    ImageMetadata metadata_;
    NegativeSettings negative_;
    DevelopState state_;
    PhotoMarks marks_;
};

/// @brief Gives the options a photograph is decoded with.
///
/// The one place a document becomes decode options (ADR 049): it reads the photograph's
/// negative settings and what its file declares, and never its develop state, so no develop
/// edit can change a decode. Every caller that holds a photograph and decodes it uses this.
/// @param photo Document to decode.
/// @param halfSize Whether the decode is the reduced one (::arraw::DecodeOptions::halfSize).
/// @return The options in canonical form (::arraw::canonical), so that two documents which
/// decode to the same pixels give equal options.
[[nodiscard]] DecodeOptions decodeOptionsFor(const Photo& photo, bool halfSize);

/// @brief Opens a photograph as a document, reading no pixels.
///
/// Reads what the file declares — its dimensions and the encoding its pixels
/// will arrive in — and pairs it with the state and marks of its sidecar
/// (see ::arraw::readSidecar). Without a sidecar, or with one that records no
/// develop settings (one of marks only, or another tool's), the state is the
/// one its kind starts from (::arraw::defaultStateFor), and without a sidecar
/// there are no marks. Decoding happens when something asks for pixels, which a
/// document never does.
///
/// @param path File to open.
/// @param log Where to report what a photographer should know about the file,
/// such as a white balance it did not record.
/// @return The document.
/// @throws std::runtime_error if the file cannot be opened or is not an image
/// arraw recognises. A sidecar that is not readable XML is not thrown: it is
/// reported as ::arraw::Notice::SidecarUnreadable and the photograph opens as
/// if it had none.
[[nodiscard]] Photo openPhoto(const std::filesystem::path& path,
                              DiagnosticLog& log = discardedDiagnostics());

} // namespace arraw
