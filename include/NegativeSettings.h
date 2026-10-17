#pragma once

#include <array>
#include <cstddef>
#include <optional>
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

/// @brief What shapes the decoded negative, as opposed to what is done with it (ADR 049).
///
/// Held by ::arraw::Photo beside its ::arraw::DevelopState. Changing it changes the
/// pixels the decode hands to development, so it costs a decode; no develop edit can reach it.
struct NegativeSettings {
    /// @brief Algorithm that turns the sensor's mosaic into colour.
    ///
    /// Applies only to a mosaiced RAW decoded at full size.
    Demosaic demosaic = Demosaic::Ahd;

    friend bool operator==(const NegativeSettings&, const NegativeSettings&) = default;
};

/// @brief Checks negative settings.
/// @param settings Settings to check.
/// @throws std::invalid_argument if the demosaic is not an enumerator.
void validate(const NegativeSettings& settings);

/// @brief Gives the negative settings a photograph of some kind starts from.
/// @param metadata What the photograph's file declares.
/// @return The default: AHD, which is what every decode used before the setting existed.
[[nodiscard]] NegativeSettings defaultNegativeFor(const ImageMetadata& metadata);

} // namespace arraw
