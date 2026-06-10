#include "RawProcessor.h"
#include "ColorManagement.h"
#include "ImageMetadata.h"
#include <libraw/libraw.h>
#include <QImage>
#include <QRectF>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

QImage RawProcessor::extractThumbImage(LibRaw& raw) {
    if (raw.unpack_thumb() != LIBRAW_SUCCESS)
        return {};

    int err = 0;
    libraw_processed_image_t* thumb = raw.dcraw_make_mem_thumb(&err);
    if (!thumb || err != LIBRAW_SUCCESS)
        return {};

    QImage img;
    if (thumb->type == LIBRAW_IMAGE_JPEG) {
        img.loadFromData(reinterpret_cast<const uchar*>(thumb->data),
                         int(thumb->data_size), "JPEG");
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

// Embedded thumbnail as a working-space ImageBuffer. Does not require
// unpack() to have been called first.
static ImageBuffer extractThumb(LibRaw& raw) {
    return toWorkingSpaceBuffer(RawProcessor::extractThumbImage(raw));
}

ImageBuffer RawProcessor::loadEmbeddedPreview(const QString& path) {
    auto raw = std::make_unique<LibRaw>();
    if (raw->open_file(path.toLocal8Bit().constData()) != LIBRAW_SUCCESS)
        return {};
    return extractThumb(*raw);
}

// With no_auto_bright and linear gamma, libraw output brightness varies a lot
// between shots (often very dark). Scale the image so its near-maximum luma
// (99.5th percentile, ignoring outliers like specular highlights) lands at a
// fixed target, giving every photo a comparable starting exposure. The gain is
// clamped so a failed estimate can't blow the image out or crush it.
static void normalizeRawExposure(ImageBuffer& buf) {
    if (!buf.valid())
        return;

    std::vector<float> luma;
    luma.reserve(size_t(buf.width * buf.height / 16 + 1));

    constexpr int kStride = 16;

    const int pixels = buf.width * buf.height;
    for (int i = 0; i < pixels; i += kStride) {
        const float* p = buf.data.data() + i * 3;
        const float y = p[0] * kLumaR + p[1] * kLumaG + p[2] * kLumaB;
        if (std::isfinite(y) && y > 0.0f)
            luma.push_back(y);
    }

    if (luma.empty())
        return;

    const size_t idx = std::min(luma.size() - 1, size_t(luma.size() * 0.995f));
    std::nth_element(luma.begin(), luma.begin() + idx, luma.end());

    const float highlight = luma[idx];
    if (highlight <= 0.0f)
        return;

    constexpr float kTargetHighlight = 0.78f;
    const float gain = std::clamp(kTargetHighlight / highlight, 0.5f, 4.0f);
    for (float& v : buf.data)
        v = std::max(0.0f, v * gain);
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

    return {double(x) / double(imageWidth),
            double(y) / double(imageHeight),
            double(w) / double(imageWidth),
            double(h) / double(imageHeight)};
}

LoadResult RawProcessor::load(const QString& path,
                               std::function<void(ImageBuffer)> onEmbeddedPreview,
                               std::shared_ptr<std::atomic<bool>> cancel) {
    auto cancelled = [&]{ return cancel && cancel->load(); };
    auto raw = std::make_unique<LibRaw>();

    int ret = raw->open_file(path.toLocal8Bit().constData());
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("open_file: %1").arg(libraw_strerror(ret))};

    // Extract embedded preview on the same open handle, before the slow unpack.
    if (onEmbeddedPreview) {
        ImageBuffer buf = extractThumb(*raw);
        if (buf.valid())
            onEmbeddedPreview(std::move(buf));
    }

    if (cancelled()) return {};

    ret = raw->unpack();
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("unpack: %1").arg(libraw_strerror(ret))};

    if (cancelled()) return {};

    const ImageMetadata metadata = extractMetadata(*raw);

    raw->imgdata.params.use_camera_wb   = 1;
    raw->imgdata.params.no_auto_bright  = 1;
    raw->imgdata.params.output_bps      = 16;
    raw->imgdata.params.output_color    = 8;   // Rec.2020 working space (needs libraw ≥ 0.21)
    raw->imgdata.params.gamm[0]         = 1.0;  // linear gamma
    raw->imgdata.params.gamm[1]         = 1.0;
    raw->imgdata.params.bright          = 1.0;

    ret = raw->dcraw_process();
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("dcraw_process: %1").arg(libraw_strerror(ret))};

    libraw_processed_image_t* img = raw->dcraw_make_mem_image(&ret);
    if (!img || ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("dcraw_make_mem_image: %1").arg(libraw_strerror(ret))};

    const int w = img->width;
    const int h = img->height;
    const float scale = 1.0f / 65535.0f;

    ImageBuffer fullRes;
    fullRes.width  = w;
    fullRes.height = h;
    fullRes.data.resize(w * h * 3);

    const uint16_t* src = reinterpret_cast<const uint16_t*>(img->data);
    for (int i = 0; i < w * h * 3; ++i)
        fullRes.data[i] = src[i] * scale;

    LibRaw::dcraw_clear_mem(img);

    const QRectF defaultCrop = defaultCropRect(*raw, fullRes.width, fullRes.height);
    normalizeRawExposure(fullRes);
    ImageBuffer preview = downsample2x(fullRes);
    return {std::move(fullRes), std::move(preview), metadata, {}, defaultCrop};
}
