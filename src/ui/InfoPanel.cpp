#include "core/ImageMetadata.h"
#include "develop/UserMetadata.h"
#include "ui/InfoPanel.h"

#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

void clearForm(QFormLayout* form) {
    while (form->rowCount() > 0)
        form->removeRow(0);
}

} // namespace

InfoPanel::InfoPanel(QWidget* parent)
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

    content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);

    contentLayout->addWidget(sectionLabel("User Metadata", content));
    userForm = new QFormLayout;
    userForm->setContentsMargins(0, 0, 0, 0);
    userForm->setSpacing(6);
    userForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    contentLayout->addLayout(userForm);

    titleEdit = new QLineEdit(content);
    titleEdit->setObjectName("titleEdit");
    captionEdit = new QPlainTextEdit(content);
    captionEdit->setObjectName("captionEdit");
    captionEdit->setTabChangesFocus(true);
    captionEdit->setFixedHeight(72);
    keywordsEdit = new QLineEdit(content);
    keywordsEdit->setObjectName("keywordsEdit");
    creatorEdit = new QLineEdit(content);
    creatorEdit->setObjectName("creatorEdit");
    copyrightEdit = new QLineEdit(content);
    copyrightEdit->setObjectName("copyrightEdit");

    userForm->addRow("Title", titleEdit);
    userForm->addRow("Caption", captionEdit);
    userForm->addRow("Keywords", keywordsEdit);
    userForm->addRow("Creator", creatorEdit);
    userForm->addRow("Copyright", copyrightEdit);

    contentLayout->addWidget(sectionLabel("EXIF", content));
    exifForm = new QFormLayout;
    exifForm->setContentsMargins(0, 0, 0, 0);
    exifForm->setSpacing(6);
    exifForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    contentLayout->addLayout(exifForm);
    contentLayout->addStretch(1);

    scroll->setWidget(content);
    scroll->hide();
    root->addWidget(scroll, 1);

    const auto commit = [this] { flushPendingEdits(); };
    connect(titleEdit, &QLineEdit::editingFinished, this, commit);
    connect(keywordsEdit, &QLineEdit::editingFinished, this, commit);
    connect(creatorEdit, &QLineEdit::editingFinished, this, commit);
    connect(copyrightEdit, &QLineEdit::editingFinished, this, commit);
    captionEdit->installEventFilter(this);
}

void InfoPanel::setUserMetadata(const UserMetadata& metadata) {
    currentMetadata = metadata;
    hasImage = true;
    syncEditors();
    updateVisibility();
}

void InfoPanel::setImageMetadata(const ImageMetadata& metadata) {
    clearForm(exifForm);
    for (const auto& [label, value] : metadata.rows) {
        if (value.isEmpty())
            continue;
        auto* name = new QLabel(label, content);
        name->setAlignment(Qt::AlignTop);
        auto* val = new QLabel(value, content);
        val->setWordWrap(true);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        exifForm->addRow(name, val);
    }
}

void InfoPanel::flushPendingEdits() {
    if (!hasImage || syncing)
        return;

    UserMetadata next = currentMetadata;
    next.title = titleEdit->text().trimmed();
    next.caption = captionEdit->toPlainText().trimmed();
    next.keywords = keywordsFromEditor();
    next.creator = creatorEdit->text().trimmed();
    next.copyright = copyrightEdit->text().trimmed();

    if (next == currentMetadata)
        return;
    const UserMetadataPresence changedFields{
        next.title != currentMetadata.title,
        next.caption != currentMetadata.caption,
        next.keywords != currentMetadata.keywords,
        next.creator != currentMetadata.creator,
        next.copyright != currentMetadata.copyright,
    };
    currentMetadata = next;
    emit userMetadataCommitted(currentMetadata, changedFields);
}

void InfoPanel::clear() {
    hasImage = false;
    currentMetadata = {};
    syncEditors();
    clearForm(exifForm);
    updateVisibility();
}

bool InfoPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == captionEdit && event->type() == QEvent::FocusOut)
        flushPendingEdits();
    return QWidget::eventFilter(watched, event);
}

void InfoPanel::syncEditors() {
    syncing = true;
    const QSignalBlocker b1(titleEdit);
    const QSignalBlocker b2(captionEdit);
    const QSignalBlocker b3(keywordsEdit);
    const QSignalBlocker b4(creatorEdit);
    const QSignalBlocker b5(copyrightEdit);
    titleEdit->setText(currentMetadata.title);
    captionEdit->setPlainText(currentMetadata.caption);
    keywordsEdit->setText(keywordsText(currentMetadata.keywords));
    creatorEdit->setText(currentMetadata.creator);
    copyrightEdit->setText(currentMetadata.copyright);
    syncing = false;
}

void InfoPanel::updateVisibility() {
    placeholder->setVisible(!hasImage);
    scroll->setVisible(hasImage);
}

QStringList InfoPanel::keywordsFromEditor() const {
    QStringList keywords;
    const QStringList parts = keywordsEdit->text().split(',', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty())
            keywords.append(trimmed);
    }
    return keywords;
}

QString InfoPanel::keywordsText(const QStringList& keywords) const {
    return keywords.join(", ");
}
