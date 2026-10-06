#pragma once

#include <SettingDescriptors.h>

#include <QString>

#include <span>
#include <string_view>

namespace arraw::app {

/// @brief How slider positions are spread across a setting's range.
enum class SliderScale {
    /// @brief Equal steps in the setting's own units.
    Linear,

    /// @brief Equal steps in the reciprocal of the value.
    ///
    /// For a colour temperature: equal steps in mired (a million over the
    /// kelvin) look like equal steps of colour, where equal kelvin steps crowd
    /// all the change into the warm end.
    Reciprocal,
};

/// @brief What a slider's groove shows behind the handle.
enum class SliderTrack {
    /// @brief The style's own groove.
    Plain,

    /// @brief The hues of the Oklab wheel, as ::arraw::app::oklabHueColour paints them.
    ///
    /// For a setting that is a hue angle in degrees, so that the colour under
    /// the handle is the one the setting means.
    OklabHue,
};

/// @brief Slider positions across a reciprocal range.
inline constexpr int reciprocalTickCount = 1000;

/// @brief Presentation of one setting, derived from the model (ADR 008).
struct SettingPresentation {
    /// @brief Localised name shown beside the control.
    QString label;

    /// @brief Suffix shown after the number, such as " EV"; may be empty.
    QString unit;

    /// @brief Digits shown after the decimal point.
    int decimals;

    /// @brief Distance between two values, in the setting's own units.
    ///
    /// For a linear slider also the distance between two positions. A
    /// reciprocal slider has its own positions, and values it yields are
    /// rounded to this.
    double step;

    /// @brief Localised one-sentence explanation for a photographer.
    QString toolTip;

    /// @brief How the slider spreads positions across the range.
    SliderScale scale = SliderScale::Linear;

    /// @brief What the slider's groove shows.
    SliderTrack track = SliderTrack::Plain;
};

/// @brief Gives the keys of the Tone group in the order the panel shows them.
[[nodiscard]] std::span<const std::string_view> toneKeys() noexcept;

/// @brief Gives the keys of the White Balance rows, in the order the panel shows them.
///
/// The rows with a slider; the preset combo has no key.
[[nodiscard]] std::span<const std::string_view> whiteBalanceKeys() noexcept;

/// @brief Gives the keys of the Color group's rows (Saturation, Vibrance), in panel order.
[[nodiscard]] std::span<const std::string_view> colorKeys() noexcept;

/// @brief Tells how many pages the HSL box has: Hue, Saturation and Luminance.
inline constexpr int hslPageCount = 3;

/// @brief Gives the keys of one page of the HSL box, one per band, in panel order.
/// @param page 0 for Hue, 1 for Saturation, 2 for Luminance.
/// @throws std::out_of_range if @p page is not within 0 to ::arraw::app::hslPageCount - 1.
[[nodiscard]] std::span<const std::string_view> hslKeys(int page);

/// @brief Gives the keys of the Black & White mix, one per band, in panel order.
[[nodiscard]] std::span<const std::string_view> blackAndWhiteKeys() noexcept;

/// @brief Gives the keys of the Colour Grading group's rows, in panel order.
///
/// Shadows, Midtones and Highlights each as hue then saturation, then Balance and Blending.
[[nodiscard]] std::span<const std::string_view> colorGradingKeys() noexcept;

/// @brief Gives the keys of the Noise Reduction group's rows, in panel order.
///
/// Luminance and its Detail, then Colour and its Smoothness. The luminance filter has one value
/// so far: a choice of one is no row.
[[nodiscard]] std::span<const std::string_view> noiseReductionKeys() noexcept;

/// @brief Gives the keys of the Effects group's rows, in panel order.
///
/// The post-crop vignette's amount, midpoint and feather, then the grain's amount, size and
/// roughness. The grain's model has one value so far and its seed is the photograph's own
/// (::arraw::SettingScope::Photo), so neither has a row.
[[nodiscard]] std::span<const std::string_view> effectsKeys() noexcept;

/// @brief Gives the keys of the Crop group's slider rows, in panel order: the Angle.
[[nodiscard]] std::span<const std::string_view> geometryKeys() noexcept;

/// @brief Gives the keys of the settings the Crop group's buttons and menu edit.
///
/// Quarter-turns, flips, the crop rectangle and its aspect: edited through the crop rules
/// (::arraw::app::CropEditing) rather than a slider row, so they have no presentation.
[[nodiscard]] std::span<const std::string_view> geometryButtonKeys() noexcept;

/// @brief Gives the keys of the settings the curve editor shows: the four tone curves.
///
/// Edited by the curve editor rather than a slider row, so they have no
/// presentation. Listed so that the test of what the panel shows can count
/// them; the editor itself writes curves through ::arraw::app::curveOf.
[[nodiscard]] std::span<const std::string_view> toneCurveKeys() noexcept;

/// @brief Which of the groups that depend on the treatment the panel shows.
struct TreatmentVisibility {
    /// @brief Whether the Color group (Saturation, Vibrance) is shown.
    bool color;

    /// @brief Whether the HSL box is shown.
    bool hsl;

    /// @brief Whether the Black & White mix is shown.
    bool blackAndWhiteMix;
};

/// @brief Decides which groups are shown for a treatment.
///
/// Black & White replaces Color and HSL with its own mix (ADR 027); White Balance
/// and Tone are always shown and so are not part of the answer.
/// @param grayscale Whether the photograph is converted to grayscale.
[[nodiscard]] constexpr TreatmentVisibility visibleGroups(bool grayscale) noexcept {
    return {!grayscale, !grayscale, grayscale};
}

/// @brief Finds the presentation of a setting.
/// @param key camelCase key of a setting the panel shows, such as one of ::arraw::app::toneKeys.
/// @return The presentation, valid for the life of the program.
/// @throws std::out_of_range if no presentation exists for @p key.
[[nodiscard]] const SettingPresentation& presentationOf(std::string_view key);

/// @brief Counts the slider steps across a range.
/// @param range Range of the setting; positive for a reciprocal scale.
/// @param step Distance between two steps, in the setting's units; positive.
/// @param scale How steps are spread; a reciprocal one has ::reciprocalTickCount of them.
/// @return The number of steps from minimum to maximum.
[[nodiscard]] int tickCount(const SettingRange& range, double step,
                            SliderScale scale = SliderScale::Linear);

/// @brief Maps a value to the nearest slider step.
/// @param value Value in the setting's units.
/// @param range Range of the setting.
/// @param step Distance between two steps, in the setting's units; positive.
/// @param scale How steps are spread.
/// @return The step, clamped to 0 through tickCount().
[[nodiscard]] int tickOf(double value, const SettingRange& range, double step,
                         SliderScale scale = SliderScale::Linear);

/// @brief Maps a slider step to its value.
///
/// On a reciprocal scale the value is rounded to a multiple of @p step, so
/// that dragging yields tidy numbers, then clamped to the range.
/// @param tick Slider step; need not lie within the range.
/// @param range Range of the setting.
/// @param step Distance between two steps, in the setting's units; positive.
/// @param scale How steps are spread.
/// @return The value, clamped to the range.
[[nodiscard]] double valueOfTick(int tick, const SettingRange& range, double step,
                                 SliderScale scale = SliderScale::Linear);

/// @brief Reads the value a setting has in a set of defaults.
/// @param descriptor Row of a float or double setting.
/// @param defaults Settings to read it from: a photograph's (::arraw::defaultStateFor), or
/// the neutral ones of a default-constructed ::arraw::DevelopSettings.
/// @return The default value.
/// @throws std::invalid_argument if the setting is neither float nor double.
[[nodiscard]] double defaultValueOf(const FieldDescriptor& descriptor,
                                    const DevelopSettings& defaults = DevelopSettings{});

} // namespace arraw::app
