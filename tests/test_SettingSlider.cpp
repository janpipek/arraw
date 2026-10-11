#include "MaskPresentation.h"
#include "SettingPresentation.h"
#include "ui/SettingSlider.h"

#include <SettingDescriptors.h>

#include <QColor>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSlider>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace arraw;
using namespace arraw::app;

/// The slider row's painted hue track.

namespace {

/// Gives the largest spread between the channels of any pixel on a slider's middle row.
int mostChroma(SettingSlider& row) {
    row.resize(360, 30);
    auto* slider = row.findChild<QSlider*>("slider");
    REQUIRE(slider != nullptr);
    const QImage image = slider->grab().toImage();
    int most = 0;
    const int y = image.height() / 2;
    for (int x = 0; x < image.width(); ++x) {
        const QColor colour = image.pixelColor(x, y);
        most = std::max(most, std::max({colour.red(), colour.green(), colour.blue()}) -
                                  std::min({colour.red(), colour.green(), colour.blue()}));
    }
    return most;
}

} // namespace

TEST_CASE("A grading hue row paints the hues behind its handle", "[app][slider]") {
    SettingSlider hue("gradeMidtoneHue");
    SettingSlider saturation("gradeMidtoneSaturation");
    CHECK(mostChroma(hue) > 60);
    CHECK(mostChroma(saturation) < 40);
}

TEST_CASE("A row made from a presentation takes its range, label and default", "[app][slider]") {
    const LocalDescriptor& exposure = *findLocalDescriptor("exposure");
    SettingSlider row("local.exposure", exposure.range, 0.0, localPresentationOf("exposure"));
    CHECK(row.key() == "local.exposure");
    CHECK(row.defaultValue() == 0.0);
    CHECK(row.findChild<QLabel*>()->text() == "Exposure");
    auto* box = row.findChild<QDoubleSpinBox*>();
    REQUIRE(box != nullptr);
    CHECK(box->minimum() == -4.0);
    CHECK(box->maximum() == 4.0);
    CHECK(box->decimals() == 2);
    CHECK(box->suffix() == " EV");

    // A reset of a row that is not optional restores the default it was given.
    SettingSlider opacity("local.opacity", maskOpacityRange, 100.0, maskOpacityPresentation());
    opacity.setValue(40.0);
    CHECK(opacity.defaultValue() == 100.0);
}

TEST_CASE("The key constructor still names the row by the setting's key", "[app][slider]") {
    SettingSlider row("exposure");
    CHECK(row.key() == "exposure");
    SettingSlider temperature("temperature");
    CHECK(temperature.key() == "temperature");
}

TEST_CASE("A logarithmic row reaches both ends of its range and rises between", "[app][slider]") {
    SettingSlider row("brush.size", brushSizeRange(), 2.0, brushSizePresentation());
    auto* slider = row.findChild<QSlider*>("slider");
    auto* box = row.findChild<QDoubleSpinBox*>();
    REQUIRE(slider != nullptr);
    REQUIRE(box != nullptr);
    CHECK(slider->minimum() == 0);
    CHECK(slider->maximum() == logarithmicTickCount);
    CHECK(box->value() == 2.0);

    slider->setValue(slider->minimum());
    CHECK(box->value() == 0.05);
    slider->setValue(slider->maximum());
    CHECK(box->value() == 100.0);

    double previous = 0.0;
    for (int tick = slider->minimum(); tick <= slider->maximum(); tick += 10) {
        slider->setValue(tick);
        CHECK(box->value() >= previous);
        previous = box->value();
    }
    row.setValue(0.05);
    CHECK(slider->value() == 0);
    row.setValue(100.0);
    CHECK(slider->value() == slider->maximum());
}
