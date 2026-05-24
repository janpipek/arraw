#pragma once
#include <QWidget>
#include <QStringList>

class QListWidget;
class ThumbnailCache;
class QLineEdit;

class FileBrowser : public QWidget {
    Q_OBJECT
public:
    explicit FileBrowser(QWidget* parent = nullptr);

    void setDirectory(const QString& dir);
    void setCurrentFile(const QString& path);
    void selectFirst();

    // Navigate ±1 from current selection. Returns false if already at boundary.
    bool navigateBy(int delta);

    QString directory() const { return currentDir; }

signals:
    void fileSelected(const QString& path);

private slots:
    void onThumbnailReady(const QString& path, const QImage& image);

private:
    void requestVisibleThumbnails();

    QLineEdit*       pathEdit;
    QListWidget*     list;
    ThumbnailCache*  thumbs;
    QString          currentDir;
    QStringList      files;   // absolute paths, same order as list widget

    static QStringList scanRawFiles(const QString& dir);
};
