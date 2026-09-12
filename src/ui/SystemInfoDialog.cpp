#include "ui/SystemInfoDialog.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFont>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QLabel* sectionTitle(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    return label;
}

// One label:value grid for a section, appended to `layout`. Values are
// selectable so a single field can be copied without the "Copy to Clipboard"
// button.
void addSection(
    QVBoxLayout* layout,
    QWidget* parent,
    const QString& title,
    std::initializer_list<std::pair<QString, QString>> rows) {
    layout->addWidget(sectionTitle(title, parent));

    auto* grid = new QGridLayout();
    grid->setSpacing(6);
    grid->setColumnMinimumWidth(0, 110);
    int row = 0;
    for (const auto& [labelText, valueText] : rows) {
        auto* label = new QLabel(labelText + ":", parent);
        auto* value = new QLabel(valueText, parent);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(label, row, 0);
        grid->addWidget(value, row, 1);
        row++;
    }
    layout->addLayout(grid);
    layout->addSpacing(10);
}

} // namespace

SystemInfoDialog::SystemInfoDialog(const sysinfo::Info& info, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("System Info"));

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    layout->setContentsMargins(20, 20, 20, 20);

    addSection(
        layout,
        this,
        tr("Rendering"),
        {
            {tr("Backend"), info.gpuBackend},
            {tr("GPU"), tr("%1 (%2)").arg(info.gpuDeviceName, info.gpuDeviceType)},
            {tr("Device IDs"), info.gpuDeviceIds},
        });

    addSection(
        layout,
        this,
        tr("File Locations"),
        {
            {tr("Settings"), info.settingsPath},
            {tr("Presets"), info.presetsPath},
            {tr("Thumbnail cache"), info.cachePath},
        });

    addSection(
        layout,
        this,
        tr("System"),
        {
            {tr("App version"), info.appVersion},
            {tr("Qt version"), info.qtVersion},
            {tr("OS"), info.osName},
            {tr("CPU architecture"), info.cpuArchitecture},
            {tr("Build type"), info.buildType},
        });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* copyButton = buttons->addButton(tr("Copy to Clipboard"), QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, this, [info] {
        QGuiApplication::clipboard()->setText(sysinfo::toPlainText(info));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
