#include "FileBrowser.h"
#include <QListWidget>
#include <QVBoxLayout>
#include <QDir>
#include <QFileInfo>

static const QStringList kRawExts = {
    "cr2","cr3","nef","arw","dng","raf","orf","rw2","pef","srw"
};

FileBrowser::FileBrowser(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    list = new QListWidget(this);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(list);

    connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < files.size())
            emit fileSelected(files[row]);
    });
}

void FileBrowser::setDirectory(const QString& dir) {
    if (dir == currentDir) return;
    currentDir = dir;
    files = scanRawFiles(dir);

    list->blockSignals(true);
    list->clear();
    for (const auto& f : files)
        list->addItem(QFileInfo(f).fileName());
    list->blockSignals(false);
}

void FileBrowser::setCurrentFile(const QString& path) {
    int idx = files.indexOf(path);
    if (idx >= 0) {
        list->blockSignals(true);
        list->setCurrentRow(idx);
        list->blockSignals(false);
    }
}

void FileBrowser::selectFirst() {
    if (!files.isEmpty())
        list->setCurrentRow(0);  // triggers currentRowChanged → fileSelected
}

bool FileBrowser::navigateBy(int delta) {
    int current = list->currentRow();
    int next    = current + delta;
    if (next < 0 || next >= files.size()) return false;
    list->setCurrentRow(next);   // triggers currentRowChanged → fileSelected
    return true;
}

QStringList FileBrowser::scanRawFiles(const QString& dir) {
    QDir d(dir);
    QStringList filters;
    for (const auto& ext : kRawExts) {
        filters << "*." + ext << "*." + ext.toUpper();
    }
    QStringList names = d.entryList(filters, QDir::Files, QDir::Name);
    QStringList result;
    result.reserve(names.size());
    for (const auto& name : names)
        result << d.filePath(name);
    return result;
}
