#include "RawProcessor.h"
#include "ColorManagement.h"
#include "ImageMetadata.h"
#include "Trace.h"
#include <algorithm>
#include <cmath>
#include <libraw/libraw.h>
#include <memory>
#include <vector>
#include <QImage>
#include <QRectF>

QImage RawProcessor::extractThumbImage(LibRaw& raw) {
    if (raw.unpack_thumb() != LIBRAW_SUCCESS)
        return {};

    int err = 0;
    libraw_processed_image_t* thumb = raw.dcraw_make_mem_thumb(&err);
    if (!thumb || err != LIBRAW_SUCCESS)
        return {};

    QImage img;
    if (thumb->type == LIBRAW_IMAGE_JPEG) {
        img.loadFromData(reinterpret_cast<const uchar*>(thumb->data), int(thumb->data_size), "JPEG");
    } else {
        img = QImage(thumb->width, thumb->height, QImage::Format_RGB888);
        if (!img.isNull()) {
            const int pixels = thumb->width * thumb->height * 3;
            if (thumb->bits == 8) {
                memcpy(img.bits(), thumb->data, size_t(pixels));
            } else if (thumb->bits == 16) {
                const auto* src16 = reinterpret_cast<const uint16_t*>(thumb->data);
                auto* dst = img.bits();
                for (int i = 0; i < pixels; ++i)
                    dst[i] = uchar(src16[i] >> 8);
            }
        }
    }
    LibRaw::dcraw_clear_mem(thumb);
    return img;
}

// Cameras often embed a full-resolution (20+ MP) JPEG preview. Converting that to
// working space with lcms costs several seconds — for an image that is replaced by
// the full demosaic moments later. Cap the preview to a display-sized edge before
// the colour transform; this is the single biggest win for perceived load time.
static constexpr int kPreviewMaxEdge = 2048;

// Embedded thumbnail as a working-space ImageBuffer. Does not require
// unpack() to have been called first.
static ImageBuffer extractThumb(LibRaw& raw) {
    QImage thumb = RawProcessor::extractThumbImage(raw);
    if (thumb.width() > kPreviewMaxEdge || thumb.height() > kPreviewMaxEdge)
        thumb = thumb.scaled(
            kPreviewMaxEdge, kPreviewMaxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ImageBuffer buf = toWorkingSpaceBuffer(thumb);
    // Match the demosaic's exposure target so the preview doesn't pop in brightness
    // when the full-res image replaces it.
    normalizeExposure(buf);
    return buf;
}

ImageBuffer RawProcessor::loadEmbeddedPreview(const QString& path) {
    auto raw = std::make_unique<LibRaw>();
    if (raw->open_file(path.toLocal8Bit().constData()) != LIBRAW_SUCCESS)
        return {};
    return extractThumb(*raw);
}

// DNG DefaultCrop tag as a normalised rect: {x, y, w, h} in pixels of the
// demosaiced image. Cameras that don't set it leave zeros, and some write
// values outside the frame — fall back to the full image in both cases.
static QRectF defaultCropRect(const LibRaw& raw, int imageWidth, int imageHeight) {
    const auto& crop = raw.imgdata.color.dng_levels.default_crop;
    const int x = int(crop[0]);
    const int y = int(crop[1]);
    const int w = int(crop[2]);
    const int h = int(crop[3]);

    if (imageWidth <= 0 || imageHeight <= 0 || w <= 0 || h <= 0)
        return {0.0, 0.0, 1.0, 1.0};
    if (x == 0 && y == 0 && w == imageWidth && h == imageHeight)
        return {0.0, 0.0, 1.0, 1.0};
    if (x < 0 || y < 0 || x + w > imageWidth || y + h > imageHeight)
        return {0.0, 0.0, 1.0, 1.0};

    return {
        double(x) / double(imageWidth),
        double(y) / double(imageHeight),
        double(w) / double(imageWidth),
        double(h) / double(imageHeight)};
}

LoadResult RawProcessor::load(
    const QString& path,
    std::function<void(ImageBuffer)> onEmbeddedPreview,
    std::shared_ptr<std::atomic<bool>> cancel) {
    auto cancelled = [&] { return cancel && cancel->load(); };
    auto raw = std::make_unique<LibRaw>();

    // Per-stage timing for diagnosing slow loads. Set ARRAW_TRACE to enable.
    trace::Laps timer;

    int ret = raw->open_file(path.toLocal8Bit().constData());
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("open_file: %1").arg(libraw_strerror(ret))};
    timer.lap("raw open_file");

    // Extract embedded preview on the same open handle, before the slow unpack.
    if (onEmbeddedPreview) {
        ImageBuffer buf = extractThumb(*raw);
        if (buf.valid())
            onEmbeddedPreview(std::move(buf));
        timer.lap("raw embedded_preview");
    }

    if (cancelled())
        return {};

    ret = raw->unpack();
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("unpack: %1").arg(libraw_strerror(ret))};
    timer.lap("raw unpack");

    if (cancelled())
        return {};

    const ImageMetadata metadata = extractMetadata(*raw);

    raw->imgdata.params.use_camera_wb = 1;
    raw->imgdata.params.no_auto_bright = 1;
    // Decode in the *native* sensor orientation; Orientation is a develop edit
    // applied downstream, seeded from the camera flag below (docs/adr/0028).
    raw->imgdata.params.user_flip = 0;
    raw->imgdata.params.output_bps = 16;
    raw->imgdata.params.output_color = 8; // Rec.2020 working space (needs libraw ≥ 0.21)
    raw->imgdata.params.gamm[0] = 1.0;    // linear gamma
    raw->imgdata.params.gamm[1] = 1.0;
    raw->imgdata.params.bright = 1.0;

    ret = raw->dcraw_process();
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("dcraw_process: %1").arg(libraw_strerror(ret))};
    timer.lap("raw dcraw_process");

    libraw_processed_image_t* img = raw->dcraw_make_mem_image(&ret);
    if (!img || ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("dcraw_make_mem_image: %1").arg(libraw_strerror(ret))};

    const int w = img->width;
    const int h = img->height;
    const float scale = 1.0f / 65535.0f;

    ImageBuffer fullRes;
    fullRes.width = w;
    fullRes.height = h;
    fullRes.data.resize(w * h * 3);

    const uint16_t* src = reinterpret_cast<const uint16_t*>(img->data);
    for (int i = 0; i < w * h * 3; ++i)
        fullRes.data[i] = src[i] * scale;

    LibRaw::dcraw_clear_mem(img);
    timer.lap("raw make+convert");

    const QRectF defaultCrop = defaultCropRect(*raw, fullRes.width, fullRes.height);
    // What the camera intended (its flip code), used to seed the Orientation edit.
    const orient::Orientation seeded = orient::fromLibrawFlip(raw->imgdata.sizes.flip);
    normalizeExposure(fullRes);
    timer.lap("raw normalize");
    ImageBuffer preview = downsample2x(fullRes);
    timer.lap("raw downsample");
    return {std::move(fullRes), std::move(preview), metadata, {}, defaultCrop, seeded};
}
