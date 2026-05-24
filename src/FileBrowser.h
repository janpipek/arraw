#pragma once
#include <QWidget>
#include <QStringList>

class QListWidget;

class FileBrowser : public QWidget {
    Q_OBJECT
public:
    explicit FileBrowser(QWidget* parent = nullptr);

    void setDirectory(const QString& dir);
    void setCurrentFile(const QString& path);

    // Navigate ±1 from current selection. Returns false if already at boundary.
    bool navigateBy(int delta);

signals:
    void fileSelected(const QString& path);

private:
    QListWidget* list;
    QString      currentDir;
    QStringList  files;   // absolute paths, same order as list widget

    static QStringList scanRawFiles(const QString& dir);
};
