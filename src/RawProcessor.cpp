#include "RawProcessor.h"
#include <libraw/libraw.h>

LoadResult RawProcessor::load(const QString& path) {
    LibRaw raw;

    int ret = raw.open_file(path.toLocal8Bit().constData());
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, QString("open_file: %1").arg(libraw_strerror(ret))};

    ret = raw.unpack();
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, QString("unpack: %1").arg(libraw_strerror(ret))};

    raw.imgdata.params.use_camera_wb   = 1;
    raw.imgdata.params.no_auto_bright  = 1;
    raw.imgdata.params.output_bps      = 16;
    raw.imgdata.params.gamm[0]         = 1.0;  // linear gamma
    raw.imgdata.params.gamm[1]         = 1.0;
    raw.imgdata.params.bright          = 1.0;

    ret = raw.dcraw_process();
    if (ret != LIBRAW_SUCCESS)
        return {{}, {}, QString("dcraw_process: %1").arg(libraw_strerror(ret))};

    libraw_processed_image_t* img = raw.dcraw_make_mem_image(&ret);
    if (!img || ret != LIBRAW_SUCCESS)
        return {{}, {}, QString("dcraw_make_mem_image: %1").arg(libraw_strerror(ret))};

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
    return {std::move(fullRes), std::move(preview), {}};
}
