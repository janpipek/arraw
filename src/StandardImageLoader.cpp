#include "StandardImageLoader.h"
#include "ColorManagement.h"
#include "Trace.h"
#include <QFileInfo>
#include <QImage>

bool StandardImageLoader::canLoad(const QString& path) {
    static const QStringList kExts = {"jpg", "jpeg", "png", "tiff", "tif", "webp", "bmp"};
    return kExts.contains(QFileInfo(path).suffix().toLower());
}

LoadResult StandardImageLoader::load(const QString& path, std::shared_ptr<std::atomic<bool>> cancel) {
    const trace::Scope trace_("StandardImageLoader::load");
    QImage img(path);
    if (img.isNull())
        return {{}, {}, {}, QString("Failed to load: %1").arg(path)};

    if (cancel && cancel->load())
        return {};

    ImageBuffer fullRes = toWorkingSpaceBuffer(img);
    if (!fullRes.valid())
        return {{}, {}, {}, QString("Failed to decode: %1").arg(path)};

    ImageBuffer preview = downsample2x(fullRes);
    return {std::move(fullRes), std::move(preview), {}, {}};
}
