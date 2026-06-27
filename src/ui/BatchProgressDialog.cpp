#include "ui/BatchProgressDialog.h"

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

BatchProgressDialog::BatchProgressDialog(int total, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Exporting…"));
    setWindowModality(Qt::ApplicationModal);
    setFixedWidth(420);

    auto* layout = new QVBoxLayout(this);

    fileLabel = new QLabel(this);
    fileLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(fileLabel);

    bar = new QProgressBar(this);
    bar->setRange(0, total);
    bar->setValue(0);
    layout->addWidget(bar);

    auto* cancelBtn = new QPushButton(tr("Cancel"), this);
    layout->addWidget(cancelBtn, 0, Qt::AlignRight);

    connect(cancelBtn, &QPushButton::clicked, this, [this] {
        cancelled = true;
        emit cancelRequested();
        close();
    });
}

void BatchProgressDialog::setCurrentFile(const QString& filename) {
    fileLabel->setText(tr("Exporting %1…").arg(filename));
}

void BatchProgressDialog::setValue(int n) {
    bar->setValue(n);
}
