#include "MainWindowStatus.h"
#include "core/ImageBuffer.h"

#include "io/XmpSidecar.h"

#include <QFileInfo>

QString sidecarWriteErrorText(const QString& path) {
    return "Could not write " + XmpSidecar::pathFor(path);
}

QString loadedImageStatusText(
    const QString& path, const ImageBuffer& fullRes, DevelopSession::SidecarState sidecarState) {
    QString status = QStringLiteral("%1  \u2014  %2 \u00d7 %3")
                         .arg(QFileInfo(path).fileName())
                         .arg(fullRes.width)
                         .arg(fullRes.height);
    if (sidecarState == DevelopSession::SidecarState::ParseError)
        status += QStringLiteral("  \u2014  Sidecar unreadable; defaults applied");
    return status;
}

QString batchExportStatusText(int exported, int total) {
    return QStringLiteral("Exported %1 of %2 file(s)").arg(exported).arg(total);
}
