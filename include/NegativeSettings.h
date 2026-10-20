#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace arraw {

struct ImageMetadata;

/// @brief Demosaic algorithm of a mosaiced RAW, one of LibRaw's built-ins.
///
/// The enumerators are LibRaw's `user_qual` 0, 1, 2, 3, 4, 11 and 12; the mapping lives in
/// the RAW decoder and nowhere else, and what is persisted is the token
/// (::arraw::tokenOf), never a number (ADR 005, ADR 049).
enum class Demosaic { Linear, Vng, Ppg, Ahd, Dcb, Dht, Aahd };

/// @brief Number of ::arraw::Demosaic enumerators.
inline constexpr std::size_t demosaicCount = 7;

/// @brief Stable lower-case tokens of the demosaics, in enumerator order.
///
/// What a sidecar, a preset and the command line call each algorithm. They never change.
inline constexpr std::array<std::string_view, demosaicCount> demosaicTokens = {
    "linear", "vng", "ppg", "ahd", "dcb", "dht", "aahd"};

/// @brief Gives the stable token of a demosaic.
/// @param demosaic Enumerator to name.
/// @return The token, or an empty view if @p demosaic is not an enumerator.
[[nodiscard]] std::string_view tokenOf(Demosaic demosaic) noexcept;

/// @brief Reads a token back into a demosaic.
/// @param token Candidate token; case-sensitive, lower case only.
/// @return The demosaic, or `std::nullopt` for a token that is not in ::arraw::demosaicTokens.
[[nodiscard]] std::optional<Demosaic> parseDemosaic(std::string_view token) noexcept;

/// @brief How the lens recorded in a photograph's negative settings came to be chosen.
enum class LensOrigin {
    Matched, ///< Found from the file's EXIF when a correction was first turned on.
    Chosen,  ///< Picked by the photographer.
};

/// @brief Number of ::arraw::LensOrigin enumerators.
inline constexpr std::size_t lensOriginCount = 2;

/// @brief Stable lower-case tokens of the lens origins, in enumerator order.
///
/// What a sidecar spells them as. They never change.
inline constexpr std::array<std::string_view, lensOriginCount> lensOriginTokens = {"matched",
                                                                                   "chosen"};

/// @brief Gives the stable token of a lens origin.
/// @param origin Enumerator to name.
/// @return The token, or an empty view if @p origin is not an enumerator.
[[nodiscard]] std::string_view tokenOf(LensOrigin origin) noexcept;

/// @brief Reads a token back into a lens origin.
/// @param token Candidate token; case-sensitive, lower case only.
/// @return The origin, or `std::nullopt` for a token that is not in ::arraw::lensOriginTokens.
[[nodiscard]] std::optional<LensOrigin> parseLensOrigin(std::string_view token) noexcept;

/// @brief The lens a photograph's corrections were found for, by name only (ADR 050, section 1).
///
/// Recorded when a correction is first turned on (a switch on, or the vignetting amount leaving 0),
/// so that a lens database update cannot move an edit to another lens. Only the identity is kept:
/// the calibration stays the database's, and a revised one shows as a notice, not as a stored
/// number.
struct RecordedLens {
    /// @brief Maker of the lens, exactly as lensfun spells it.
    std::string maker;

    /// @brief Model of the lens, exactly as lensfun spells it.
    std::string model;

    /// @brief Crop factor of the image, as the match used it.
    double cropFactor = 1.0;

    /// @brief How the lens was chosen.
    LensOrigin origin = LensOrigin::Matched;

    /// @brief Stamp of the lens database at the time of recording.
    std::string database;

    friend bool operator==(const RecordedLens&, const RecordedLens&) = default;
};

/// @brief Vignetting amount that corrects nothing.
inline constexpr float minimumVignettingAmount = 0.0F;

/// @brief Vignetting amount that applies the lens profile as it is.
inline constexpr float profileVignettingAmount = 100.0F;

/// @brief Largest vignetting amount.
inline constexpr float maximumVignettingAmount = 150.0F;

/// @brief Which lens corrections a photograph asks for, and the lens they were found for.
///
/// All off by default, for new photographs too (ADR 050). They apply to a RAW only. Distortion and
/// lateral CA are switches; vignetting is an amount, since a profile can be too strong for a body.
struct LensSettings {
    /// @brief Whether to take geometric distortion out.
    bool distortion = false;

    /// @brief Share of the profile's vignetting stops to lift, in percent: 0 is off, 100 the
    /// profile.
    float vignettingAmount = 0.0F;

    /// @brief Whether to realign the colour fringes at the edges.
    bool lateralCa = false;

    /// @brief The lens recorded for these corrections, absent until one is first turned on.
    ///
    /// Taking the corrections off keeps it.
    std::optional<RecordedLens> lens;

    /// @brief Checks whether any correction is asked for.
    [[nodiscard]] constexpr bool any() const noexcept {
        return distortion || vignettingAmount > 0.0F || lateralCa;
    }

    friend bool operator==(const LensSettings&, const LensSettings&) = default;
};

/// @brief Gives lens settings with the corrections of another and their own recorded lens.
/// @param settings Settings whose recorded lens is kept.
/// @param from Settings whose corrections (the switches and the vignetting amount) are taken.
/// @return The settings with the corrections of `from`.
[[nodiscard]] constexpr LensSettings withCorrectionsOf(LensSettings settings,
                                                       const LensSettings& from) noexcept {
    settings.distortion = from.distortion;
    settings.vignettingAmount = from.vignettingAmount;
    settings.lateralCa = from.lateralCa;
    return settings;
}

/// @brief What shapes the decoded negative, as opposed to what is done with it (ADR 049).
///
/// Held by ::arraw::Photo beside its ::arraw::DevelopState. No develop edit can reach it.
/// The demosaic changes the pixels the decode hands to development, so it costs a decode; the
/// lens corrections do not, since they are a stage of the render (ADR 050) and so stay out of
/// ::arraw::DecodeOptions.
struct NegativeSettings {
    /// @brief Algorithm that turns the sensor's mosaic into colour.
    ///
    /// Applies only to a mosaiced RAW decoded at full size.
    Demosaic demosaic = Demosaic::Ahd;

    /// @brief Lens corrections asked for, and the lens they are for.
    ///
    /// Applies to a RAW only.
    LensSettings lens{};

    friend bool operator==(const NegativeSettings&, const NegativeSettings&) = default;
};

/// @brief Checks negative settings.
/// @param settings Settings to check.
/// @throws std::invalid_argument if the demosaic is not an enumerator, or a recorded lens has no
/// model, a crop factor that is not finite and positive, or an origin that is not an enumerator, or
/// if the vignetting amount is not finite or lies outside the range from
/// ::arraw::minimumVignettingAmount to ::arraw::maximumVignettingAmount.
void validate(const NegativeSettings& settings);

/// @brief Gives the negative settings a photograph of some kind starts from.
/// @param metadata What the photograph's file declares.
/// @return The default: AHD, which is what every decode used before the setting existed, and no
/// lens correction.
[[nodiscard]] NegativeSettings defaultNegativeFor(const ImageMetadata& metadata);

} // namespace arraw
