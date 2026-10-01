#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace arraw {

/// @brief Colour a photograph is labelled with while culling.
enum class ColorLabel { Red, Yellow, Green, Blue, Purple };

/// @brief Names of the colour labels, as Lightroom writes `xmp:Label`.
inline constexpr std::array<std::pair<ColorLabel, std::string_view>, 5> colorLabelNames{{
    {ColorLabel::Red, "Red"},
    {ColorLabel::Yellow, "Yellow"},
    {ColorLabel::Green, "Green"},
    {ColorLabel::Blue, "Blue"},
    {ColorLabel::Purple, "Purple"},
}};

/// @brief Rating that marks a photograph as rejected.
inline constexpr int rejectedRating = -1;

/// @brief Highest star rating.
inline constexpr int highestRating = 5;

/// @brief Culling marks of a photograph, which are not develop settings.
struct PhotoMarks {
    /// @brief Rating, from ::arraw::rejectedRating through no stars (0) to ::arraw::highestRating.
    int rating = 0;

    /// @brief Colour label, absent when there is none.
    std::optional<ColorLabel> label = std::nullopt;

    friend bool operator==(const PhotoMarks&, const PhotoMarks&) = default;
};

} // namespace arraw
