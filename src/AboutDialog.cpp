#include "AboutDialog.h"
#include "ThemeColors.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("About arraw"));

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setSizeConstraint(QLayout::SetFixedSize);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(20);

    // ── Left Side: App Icon ──────────────────────────────────────────────────
    auto* iconLabel = new QLabel(this);
    QPixmap iconPixmap(":/icons/arraw-128.png");
    if (!iconPixmap.isNull()) {
        iconLabel->setPixmap(iconPixmap);
    } else {
        // Fallback to app icon if the specific resource fails
        iconLabel->setPixmap(qApp->windowIcon().pixmap(128, 128));
    }
    iconLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    mainLayout->addWidget(iconLabel);

    // ── Right Side: Text & Info ──────────────────────────────────────────────
    auto* rightLayout = new QVBoxLayout();
    rightLayout->setSpacing(10);

    // App name & version
    auto* titleLabel = new QLabel("arraw", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(22);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    auto* versionLabel = new QLabel(tr("Version %1").arg(qApp->applicationVersion()), this);
    QFont versionFont = versionLabel->font();
    versionFont.setPointSize(10);
    versionLabel->setFont(versionFont);

    auto* descLabel = new QLabel(
        tr("A lightweight, cross-platform RAW photo editor with a Lightroom-style "
           "development workflow. Features real-time GPU previews, non-destructive edits, "
           "and Lightroom-compatible XMP sidecar support."),
        this);
    descLabel->setWordWrap(true);

    rightLayout->addWidget(titleLabel);
    rightLayout->addWidget(versionLabel);
    rightLayout->addWidget(descLabel);
    rightLayout->addSpacing(5);

    // Metadata Grid
    auto* grid = new QGridLayout();
    grid->setSpacing(6);
    grid->setColumnMinimumWidth(0, 80);

    int row = 0;
    auto addRow = [&](const QString& labelText, const QString& valueText, bool isHtml = false) {
        auto* label = new QLabel(labelText + ":", this);
        QFont f = label->font();
        f.setBold(true);
        label->setFont(f);

        auto* value = new QLabel(valueText, this);
        if (isHtml) {
            value->setTextFormat(Qt::RichText);
            value->setOpenExternalLinks(true);
        }
        grid->addWidget(label, row, 0);
        grid->addWidget(value, row, 1);
        row++;
    };

    addRow(
        tr("Website"),
        QStringLiteral(
            "<a href=\"https://github.com/janpipek/arraw\">github.com/janpipek/arraw</a>"),
        true);
    addRow(tr("License"), tr("GNU GPL v3"));
    addRow(
        tr("Libraries"),
        tr("Qt %1, LibRaw, Little CMS 2, Lensfun").arg(QString::fromLatin1(qVersion())));

    rightLayout->addLayout(grid);
    rightLayout->addSpacing(10);

    // Buttons
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    rightLayout->addWidget(buttons);

    mainLayout->addLayout(rightLayout);
}
