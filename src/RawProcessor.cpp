#include "RawProcessor.h"
#include "ImageMetadata.h"
#include <libraw/libraw.h>
#include <QImage>

// Extract the embedded JPEG/bitmap thumbnail from an already-open LibRaw
// instance and return it as a linear-light ImageBuffer.  Does not require
// unpack() to have been called first.
static ImageBuffer extractThumb(LibRaw& raw) {
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
    return srgbToLinearBuffer(img);
}

ImageBuffer RawProcessor::loadEmbeddedPreview(const QString& path) {
    LibRaw raw;
    if (raw.open_file(path.toLocal8Bit().constData()) != LIBRAW_SUCCESS)
        return {};
    return extractThumb(raw);
}

LoadResult RawProcessor::load(const QString& path,
                               std::function<void(ImageBuffer)> onEmbeddedPreview,
                               std::shared_ptr<std::atomic<bool>> cancel) {
    auto cancelled = [&]{ return cancel && cancel->load(); };
    LibRaw raw;

    int ret = raw.open_file(path.toLocal8Bit().constData());
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("open_file: %1").arg(libraw_strerror(ret))};

    // Extract embedded preview on the same open handle, before the slow unpack.
    if (onEmbeddedPreview) {
        ImageBuffer buf = extractThumb(raw);
        if (buf.valid())
            onEmbeddedPreview(std::move(buf));
    }

    if (cancelled()) return {};

    ret = raw.unpack();
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("unpack: %1").arg(libraw_strerror(ret))};

    if (cancelled()) return {};

    const ImageMetadata metadata = extractMetadata(raw);

    raw.imgdata.params.use_camera_wb   = 1;
    raw.imgdata.params.no_auto_bright  = 1;
    raw.imgdata.params.output_bps      = 16;
    raw.imgdata.params.gamm[0]         = 1.0;  // linear gamma
    raw.imgdata.params.gamm[1]         = 1.0;
    raw.imgdata.params.bright          = 1.0;

    ret = raw.dcraw_process();
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, {}, QString("dcraw_process: %1").arg(libraw_strerror(ret))};

    libraw_processed_image_t* img = raw.dcraw_make_mem_image(&ret);
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

    ImageBuffer preview = downsample2x(fullRes);
    return {std::move(fullRes), std::move(preview), metadata, {}};
}
