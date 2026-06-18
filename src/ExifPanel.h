#pragma once
#include "ImageMetadata.h"
#include <QWidget>

class QFormLayout;
class QScrollArea;
class QLabel;

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
