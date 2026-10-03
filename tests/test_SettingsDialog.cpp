#include "ui/SettingsDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace arraw;

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    const QApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}

TEST_CASE("Settings dialog opens with automatic or CPU processing", "[app][settings][dialog]") {
    SECTION("Automatic") {
        app::SettingsDialog dialog({});
        const auto* device = dialog.findChild<QComboBox*>("processingDevice");
        REQUIRE(device);
        REQUIRE(device->currentIndex() == 0);
        REQUIRE_FALSE(dialog.settings().cpuOnly);
        REQUIRE_FALSE(dialog.settings().gpu);
    }
    SECTION("CPU") {
        app::SettingsDialog dialog({.cpuOnly = true, .gpu = std::nullopt});
        REQUIRE(dialog.settings().cpuOnly);
        REQUIRE_FALSE(dialog.settings().gpu);
    }
}

TEST_CASE("Missing GPU preferences survive opening and accepting the dialog",
          "[app][settings][dialog]") {
    const GpuAdapterInfo missing{
        .name = "Arraw test GPU that does not exist", .vendorId = 123, .deviceId = 456};
    app::SettingsDialog dialog({.cpuOnly = false, .gpu = missing});
    const auto* device = dialog.findChild<QComboBox*>("processingDevice");
    REQUIRE(device);
    REQUIRE(device->currentText().contains("unavailable"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(buttons);
    buttons->button(QDialogButtonBox::Ok)->click();
    REQUIRE(dialog.result() == QDialog::Accepted);
    REQUIRE(dialog.settings().gpu);
    REQUIRE(dialog.settings().gpu->name == missing.name);
    REQUIRE(dialog.settings().gpu->vendorId == missing.vendorId);
}

TEST_CASE("Restore defaults selects automatic and Cancel rejects the dialog",
          "[app][settings][dialog]") {
    app::SettingsDialog dialog({.cpuOnly = true, .gpu = std::nullopt});
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(buttons);
    buttons->button(QDialogButtonBox::RestoreDefaults)->click();
    REQUIRE_FALSE(dialog.settings().cpuOnly);
    REQUIRE_FALSE(dialog.settings().gpu);
    buttons->button(QDialogButtonBox::Cancel)->click();
    REQUIRE(dialog.result() == QDialog::Rejected);
}

TEST_CASE("An unavailable preferred GPU does not select a different device",
          "[app][settings][dialog]") {
    const GpuAdapterInfo missing{
        .name = "Arraw test GPU that does not exist", .vendorId = 123, .deviceId = 456};
    std::string reason;
    REQUIRE_FALSE(app::createAppGpuContext({.cpuOnly = false, .gpu = missing}, reason));
    REQUIRE_FALSE(reason.empty());
}
