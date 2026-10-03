#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>

#include <exception>
#include <string>

namespace arraw::app {

SettingsDialog::SettingsDialog(const AppSettings& initial, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    resize(480, 240);

    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    layout->addWidget(tabs);
    auto* processing = new QWidget(tabs);
    tabs->addTab(processing, tr("Processing"));
    auto* form = new QFormLayout(processing);
    deviceBox_ = new QComboBox(processing);
    deviceBox_->setObjectName("processingDevice");
    deviceBox_->addItem(tr("Automatic (prefer GPU)"));
    choices_.push_back({});
    deviceBox_->addItem(tr("CPU only"));
    choices_.push_back({.cpuOnly = true, .gpu = std::nullopt});

    QString explanation = tr("Automatic uses the default hardware GPU when available. "
                             "If a GPU cannot be used, processing falls back to the CPU.");
    int selected = initial.cpuOnly ? 1 : 0;
    try {
        const auto adapters = listGpuAdapters(defaultGpuBackend());
        const auto preferred =
            initial.gpu ? findPreferredGpu(*initial.gpu, adapters) : std::nullopt;
        for (std::size_t index = 0; index < adapters.size(); ++index) {
            if (adapters[index].kind == GpuDeviceKind::Software) {
                continue;
            }
            deviceBox_->addItem(QString::fromStdString(adapters[index].name));
            choices_.push_back({.cpuOnly = false, .gpu = adapters[index]});
            if (!initial.cpuOnly && preferred == index) {
                selected = deviceBox_->count() - 1;
            }
        }
        if (deviceBox_->count() == 2) {
            explanation += tr("\nNo selectable hardware GPUs were detected. Automatic may still "
                              "use the system GPU on platforms that do not list adapters.");
        }
    } catch (const std::exception& error) {
        explanation += tr("\nGPU discovery failed: %1").arg(QString::fromUtf8(error.what()));
    }
    if (!initial.cpuOnly && initial.gpu && selected == 0) {
        deviceBox_->addItem(tr("%1 (unavailable)").arg(QString::fromStdString(initial.gpu->name)));
        choices_.push_back(initial);
        selected = deviceBox_->count() - 1;
    }
    deviceBox_->setCurrentIndex(selected);
    form->addRow(tr("Processing &device:"), deviceBox_);
    auto* description = new QLabel(explanation, processing);
    description->setWordWrap(true);
    form->addRow(description);
    auto* restart =
        new QLabel(tr("Applies to previews and exports after restarting Arraw."), processing);
    restart->setWordWrap(true);
    form->addRow(restart);
    if (qEnvironmentVariable("ARRAW_PREVIEW_DEVICE") == "cpu") {
        auto* override = new QLabel(tr("This session uses the CPU because ARRAW_PREVIEW_DEVICE=cpu "
                                       "is set. Your saved preference will still be remembered."),
                                    processing);
        override->setWordWrap(true);
        form->addRow(override);
    }

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::clicked, this, [this, buttons](QAbstractButton* button) {
        if (buttons->buttonRole(button) == QDialogButtonBox::ResetRole) {
            deviceBox_->setCurrentIndex(0);
        }
    });
}

AppSettings SettingsDialog::settings() const {
    return choices_.at(static_cast<std::size_t>(deviceBox_->currentIndex()));
}

} // namespace arraw::app
