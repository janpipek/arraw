#pragma once
#include "ImageMetadata.h"
#include "UserMetadata.h"
#include <QWidget>

class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QScrollArea;

/**
 * Metadata panel for the active image.
 *
 * The top section edits User Metadata and commits it through MainWindow. The
 * lower section renders read-only EXIF rows from ImageMetadata.
 */
class InfoPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(InfoPanel)
public:
    explicit InfoPanel(QWidget* parent = nullptr);

    void setUserMetadata(const UserMetadata& metadata);
    void setImageMetadata(const ImageMetadata& metadata);

    UserMetadata userMetadata() const { return currentMetadata; }

    void flushPendingEdits();
    void clear();

signals:
    void userMetadataCommitted(
        const UserMetadata& metadata, const UserMetadataPresence& changedFields);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void syncEditors();
    void updateVisibility();
    QStringList keywordsFromEditor() const;
    QString keywordsText(const QStringList& keywords) const;

    QScrollArea* scroll;
    QWidget* content;
    QLabel* placeholder;
    QFormLayout* userForm;
    QFormLayout* exifForm;
    QLineEdit* titleEdit;
    QPlainTextEdit* captionEdit;
    QLineEdit* keywordsEdit;
    QLineEdit* creatorEdit;
    QLineEdit* copyrightEdit;
    UserMetadata currentMetadata;
    bool hasImage = false;
    bool syncing = false;
};
