#include "WhiteBalanceChoice.h"

#include <QCoreApplication>

#include <algorithm>

namespace arraw::app {

namespace {

constexpr std::array<WhiteBalancePreset, 6> presets{{
    {WhiteBalanceChoice::Daylight, "Daylight", {5500.0F, 10.0F}},
    {WhiteBalanceChoice::Cloudy, "Cloudy", {6500.0F, 10.0F}},
    {WhiteBalanceChoice::Shade, "Shade", {7500.0F, 10.0F}},
    {WhiteBalanceChoice::Tungsten, "Tungsten", {2850.0F, 0.0F}},
    {WhiteBalanceChoice::Fluorescent, "Fluorescent", {3800.0F, 21.0F}},
    {WhiteBalanceChoice::Flash, "Flash", {5500.0F, 0.0F}},
}};

constexpr std::array<WhiteBalanceChoice, 8> choices{
    WhiteBalanceChoice::AsShot, WhiteBalanceChoice::Daylight, WhiteBalanceChoice::Cloudy,
    WhiteBalanceChoice::Shade,  WhiteBalanceChoice::Tungsten, WhiteBalanceChoice::Fluorescent,
    WhiteBalanceChoice::Flash,  WhiteBalanceChoice::Custom};

QString tr(const char* text) {
    return QCoreApplication::translate("WhiteBalanceChoice", text);
}

/// @brief Reads a mode's values, ignoring leftovers in As Shot.
ColorSettings settled(ColorSettings settings) {
    if (settings.whiteBalance == WhiteBalanceMode::AsShot) {
        settings.temperature.reset();
        settings.tint.reset();
    }
    return settings;
}

/// @brief Sets the mode that goes with whichever values are present.
ColorSettings resolved(ColorSettings settings) {
    settings.whiteBalance =
        settings.temperature || settings.tint ? WhiteBalanceMode::Custom : WhiteBalanceMode::AsShot;
    return settings;
}

} // namespace

std::span<const WhiteBalancePreset> whiteBalancePresets() noexcept {
    return presets;
}

std::span<const WhiteBalanceChoice> whiteBalanceChoices() noexcept {
    return choices;
}

QString nameOf(WhiteBalanceChoice choice) {
    if (choice == WhiteBalanceChoice::AsShot) {
        return tr("As Shot");
    }
    if (choice == WhiteBalanceChoice::Custom) {
        return tr("Custom");
    }
    for (const WhiteBalancePreset& preset : presets) {
        if (preset.choice == choice) {
            return tr(preset.name);
        }
    }
    return {};
}

WhiteBalanceChoice choiceOf(const ColorSettings& settings) {
    if (settings.whiteBalance == WhiteBalanceMode::AsShot) {
        return WhiteBalanceChoice::AsShot;
    }
    for (const WhiteBalancePreset& preset : presets) {
        if (settings.temperature == preset.light.kelvin && settings.tint == preset.light.tint) {
            return preset.choice;
        }
    }
    return WhiteBalanceChoice::Custom;
}

ColorSettings withChoice(ColorSettings settings, WhiteBalanceChoice choice) {
    if (choice == WhiteBalanceChoice::Custom) {
        return settings;
    }
    if (choice == WhiteBalanceChoice::AsShot) {
        return {WhiteBalanceMode::AsShot, std::nullopt, std::nullopt};
    }
    const auto preset = std::ranges::find(presets, choice, &WhiteBalancePreset::choice);
    if (preset == presets.end()) {
        return settings;
    }
    return {WhiteBalanceMode::Custom, preset->light.kelvin, preset->light.tint};
}

ColorSettings withTemperature(ColorSettings settings, std::optional<float> kelvin) {
    settings = settled(settings);
    settings.temperature = kelvin;
    return resolved(settings);
}

ColorSettings withTint(ColorSettings settings, std::optional<float> tint) {
    settings = settled(settings);
    settings.tint = tint;
    return resolved(settings);
}

ColourTemperature shownLight(const ColorSettings& settings, ColourTemperature asShot) {
    const ColorSettings own = settled(settings);
    return {own.temperature.value_or(asShot.kelvin), own.tint.value_or(asShot.tint)};
}

} // namespace arraw::app
