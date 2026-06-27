#include "pipeline/StandardImageLoader.h"
#include "pipeline/ColorManagement.h"
#include "core/Trace.h"
#include <QFileInfo>
#include <QImage>
#include <QImageReader>

bool StandardImageLoader::canLoad(const QString& path) {
    static const QStringList kExts = {"jpg", "jpeg", "png", "tiff", "tif", "webp", "bmp"};
    return kExts.contains(QFileInfo(path).suffix().toLower());
}

LoadResult StandardImageLoader::load(const QString& path, std::shared_ptr<std::atomic<bool>> cancel) {
    const trace::Scope trace_("StandardImageLoader::load");
    // Read the EXIF orientation but do NOT auto-apply it (autoTransform stays
    // off): the buffer stays native and Orientation is seeded as a develop edit,
    // matching the RAW path (docs/adr/0028).
    QImageReader reader(path);
    const orient::Orientation seeded = orient::fromQtTransformation(int(reader.transformation()));

    QImage img(path);
    if (img.isNull()) {
        LoadResult result;
        result.error = QString("Failed to load: %1").arg(path);
        return result;
    }

    if (cancel && cancel->load())
        return {};

    ImageBuffer fullRes = toWorkingSpaceBuffer(img);
    if (!fullRes.valid()) {
        LoadResult result;
        result.error = QString("Failed to decode: %1").arg(path);
        return result;
    }

    ImageBuffer preview = downsample2x(fullRes);
    return {
        std::move(fullRes),
        std::move(preview),
        {}, // sensorClipFullRes
        {}, // sensorClipPreview
        {}, // metadata
        {}, // embeddedMetadata
        {}, // embeddedMetadataPresence
        {}, // error
        {0.0, 0.0, 1.0, 1.0}, // defaultCrop
        {}, // lensModel
        seeded};
}
