#include "SettingPresentation.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace arraw::app {

namespace {

constexpr std::array<std::string_view, 7> toneKeyList{
    "exposure", "contrast", "highlights", "shadows", "whites", "blacks", "filmicHighlights"};

constexpr std::array<std::string_view, 2> whiteBalanceKeyList{"temperature", "tint"};

QString tr(const char* text) {
    return QCoreApplication::translate("SettingPresentation", text);
}

struct Row {
    std::string_view key;
    SettingPresentation presentation;
};

const std::array<Row, 9>& table() {
    // Built on first use, so that translations see the installed translator.
    static const std::array<Row, 9> rows{{
        {"exposure",
         {tr("Exposure"), tr(" EV"), 2, 0.01,
          tr("Brightens or darkens the whole photograph, in stops.")}},
        {"contrast",
         {tr("Contrast"),
          {},
          0,
          1.0,
          tr("Spreads the tones apart around middle grey, or gathers them together.")}},
        {"highlights",
         {tr("Highlights"), {}, 0, 1.0, tr("Recovers or brightens the bright tones.")}},
        {"shadows", {tr("Shadows"), {}, 0, 1.0, tr("Lifts or deepens the dark tones.")}},
        {"whites", {tr("Whites"), {}, 0, 1.0, tr("Moves the white point.")}},
        {"blacks", {tr("Blacks"), {}, 0, 1.0, tr("Moves the black point.")}},
        {"filmicHighlights",
         {tr("Filmic highlights"),
          {},
          0,
          1.0,
          tr("Rolls the brightest values smoothly toward white instead of "
             "clipping them.")}},
        {"temperature",
         {tr("Temp"), tr(" K"), 0, 50.0,
          tr("The temperature of the light the photograph is balanced for. Lower is "
             "bluer, higher is warmer. Double-click the label to follow the camera again."),
          SliderScale::Reciprocal}},
        {"tint",
         {tr("Tint"),
          {},
          0,
          1.0,
          tr("How far toward green (negative) or magenta (positive) the light is. "
             "Double-click the label to follow the camera again.")}},
    }};
    return rows;
}

} // namespace

std::span<const std::string_view> toneKeys() noexcept {
    return toneKeyList;
}

std::span<const std::string_view> whiteBalanceKeys() noexcept {
    return whiteBalanceKeyList;
}

const SettingPresentation& presentationOf(std::string_view key) {
    for (const Row& row : table()) {
        if (row.key == key) {
            return row.presentation;
        }
    }
    throw std::out_of_range("no presentation for setting '" + std::string(key) + "'");
}

namespace {

/// Position of a value along a reciprocal range: 0 at the minimum, 1 at the maximum.
double reciprocalFraction(double value, const SettingRange& range) {
    const double low = 1.0 / range.minimum;
    const double high = 1.0 / range.maximum;
    return (low - 1.0 / value) / (low - high);
}

} // namespace

int tickCount(const SettingRange& range, double step, SliderScale scale) {
    if (scale == SliderScale::Reciprocal) {
        return reciprocalTickCount;
    }
    return static_cast<int>(std::lround((range.maximum - range.minimum) / step));
}

int tickOf(double value, const SettingRange& range, double step, SliderScale scale) {
    const int count = tickCount(range, step, scale);
    if (scale == SliderScale::Reciprocal) {
        const double clamped = std::clamp(value, range.minimum, range.maximum);
        return static_cast<int>(
            std::clamp<long>(std::lround(reciprocalFraction(clamped, range) * count), 0, count));
    }
    const long tick = std::lround((value - range.minimum) / step);
    return static_cast<int>(std::clamp<long>(tick, 0, count));
}

double valueOfTick(int tick, const SettingRange& range, double step, SliderScale scale) {
    if (scale == SliderScale::Reciprocal) {
        const double fraction =
            std::clamp(static_cast<double>(tick) / tickCount(range, step, scale), 0.0, 1.0);
        const double low = 1.0 / range.minimum;
        const double high = 1.0 / range.maximum;
        const double value = 1.0 / (low - fraction * (low - high));
        return std::clamp(std::round(value / step) * step, range.minimum, range.maximum);
    }
    return std::clamp(range.minimum + tick * step, range.minimum, range.maximum);
}

double defaultValueOf(const FieldDescriptor& descriptor) {
    const DevelopSettings defaults{};
    return visitField(descriptor, defaults, [&](const auto& field) -> double {
        using Field = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<Field, float> || std::is_same_v<Field, double>) {
            return static_cast<double>(field);
        } else {
            throw std::invalid_argument("setting '" + std::string(descriptor.key) +
                                        "' is not a number");
        }
    });
}

} // namespace arraw::app
