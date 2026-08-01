#include "cli/ImagePathPreflight.h"
#include "pipeline/RawProcessor.h"
#include "pipeline/StandardImageLoader.h"
#include <QFileInfo>

namespace cli {

QString preflightImagePaths(const QStringList& paths) {
    for (const QString& path : paths) {
        const QFileInfo fi(path);
        if (!fi.exists())
            return QStringLiteral("no such file: %1").arg(path);
        if (fi.isDir())
            return QStringLiteral("is a directory: %1").arg(path);
        if (!StandardImageLoader::canLoad(path) && !RawProcessor::canLoad(path))
            return QStringLiteral("not a supported image: %1").arg(path);
    }
    return {};
}

} // namespace cli
