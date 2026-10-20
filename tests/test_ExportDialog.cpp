#include "ui/ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>

#include <catch2/catch_test_macros.hpp>

using namespace arraw;
using namespace arraw::app;

namespace {

constexpr ImageSize frame{6000, 4000};

/// @brief Finds the size box: the one whose first item is the original size.
QComboBox* sizeBoxOf(ExportDialog& dialog) {
    for (auto* box : dialog.findChildren<QComboBox*>()) {
        if (box->itemText(0) == QObject::tr("Original size")) {
            return box;
        }
    }
    return nullptr;
}

/// @brief Finds the allow-enlarging check.
QCheckBox* enlargeOf(ExportDialog& dialog) {
    for (auto* check : dialog.findChildren<QCheckBox*>()) {
        if (check->text() == QObject::tr("Allow enlarging")) {
            return check;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("The export dialog offers the original size and the long edges",
          "[app][dialog][export]") {
    ExportDialog dialog(ExportSettings{}, frame);
    auto* box = sizeBoxOf(dialog);
    REQUIRE(box != nullptr);
    REQUIRE(box->count() == 6);
    CHECK(box->currentIndex() == 0);
    CHECK_FALSE(dialog.settings().resize);
    CHECK_FALSE(enlargeOf(dialog)->isEnabled());
}

TEST_CASE("Each long edge gives a square box of that edge", "[app][dialog][export]") {
    ExportDialog dialog(ExportSettings{}, frame);
    auto* box = sizeBoxOf(dialog);
    const std::uint32_t expected[]{1080, 1600, 2048, 3840};
    for (int i = 0; i < 4; ++i) {
        box->setCurrentIndex(i + 1);
        const ExportSettings settings = dialog.settings();
        CHECK(settings.resize);
        CHECK(settings.width == expected[i]);
        CHECK(settings.height == expected[i]);
        CHECK(enlargeOf(dialog)->isEnabled());
    }
    box->setCurrentIndex(0);
    CHECK_FALSE(dialog.settings().resize);
    CHECK_FALSE(enlargeOf(dialog)->isEnabled());
}

TEST_CASE("The custom size starts at the frame and takes the boxes", "[app][dialog][export]") {
    ExportDialog dialog(ExportSettings{}, frame);
    auto* box = sizeBoxOf(dialog);
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    for (auto* spin : dialog.findChildren<QSpinBox*>()) {
        if (spin->suffix() == QObject::tr(" px")) {
            (width == nullptr ? width : height) = spin;
        }
    }
    REQUIRE(height != nullptr);
    CHECK_FALSE(width->isEnabled());
    box->setCurrentIndex(5);
    CHECK(width->isEnabled());
    CHECK(height->isEnabled());
    CHECK(enlargeOf(dialog)->isEnabled());
    ExportSettings settings = dialog.settings();
    CHECK(settings.resize);
    CHECK(settings.width == 6000);
    CHECK(settings.height == 4000);
    width->setValue(800);
    height->setValue(600);
    settings = dialog.settings();
    CHECK(settings.width == 800);
    CHECK(settings.height == 600);
}

TEST_CASE("The dialog starts at the stored size choice", "[app][dialog][export]") {
    {
        ExportDialog dialog(ExportSettings{.resize = true, .width = 1600, .height = 1600}, frame);
        CHECK(sizeBoxOf(dialog)->currentIndex() == 2);
        CHECK(dialog.settings().width == 1600);
    }
    {
        ExportDialog dialog(ExportSettings{.resize = true, .width = 700, .height = 500}, frame);
        CHECK(sizeBoxOf(dialog)->currentIndex() == 5);
        CHECK(dialog.settings().width == 700);
        CHECK(dialog.settings().height == 500);
    }
}
