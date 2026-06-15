#include "ExifPanel.h"
#include <QFormLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

ExifPanel::ExifPanel(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    placeholder = new QLabel("No image loaded", this);
    placeholder->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    placeholder->setWordWrap(true);
    root->addWidget(placeholder);

    scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(content);
    scroll->hide();
    root->addWidget(scroll, 1);
}

void ExifPanel::clear() {
    setMetadata({});
}

void ExifPanel::setMetadata(const ImageMetadata& meta) {
    while (form->rowCount() > 0)
        form->removeRow(0);

    if (meta.empty()) {
        placeholder->show();
        scroll->hide();
        return;
    }

    placeholder->hide();
    scroll->show();

    for (const auto& [label, value] : meta.rows) {
        if (value.isEmpty())
            continue;
        auto* name = new QLabel(label, this);
        name->setAlignment(Qt::AlignTop);
        auto* val = new QLabel(value, this);
        val->setWordWrap(true);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(name, val);
    }
}
