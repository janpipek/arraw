#include "ui/SettingSlider.h"

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QSlider>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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
