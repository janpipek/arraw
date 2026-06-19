#pragma once
#include "ImageMetadata.h"
#include <QWidget>

class QFormLayout;
class QScrollArea;
class QLabel;

/**
 * Read-only metadata panel for the active image.
 *
 * ExifPanel renders ImageMetadata as labels in the EXIF tab. It never writes
 * sidecars or culling marks; user-authored metadata belongs to UserMetadata and
 * is coordinated through DevelopSession, FilmStrip, and MainWindow.
 */
class ExifPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ExifPanel)
public:
    explicit ExifPanel(QWidget* parent = nullptr);

    void setMetadata(const ImageMetadata& meta);
    void clear();

private:
    QFormLayout* form;
    QScrollArea* scroll;
    QLabel* placeholder;
};
