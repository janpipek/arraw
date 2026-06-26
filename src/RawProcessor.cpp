#include "RawProcessor.h"
#include "ColorManagement.h"
#include "ImageMetadata.h"
#include "LensfunSource.h"
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

static unsigned sensorClipThreshold(const LibRaw& raw, int channel) {
    const auto& color = raw.imgdata.color;
    if (channel >= 0 && channel < 4 && color.linear_max[channel] > 0)
        return color.linear_max[channel];
    if (color.maximum > 0)
        return color.maximum;
    if (color.data_maximum > 0)
        return color.data_maximum;
    return 65535;
}

static void markSensorClipPixel(ImageBuffer& mask, int x, int y, int channel) {
    if (channel < 0 || channel > 3)
        return;
    const int outChannel = std::min(channel, 2); // LibRaw uses channel 3 for the second green.
    mask.data[(size_t(y) * size_t(mask.width) + size_t(x)) * 3u + size_t(outChannel)] = 1.0f;
}

static ImageBuffer sensorClipMask(LibRaw& raw, int width, int height) {
    if (width <= 0 || height <= 0)
        return {};

    ImageBuffer mask;
    mask.width = width;
    mask.height = height;
    mask.data.assign(size_t(width) * size_t(height) * 3u, 0.0f);

    const auto& sizes = raw.imgdata.sizes;
    const auto& rawdata = raw.imgdata.rawdata;
    if (rawdata.raw_image && sizes.raw_width > 0 && sizes.raw_height > 0) {
        const int left = sizes.left_margin;
        const int top = sizes.top_margin;
        for (int y = 0; y < height; ++y) {
            const int rawY = y + top;
            if (rawY < 0 || rawY >= sizes.raw_height)
                continue;
            for (int x = 0; x < width; ++x) {
                const int rawX = x + left;
                if (rawX < 0 || rawX >= sizes.raw_width)
                    continue;
                const int channel = raw.COLOR(rawY, rawX);
                const ushort value
                    = rawdata.raw_image[size_t(rawY) * sizes.raw_width + size_t(rawX)];
                if (value >= sensorClipThreshold(raw, channel))
                    markSensorClipPixel(mask, x, y, channel);
            }
        }
        return mask;
    }

    if (rawdata.color3_image) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto& px = rawdata.color3_image[size_t(y) * size_t(width) + size_t(x)];
                for (int c = 0; c < 3; ++c)
                    if (px[c] >= sensorClipThreshold(raw, c))
                        markSensorClipPixel(mask, x, y, c);
            }
        }
        return mask;
    }

    if (rawdata.color4_image) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto& px = rawdata.color4_image[size_t(y) * size_t(width) + size_t(x)];
                for (int c = 0; c < 4; ++c)
                    if (px[c] >= sensorClipThreshold(raw, c))
                        markSensorClipPixel(mask, x, y, c);
            }
        }
        return mask;
    }

    return {};
}

static LoadResult rawError(const QString& message) {
    LoadResult result;
    result.error = message;
    return result;
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
        return rawError(QString("open_file: %1").arg(libraw_strerror(ret)));
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
        return rawError(QString("unpack: %1").arg(libraw_strerror(ret)));
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
        return rawError(QString("dcraw_process: %1").arg(libraw_strerror(ret)));
    timer.lap("raw dcraw_process");

    libraw_processed_image_t* img = raw->dcraw_make_mem_image(&ret);
    if (!img || ret != LIBRAW_SUCCESS)
        return rawError(QString("dcraw_make_mem_image: %1").arg(libraw_strerror(ret)));

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

    ImageBuffer sensorClipFullRes = sensorClipMask(*raw, w, h);
    LibRaw::dcraw_clear_mem(img);
    timer.lap("raw make+convert");

    const QRectF defaultCrop = defaultCropRect(*raw, fullRes.width, fullRes.height);
    // What the camera intended (its flip code), used to seed the Orientation edit.
    const orient::Orientation seeded = orient::fromLibrawFlip(raw->imgdata.sizes.flip);
    normalizeExposure(fullRes);
    timer.lap("raw normalize");
    ImageBuffer preview = downsample2x(fullRes);
    ImageBuffer sensorClipPreview = downsample2x(sensorClipFullRes);
    timer.lap("raw downsample");
    // Resolve a lens profile from EXIF (docs/adr/0027). Off the main thread; the
    // correction itself is applied later, toggle-gated, in DevelopSession. An empty
    // db path lets resolveLensfunModel pick a bundled DB (AppImage/Windows) and else
    // lensfun's system database; no match leaves the model empty.
    LensCorrectionModel lensModel;
    {
        const auto& id = raw->imgdata.idata;
        const auto& other = raw->imgdata.other;
        LensQuery query;
        query.cameraMaker = QString::fromUtf8(id.make);
        query.cameraModel = QString::fromUtf8(id.model);
        query.lensModel = QString::fromUtf8(raw->imgdata.lens.Lens);
        query.focal = other.focal_len;
        query.aperture = other.aperture;
        query.width = fullRes.width;
        query.height = fullRes.height;
        if (auto resolved = resolveLensfunModel(QString(), query))
            lensModel = std::move(*resolved);
        timer.lap("lens profile resolve");
    }

    return {
        std::move(fullRes),
        std::move(preview),
        std::move(sensorClipFullRes),
        std::move(sensorClipPreview),
        metadata,
        {},
        defaultCrop,
        std::move(lensModel),
        seeded};
}
