#include "render/RendererCore.h"
#include "core/NoiseReduction.h"
#include "core/ThemeColors.h"
#include "develop/GlobalAdjustment.h"
#include "develop/LocalAdjustment.h"
#include "develop/WhiteBalance.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <variant>
#include <QFile>
#include <QMatrix4x4>

// Slider ±100 → shader uniform ±0.2 (gentler than dividing by 100 alone).
static constexpr float kToneSliderToUniform = 500.0f;

// Fullscreen quad, interleaved (x, y, u, v). V is flipped (bottom vertices get
// v=1) because NDC Y points up (GL convention, kept by QRhi) while image row 0
// is the top. Backend NDC differences are absorbed by clipCorr in image.vert.
static const float kQuad[] = {
    -1,
    -1,
    0,
    1,
    1,
    -1,
    1,
    1,
    -1,
    1,
    0,
    0,
    1,
    1,
    1,
    0,
};

// The viewport surround. Single-sourced from ThemeColors so the GPU clear color
// and the widget palette never drift (ADR 0031). Value is bit-identical to the
// historical 0.15 gray to keep the golden-image references valid (ADR 0005).
static const QColor kClearColor = ThemeColors::kCanvas;

static QShader loadShader(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

void RendererCore::initialize(QRhi* r) {
    if (rhi == r)
        return;
    if (rhi)
        release();
    rhi = r;

    vs = loadShader(QStringLiteral(":/shaders/image.vert.qsb"));
    fs = loadShader(QStringLiteral(":/shaders/image.frag.qsb"));
    spatialExtractFs = loadShader(QStringLiteral(":/shaders/spatial_extract.frag.qsb"));

    nrVs = loadShader(QStringLiteral(":/shaders/nr.vert.qsb"));
    nrExtractFs = loadShader(QStringLiteral(":/shaders/nr_extract.frag.qsb"));
    nrBlurHFs = loadShader(QStringLiteral(":/shaders/nr_blur_h.frag.qsb"));
    nrBlurVFs = loadShader(QStringLiteral(":/shaders/nr_blur_v.frag.qsb"));
    nrRecombineFs = loadShader(QStringLiteral(":/shaders/nr_recombine.frag.qsb"));

    vbuf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    vbuf->create();
    needQuadUpload = true;

    ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Ubuf)));
    ubuf->create();

    nrUbuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(NrUbuf)));
    nrUbuf->create();

    spatialUbuf.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(NrUbuf)));
    spatialUbuf->create();

    sampler.reset(rhi->newSampler(
        QRhiSampler::Linear,
        QRhiSampler::Linear,
        QRhiSampler::None,
        QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge));
    sampler->create();

    curveLutTex.reset(rhi->newTexture(QRhiTexture::RGBA32F, QSize(256, 1)));
    curveLutTex->create();
    ++generation;

    sensorClipDummyTex.reset(rhi->newTexture(QRhiTexture::RGBA32F, QSize(1, 1)));
    sensorClipDummyTex->create();
    sensorClipDummyDirty = true;
    ++generation;

    // A 1×1 single-layer R8 array bound whenever no brush mask exists (adr 0047).
    brushMaskDummyTex.reset(rhi->newTextureArray(QRhiTexture::R8, 1, QSize(1, 1)));
    brushMaskDummyTex->create();
    brushMaskDummyDirty = true;
    ++generation;

    toneLutTex.reset(rhi->newTexture(QRhiTexture::RGBA32F, QSize(tone::kLutSize, tone::kLutRows)));
    toneLutTex->create();
    toneLutSource.clear();
    ++generation;

    // The 3D sampler binding must always reference a valid texture, even with
    // useLut off — start with a 1×1×1 dummy.
    if (!displayLutDirty) {
        pendingDisplayLut = DisplayLut{{0.0f, 0.0f, 0.0f, 1.0f}, 1};
        displayLutDirty = true;
    }

    // Identity curve until the owner provides real LUTs.
    if (!curveLutDirty) {
        for (int i = 0; i < 256; ++i) {
            const float v = i / 255.0f;
            pendingCurveLut[i * 4 + 0] = v;
            pendingCurveLut[i * 4 + 1] = v;
            pendingCurveLut[i * 4 + 2] = v;
            pendingCurveLut[i * 4 + 3] = v;
        }
        curveLutDirty = true;
    }
}

void RendererCore::release() {
    pipelines.clear();
    srb.reset();
    srbImageTex = nullptr;
    srbSensorClipTex = nullptr;
    srbSpatialTex = nullptr;
    srbGeneration = -1;
    nrPipeExtract.reset();
    nrPipeBlurH.reset();
    nrPipeBlurV.reset();
    nrPipeRecombine.reset();
    nrSrbExtract.reset();
    nrSrbBlurH.reset();
    nrSrbBlurV.reset();
    nrSrbRecombine.reset();
    spatialPipeExtract.reset();
    spatialPipeBlurH.reset();
    spatialPipeBlurV.reset();
    spatialSrbExtract.reset();
    spatialSrbBlurH.reset();
    spatialSrbBlurV.reset();
    spatialSrbExtractTex = nullptr;
    spatialSrbBlurHTex = nullptr;
    spatialSrbBlurVTex = nullptr;
    nrUbuf.reset();
    spatialUbuf.reset();
    for (NrSlot& s : nrSlot)
        s = NrSlot{};
    for (SpatialSlot& s : spatialSlot)
        s = SpatialSlot{};
    // Destroy any in-flight readback targets before the QRhi goes away: this
    // frees each QRhiReadbackResult so a pending `completed` lambda can never
    // fire against a torn-down RendererCore (docs/adr/0035).
    readbackPool.clear();
    imageTex[0].reset();
    imageTex[1].reset();
    sensorClipTex[0].reset();
    sensorClipTex[1].reset();
    sensorClipDummyTex.reset();
    brushMaskArrayTex.reset();
    brushMaskDummyTex.reset();
    displayLutTex.reset();
    curveLutTex.reset();
    toneLutTex.reset();
    sampler.reset();
    ubuf.reset();
    vbuf.reset();
    rhi = nullptr;
}

// ── Pending data (safe to call before initialize) ────────────────────────────

QByteArray RendererCore::expandToRgba(const ImageBuffer& buf) {
    const qsizetype pixels = qsizetype(buf.width) * buf.height;
    QByteArray out(pixels * 4 * qsizetype(sizeof(float)), Qt::Uninitialized);
    const float* src = buf.data.data();
    float* dst = reinterpret_cast<float*>(out.data());
    for (qsizetype i = 0; i < pixels; ++i) {
        dst[i * 4 + 0] = src[i * 3 + 0];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 1.0f;
    }
    return out;
}

void RendererCore::setImage(Slot slot, const ImageBuffer& buf) {
    const int i = int(slot);
    if (!buf.valid()) {
        imageTex[i].reset();
        pendingImage[i] = {};
        pendingImageDirty[i] = false;
        return;
    }
    pendingImage[i] = {expandToRgba(buf), QSize(buf.width, buf.height)};
    pendingImageDirty[i] = true;
}

void RendererCore::setSensorClipMask(Slot slot, const ImageBuffer& buf) {
    const int i = int(slot);
    if (!buf.valid()) {
        sensorClipTex[i].reset();
        pendingSensorClip[i] = {};
        pendingSensorClipDirty[i] = false;
        ++generation;
        return;
    }
    pendingSensorClip[i] = {expandToRgba(buf), QSize(buf.width, buf.height)};
    pendingSensorClipDirty[i] = true;
}

bool RendererCore::hasImage(Slot slot) const {
    const int i = int(slot);
    return pendingImageDirty[i] || imageTex[i];
}

// The LUT depends only on the six Basic Tone fields of the global fill and each
// Local Adjustment — not on mask geometry or colour. Snapshot just those so the
// dirty-check is a cheap value compare and avoids copying mask variants.
static std::vector<std::array<float, 6>> toneSignature(const GlobalAdjustment& a) {
    auto fields = [](const SharedAdjustment& s) {
        return std::array<float, 6>{
            s.exposure, s.contrast, s.highlights, s.shadows, s.whites, s.blacks};
    };
    const int n = std::min<int>(int(a.localAdjustments.size()), 16);
    std::vector<std::array<float, 6>> sig;
    sig.reserve(size_t(n) + 1);
    sig.push_back(fields(a));
    for (int i = 0; i < n; ++i)
        sig.push_back(fields(a.localAdjustments[i]));
    return sig;
}

void RendererCore::prepareToneLut(const GlobalAdjustment& adjustment) {
    std::vector<std::array<float, 6>> signature = toneSignature(adjustment);
    if (signature == toneLutSource) // never empty once built (always has the global row)
        return;
    pendingToneLut = tone::makeLutAtlas(adjustment);
    toneLutDirty = true;
    toneLutSource = std::move(signature);
}

// Identity of each local adjustment's brush raster (nullptr for non-brush or
// unpainted), so the array is rebuilt only when a stroke swaps in a new raster.
static std::vector<const void*> brushSignature(const GlobalAdjustment& a) {
    const int n = std::min<int>(int(a.localAdjustments.size()), 16);
    std::vector<const void*> sig;
    sig.reserve(n);
    for (int i = 0; i < n; ++i) {
        const auto* b = std::get_if<BrushMask>(&a.localAdjustments[i].mask);
        sig.push_back(b && b->raster ? static_cast<const void*>(b->raster.get()) : nullptr);
    }
    return sig;
}

void RendererCore::prepareBrushMasks(const GlobalAdjustment& adjustment) {
    std::vector<const void*> signature = brushSignature(adjustment);
    if (signature == brushMaskSource)
        return;

    const int n = int(signature.size());
    // The array size comes from the first painted raster; every brush raster of an
    // image shares the buffer-derived resolution (docs/adr/0047). Without any
    // brush mask the array stays empty and the dummy is bound.
    int w = 0, h = 0;
    for (int i = 0; i < n; ++i) {
        const auto* b = std::get_if<BrushMask>(&adjustment.localAdjustments[i].mask);
        if (b && b->raster && b->raster->width > 0) {
            w = b->raster->width;
            h = b->raster->height;
            break;
        }
    }

    PendingBrushMasks pending;
    if (w > 0 && h > 0) {
        pending.width = w;
        pending.height = h;
        pending.layers.resize(n);
        const QByteArray zero(qsizetype(w) * h, '\0');
        for (int i = 0; i < n; ++i) {
            const auto* b = std::get_if<BrushMask>(&adjustment.localAdjustments[i].mask);
            if (b && b->raster && b->raster->width == w && b->raster->height == h)
                pending.layers[i] = QByteArray(
                    reinterpret_cast<const char*>(b->raster->data.data()),
                    qsizetype(b->raster->data.size()));
            else
                pending.layers[i] = zero;
        }
    }
    pendingBrushMasks = std::move(pending);
    brushMasksDirty = true;
    brushMaskSource = std::move(signature);
}

void RendererCore::setCurveLut(const std::array<float, 256 * 4>& rgba) {
    pendingCurveLut = rgba;
    curveLutDirty = true;
}

void RendererCore::setDisplayLut(const DisplayLut& lut) {
    if (!lut.valid())
        return;
    pendingDisplayLut = lut;
    displayLutDirty = true;
}

void RendererCore::flushPendingUploads(QRhiResourceUpdateBatch* batch) {
    if (needQuadUpload) {
        batch->uploadStaticBuffer(vbuf.get(), kQuad);
        needQuadUpload = false;
    }

    for (int i = 0; i < 2; ++i) {
        if (!pendingImageDirty[i])
            continue;
        if (!imageTex[i] || imageTex[i]->pixelSize() != pendingImage[i].size) {
            imageTex[i].reset(rhi->newTexture(QRhiTexture::RGBA32F, pendingImage[i].size));
            imageTex[i]->create();
            ++generation;
        }
        batch->uploadTexture(
            imageTex[i].get(),
            QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                0, 0, QRhiTextureSubresourceUploadDescription(pendingImage[i].rgba))));
        pendingImage[i] = {}; // the batch shares the byte array
        pendingImageDirty[i] = false;
    }

    for (int i = 0; i < 2; ++i) {
        if (!pendingSensorClipDirty[i])
            continue;
        if (!sensorClipTex[i] || sensorClipTex[i]->pixelSize() != pendingSensorClip[i].size) {
            sensorClipTex[i].reset(rhi->newTexture(QRhiTexture::RGBA32F, pendingSensorClip[i].size));
            sensorClipTex[i]->create();
            ++generation;
        }
        batch->uploadTexture(
            sensorClipTex[i].get(),
            QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                0, 0, QRhiTextureSubresourceUploadDescription(pendingSensorClip[i].rgba))));
        pendingSensorClip[i] = {};
        pendingSensorClipDirty[i] = false;
    }

    if (sensorClipDummyDirty) {
        const std::array<float, 4> black{};
        const QByteArray data(reinterpret_cast<const char*>(black.data()), qsizetype(sizeof(black)));
        batch->uploadTexture(
            sensorClipDummyTex.get(),
            QRhiTextureUploadDescription(
                QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(data))));
        sensorClipDummyDirty = false;
    }

    if (curveLutDirty) {
        const QByteArray data(
            reinterpret_cast<const char*>(pendingCurveLut.data()),
            qsizetype(pendingCurveLut.size() * sizeof(float)));
        batch->uploadTexture(
            curveLutTex.get(),
            QRhiTextureUploadDescription(
                QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(data))));
        curveLutDirty = false;
    }

    if (toneLutDirty) {
        const QByteArray data(
            reinterpret_cast<const char*>(pendingToneLut.rgba.data()),
            qsizetype(pendingToneLut.rgba.size() * sizeof(float)));
        batch->uploadTexture(
            toneLutTex.get(),
            QRhiTextureUploadDescription(
                QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(data))));
        toneLutDirty = false;
    }

    if (displayLutDirty) {
        const int n = pendingDisplayLut.size;
        if (!displayLutTex || displayLutTex->pixelSize() != QSize(n, n)) {
            displayLutTex.reset(
                rhi->newTexture(QRhiTexture::RGBA32F, n, n, n, 1, QRhiTexture::ThreeDimensional));
            displayLutTex->create();
            ++generation;
        }
        // One upload entry per depth slice (layer = z for 3D textures).
        const qsizetype sliceBytes = qsizetype(n) * n * 4 * sizeof(float);
        const char* base = reinterpret_cast<const char*>(pendingDisplayLut.data.data());
        std::vector<QRhiTextureUploadEntry> entries;
        entries.reserve(n);
        for (int z = 0; z < n; ++z)
            entries.emplace_back(
                z,
                0,
                QRhiTextureSubresourceUploadDescription(
                    QByteArray(base + z * sliceBytes, sliceBytes)));
        QRhiTextureUploadDescription desc;
        desc.setEntries(entries.begin(), entries.end());
        batch->uploadTexture(displayLutTex.get(), desc);
        pendingDisplayLut = {};
        displayLutDirty = false;
    }

    if (brushMaskDummyDirty) {
        const QByteArray data(1, '\0');
        batch->uploadTexture(
            brushMaskDummyTex.get(),
            QRhiTextureUploadDescription(
                QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(data))));
        brushMaskDummyDirty = false;
    }

    if (brushMasksDirty) {
        const int layers = int(pendingBrushMasks.layers.size());
        const QSize sz(pendingBrushMasks.width, pendingBrushMasks.height);
        if (layers == 0 || sz.isEmpty()) {
            if (brushMaskArrayTex) { // no brush mask left — fall back to the dummy
                brushMaskArrayTex.reset();
                ++generation;
            }
        } else {
            if (!brushMaskArrayTex || brushMaskArrayTex->pixelSize() != sz
                || brushMaskArrayTex->arraySize() != layers) {
                brushMaskArrayTex.reset(rhi->newTextureArray(QRhiTexture::R8, layers, sz));
                brushMaskArrayTex->create();
                ++generation;
            }
            std::vector<QRhiTextureUploadEntry> entries;
            entries.reserve(layers);
            for (int i = 0; i < layers; ++i)
                entries.emplace_back(
                    i, 0, QRhiTextureSubresourceUploadDescription(pendingBrushMasks.layers[i]));
            QRhiTextureUploadDescription desc;
            desc.setEntries(entries.begin(), entries.end());
            batch->uploadTexture(brushMaskArrayTex.get(), desc);
        }
        pendingBrushMasks = {};
        brushMasksDirty = false;
    }

    if (extraUploadTex) {
        batch->uploadTexture(
            extraUploadTex,
            QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                0, 0, QRhiTextureSubresourceUploadDescription(extraUploadData))));
        extraUploadTex = nullptr;
        extraUploadData = {};
    }
}

// ── Pipeline / bindings ───────────────────────────────────────────────────────

void RendererCore::buildBindings(
    std::unique_ptr<QRhiShaderResourceBindings>& dst,
    QRhiBuffer* ub,
    QRhiTexture* tex,
    QRhiTexture* sensorTex,
    QRhiTexture* spatialTex) {
    dst.reset(rhi->newShaderResourceBindings());
    dst->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, ub),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, tex, sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, curveLutTex.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            3, QRhiShaderResourceBinding::FragmentStage, displayLutTex.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            4, QRhiShaderResourceBinding::FragmentStage, toneLutTex.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            5, QRhiShaderResourceBinding::FragmentStage, sensorTex, sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            6, QRhiShaderResourceBinding::FragmentStage, spatialTex, sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            7,
            QRhiShaderResourceBinding::FragmentStage,
            brushMaskArrayTex ? brushMaskArrayTex.get() : brushMaskDummyTex.get(),
            sampler.get()),
    });
    dst->create();
}

QRhiShaderResourceBindings* RendererCore::bindingsFor(
    QRhiTexture* tex, QRhiTexture* sensorTex, QRhiTexture* spatialTex) {
    if (srb && srbImageTex == tex && srbSensorClipTex == sensorTex && srbSpatialTex == spatialTex
        && srbGeneration == generation)
        return srb.get();

    buildBindings(srb, ubuf.get(), tex, sensorTex, spatialTex);
    srbImageTex = tex;
    srbSensorClipTex = sensorTex;
    srbSpatialTex = spatialTex;
    srbGeneration = generation;
    return srb.get();
}

QRhiGraphicsPipeline* RendererCore::pipelineFor(QRhiRenderPassDescriptor* rpDesc) {
    const QVector<quint32> key = rpDesc->serializedFormat();
    for (const auto& [k, p] : pipelines)
        if (k == key)
            return p.get();

    auto pipe = std::unique_ptr<QRhiGraphicsPipeline>(rhi->newGraphicsPipeline());
    pipe->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipe->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    pipe->setVertexInputLayout(layout);
    pipe->setShaderResourceBindings(srb.get());
    pipe->setRenderPassDescriptor(rpDesc);
    pipe->create();

    pipelines.emplace_back(key, std::move(pipe));
    return pipelines.back().second.get();
}

// ── Uniforms ──────────────────────────────────────────────────────────────────

namespace {
// Normalise the shared colour scalars and retain the pre-ADR-0033 tone packing.
// Basic Tone itself is sourced from the LUT atlas; leaving these slots populated
// avoids a risky uniform-layout migration while laTone2/laColor still carry WB
// and colour values used by Local Adjustments.
struct SharedUniform {
    float exposure, contrast, highlights, shadows, whites, blacks, tint, saturation, vibrance;
};

SharedUniform toUniform(const SharedAdjustment& s) {
    return {
        s.exposure,
        s.contrast / kToneSliderToUniform,
        s.highlights / kToneSliderToUniform,
        s.shadows / kToneSliderToUniform,
        s.whites / kToneSliderToUniform,
        s.blacks / kToneSliderToUniform,
        s.tint / 100.0f,
        s.saturation / 100.0f,
        s.vibrance / 100.0f,
    };
}
} // namespace

void RendererCore::fillUbuf(Ubuf& ub, const FrameParams& fp) const {
    const QMatrix4x4 corr = rhi->clipSpaceCorrMatrix();
    std::memcpy(ub.clipCorr, corr.constData(), sizeof(ub.clipCorr));

    ub.transform[0] = fp.transform.x();
    ub.transform[1] = fp.transform.y();
    ub.transform[2] = fp.transform.z();
    ub.transform[3] = fp.transform.w();
    ub.cropRect[0] = float(fp.cropRect.left());
    ub.cropRect[1] = float(fp.cropRect.top());
    ub.cropRect[2] = float(fp.cropRect.right());
    ub.cropRect[3] = float(fp.cropRect.bottom());

    const GlobalAdjustment& a = fp.adjustments;
    ub.effectRect[0] = float(a.cropRect.left());
    ub.effectRect[1] = float(a.cropRect.top());
    ub.effectRect[2] = float(a.cropRect.right());
    ub.effectRect[3] = float(a.cropRect.bottom());
    bool hslActive = false;
    for (int i = 0; i < 8; ++i) {
        ub.hslHue[i] = a.hslHue[i] / 100.0f;
        ub.hslSat[i] = a.hslSat[i] / 100.0f;
        ub.hslLum[i] = a.hslLum[i] / 100.0f;
        if (a.hslHue[i] != 0.0f || a.hslSat[i] != 0.0f || a.hslLum[i] != 0.0f)
            hslActive = true;
    }

    const SharedUniform g = toUniform(a);
    ub.rotation = a.rotation;
    ub.aspect = fp.aspect;
    ub.orientQuarterTurns = a.orientation.quarterTurnsCW;
    ub.orientMirrored = a.orientation.mirrored ? 1 : 0;
    ub.exposure = g.exposure;
    ub.contrast = g.contrast;
    ub.highlights = g.highlights;
    ub.shadows = g.shadows;
    ub.whites = g.whites;
    ub.blacks = g.blacks;
    // White balance: blackbody-derived per-channel gain, computed CPU-side and
    // applied as a multiply in the shader so black stays black (docs/adr/0025).
    const auto wb = whiteBalanceGain(a.temperature, a.tint);
    ub.wbGainR = wb[0];
    ub.wbGainG = wb[1];
    ub.wbGainB = wb[2];
    ub.saturation = g.saturation;
    ub.vibrance = g.vibrance;
    // Highlight roll-off shoulder + path to white (docs/adr/0040); 0 = off.
    ub.filmicHighlights = std::clamp(a.filmicHighlights / 100.0f, 0.0f, 1.0f);
    ub.textureAmount = std::clamp(a.texture / 100.0f, -1.0f, 1.0f);
    ub.clarity = std::clamp(a.clarity / 100.0f, -1.0f, 1.0f);
    ub.dehaze = std::clamp(a.dehaze / 100.0f, -1.0f, 1.0f);
    ub.postCropVignetteAmount = a.postCropVignetteAmount / 50.0f;
    ub.postCropVignetteMidpoint = std::clamp(a.postCropVignetteMidpoint / 100.0f, 0.0f, 1.0f);
    ub.postCropVignetteFeather = std::clamp(a.postCropVignetteFeather / 100.0f, 0.0f, 1.0f);
    ub.grainAmount = std::clamp(a.grainAmount / 100.0f, 0.0f, 1.0f) * 0.08f;
    const float size = std::clamp(a.grainSize / 100.0f, 0.0f, 1.0f);
    ub.grainSize = std::exp2(std::lerp(std::log2(0.5f / 2048.0f), std::log2(4.0f / 2048.0f), size));
    ub.grainRoughness = std::clamp(a.grainRoughness / 100.0f, 0.0f, 1.0f);
    // A foreign crs sidecar can enable Grain without arraw:GrainSeed. Until the
    // editor opens that image and mints a persistent seed, render with a stable
    // fallback derived from the image's own geometry, so distinct photos in a
    // grid don't all share one grain pattern.
    if (a.grainAmount > 0.0f && a.grainSeed == 0) {
        quint32 h = 0x6d2b79f5U;
        auto mix = [&h](float f) {
            quint32 bits;
            std::memcpy(&bits, &f, sizeof(bits));
            h ^= bits + 0x9e3779b9U + (h << 6) + (h >> 2);
        };
        mix(fp.aspect);
        mix(float(a.cropRect.left()));
        mix(float(a.cropRect.top()));
        mix(float(a.cropRect.right()));
        mix(float(a.cropRect.bottom()));
        ub.grainSeed = h | 1U;
    } else {
        ub.grainSeed = a.grainSeed;
    }

    ub.useLut = fp.useLut ? 1 : 0;
    ub.gamutWarn = fp.gamutWarn ? 1 : 0;
    ub.baseLook = fp.baseLook ? 1 : 0;
    ub.displayEncode = fp.displayEncode ? 1 : 0;
    ub.curveInput = fp.curveInput ? 1 : 0;
    ub.hslActive = hslActive ? 1 : 0;
    ub.wbInput = fp.wbInput ? 1 : 0;
    ub.clipWarn = (fp.clipHighlights ? 1 : 0) | (fp.clipShadows ? 2 : 0);
    ub.histoRaw = fp.histoRaw ? 1 : 0;
    ub.sensorClipWarn = fp.sensorClip ? 1 : 0;

    // Local adjustments (docs/adr/0010): pack geometry and colour values into
    // the parallel vec4 arrays, honouring the 16-mask cap. Tone comes from the
    // matching LUT row (docs/adr/0033); white balance remains a channel gain.
    const int n = std::min<int>(int(a.localAdjustments.size()), 16);
    ub.numLocalAdj = n;
    for (int i = 0; i < n; ++i) {
        const LocalAdjustment& la = a.localAdjustments[i];
        const SharedUniform d = toUniform(la);
        const int k = i * 4;

        float maskType = 0.0f; // 0 = Linear, 1 = Radial, 2 = Brush
        if (std::holds_alternative<BrushMask>(la.mask)) {
            // Geometry slots unused; the raster is sampled at vUV from the brush
            // array's layer i (docs/adr/0047).
            maskType = 2.0f;
        } else if (const auto* m = std::get_if<LinearMask>(&la.mask)) {
            ub.laGeom[k + 0] = float(m->p0.x());
            ub.laGeom[k + 1] = float(m->p0.y());
            ub.laGeom[k + 2] = float(m->p1.x());
            ub.laGeom[k + 3] = float(m->p1.y());
        } else if (const auto* r = std::get_if<RadialMask>(&la.mask)) {
            maskType = 1.0f;
            ub.laGeom[k + 0] = float(r->center.x());
            ub.laGeom[k + 1] = float(r->center.y());
            ub.laGeom[k + 2] = float(r->radiusX);
            ub.laGeom[k + 3] = float(r->radiusY);
            ub.laGeom2[k + 0] = float(r->angle);
            ub.laGeom2[k + 1] = float(r->feather);
            ub.laGeom2[k + 2] = r->invert ? 1.0f : 0.0f;
            ub.laGeom2[k + 3] = 0.0f;
        }
        ub.laTone[k + 0] = d.exposure;
        ub.laTone[k + 1] = d.contrast;
        ub.laTone[k + 2] = d.highlights;
        ub.laTone[k + 3] = d.shadows;
        ub.laTone2[k + 0] = d.whites;
        ub.laTone2[k + 1] = d.blacks;
        // Local white balance: the relative -100..100 shift maps to an effective
        // Kelvin and reuses the global blackbody gain (docs/adr/0025); the three
        // gain channels ride in the spare laTone2.zw / laColor.w slots.
        constexpr float kLocalTempKelvinPerUnit = 30.0f; // ±100 → 2500..8500 K
        const auto lwb
            = whiteBalanceGain(5500.0f + la.temperature * kLocalTempKelvinPerUnit, la.tint);
        ub.laTone2[k + 2] = lwb[0];
        ub.laTone2[k + 3] = lwb[1];
        ub.laColor[k + 0] = d.saturation;
        ub.laColor[k + 1] = d.vibrance;
        ub.laColor[k + 2] = maskType;
        ub.laColor[k + 3] = lwb[2];
    }
}

// ── Pass recording — the single point the pipeline is drawn (ADR 0006) ───────

// `batch` must already contain the pending uploads (flushPendingUploads can
// recreate image textures, so the sampled texture is resolved only afterwards).
void RendererCore::recordPassWith(
    QRhiCommandBuffer* cb,
    QRhiRenderTarget* rt,
    const FrameParams& fp,
    QRhiResourceUpdateBatch* batch,
    QRhiBuffer* ub,
    QRhiShaderResourceBindings* bindings) {
    Ubuf u{};
    fillUbuf(u, fp);
    batch->updateDynamicBuffer(ub, 0, sizeof(Ubuf), &u);

    // The pipeline carries only the bindings *layout*; `bindings` is bound per
    // draw and need only be layout-compatible (same as the on-screen srb).
    QRhiGraphicsPipeline* pipe = pipelineFor(rt->renderPassDescriptor());

    cb->beginPass(rt, kClearColor, {1.0f, 0}, batch);
    cb->setGraphicsPipeline(pipe);
    const QSize sz = rt->pixelSize();
    cb->setViewport({0, 0, float(sz.width()), float(sz.height())});
    cb->setShaderResources(bindings);
    const QRhiCommandBuffer::VertexInput vi(vbuf.get(), 0);
    cb->setVertexInput(0, 1, &vi);
    cb->draw(4);
    cb->endPass();
}

void RendererCore::recordPass(
    QRhiCommandBuffer* cb,
    QRhiRenderTarget* rt,
    QRhiTexture* tex,
    QRhiTexture* sensorTex,
    QRhiTexture* spatialTex,
    const FrameParams& fp,
    QRhiResourceUpdateBatch* batch) {
    recordPassWith(cb, rt, fp, batch, ubuf.get(), bindingsFor(tex, sensorTex, spatialTex));
}

// ── Colour Noise Reduction pre-pass (docs/adr/0034) ──────────────────────────

void RendererCore::ensureNrResources() {
    if (nrRpDesc)
        return;
    // A standalone RGBA32F render-pass descriptor, kept independent of any slot's
    // textures so the NR pipelines stay valid across slot resizes. Built from a
    // throwaway 1×1 target; the descriptor outlives it.
    std::unique_ptr<QRhiTexture> tmp(
        rhi->newTexture(QRhiTexture::RGBA32F, QSize(1, 1), 1, QRhiTexture::RenderTarget));
    tmp->create();
    QRhiColorAttachment att(tmp.get());
    std::unique_ptr<QRhiTextureRenderTarget> tmpRt(rhi->newTextureRenderTarget({att}));
    nrRpDesc.reset(tmpRt->newCompatibleRenderPassDescriptor());
}

void RendererCore::ensureNrSlot(int key, QSize fullSize) {
    NrSlot& ns = nrSlot[key];
    if (ns.denoised && ns.fullSize == fullSize && ns.gen == generation)
        return;
    ns.fullSize = fullSize;
    const QSize quarter(std::max(1, fullSize.width() / 4), std::max(1, fullSize.height() / 4));

    auto make = [&](std::unique_ptr<QRhiTexture>& tex,
                    std::unique_ptr<QRhiTextureRenderTarget>& rt,
                    QSize sz) {
        tex.reset(rhi->newTexture(
            QRhiTexture::RGBA32F,
            sz,
            1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        tex->create();
        QRhiColorAttachment att(tex.get());
        rt.reset(rhi->newTextureRenderTarget({att}));
        rt->setRenderPassDescriptor(nrRpDesc.get());
        rt->create();
    };
    make(ns.chromaA, ns.chromaART, quarter);
    make(ns.chromaB, ns.chromaBRT, quarter);
    make(ns.denoised, ns.denoisedRT, fullSize);
    ns.smoothness = -1.0f; // textures changed → force a re-run
    ns.strength = -1.0f;
    ns.gen = generation;
}

void RendererCore::nrPass(
    QRhiCommandBuffer* cb,
    QRhiTextureRenderTarget* rt,
    QRhiGraphicsPipeline* pipe,
    QRhiShaderResourceBindings* bindings,
    QRhiResourceUpdateBatch* batch) {
    cb->beginPass(rt, kClearColor, {1.0f, 0}, batch);
    cb->setGraphicsPipeline(pipe);
    const QSize sz = rt->pixelSize();
    cb->setViewport({0, 0, float(sz.width()), float(sz.height())});
    cb->setShaderResources(bindings);
    const QRhiCommandBuffer::VertexInput vi(vbuf.get(), 0);
    cb->setVertexInput(0, 1, &vi);
    cb->draw(4);
    cb->endPass();
}

QRhiTexture* RendererCore::ensureDenoised(
    QRhiCommandBuffer* cb, int key, QRhiTexture* rawTex, float smoothness, float strength) {
    ensureNrResources();
    ensureNrSlot(key, rawTex->pixelSize());
    NrSlot& ns = nrSlot[key];

    if (ns.smoothness == smoothness && ns.strength == strength && ns.gen == generation)
        return ns.denoised.get(); // cached — reused for pan/zoom/other edits, no work
    ns.smoothness = smoothness;
    ns.strength = strength;

    // SRBs point at the live textures; rebuilt each run (only on amount/texture
    // change), kept as members so they outlive command-buffer submission.
    auto bindUbufTex = [&](std::unique_ptr<QRhiShaderResourceBindings>& dst, QRhiTexture* t) {
        dst.reset(rhi->newShaderResourceBindings());
        dst->setBindings(
            {QRhiShaderResourceBinding::uniformBuffer(
                 0,
                 QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                 nrUbuf.get()),
             QRhiShaderResourceBinding::sampledTexture(
                 1, QRhiShaderResourceBinding::FragmentStage, t, sampler.get())});
        dst->create();
    };
    bindUbufTex(nrSrbExtract, rawTex);
    bindUbufTex(nrSrbBlurH, ns.chromaA.get());
    bindUbufTex(nrSrbBlurV, ns.chromaB.get());
    nrSrbRecombine.reset(rhi->newShaderResourceBindings());
    nrSrbRecombine->setBindings(
        {QRhiShaderResourceBinding::uniformBuffer(
             0,
             QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
             nrUbuf.get()),
         QRhiShaderResourceBinding::sampledTexture(
             1, QRhiShaderResourceBinding::FragmentStage, rawTex, sampler.get()),
         QRhiShaderResourceBinding::sampledTexture(
             2, QRhiShaderResourceBinding::FragmentStage, ns.chromaA.get(), sampler.get())});
    nrSrbRecombine->create();

    if (!nrPipeExtract) {
        auto makePipe = [&](std::unique_ptr<QRhiGraphicsPipeline>& dst,
                            const QShader& fsStage,
                            QRhiShaderResourceBindings* layout) {
            dst.reset(rhi->newGraphicsPipeline());
            dst->setTopology(QRhiGraphicsPipeline::TriangleStrip);
            dst->setShaderStages(
                {{QRhiShaderStage::Vertex, nrVs}, {QRhiShaderStage::Fragment, fsStage}});
            QRhiVertexInputLayout vl;
            vl.setBindings({{4 * sizeof(float)}});
            vl.setAttributes({
                {0, 0, QRhiVertexInputAttribute::Float2, 0},
                {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
            });
            dst->setVertexInputLayout(vl);
            dst->setShaderResourceBindings(layout);
            dst->setRenderPassDescriptor(nrRpDesc.get());
            dst->create();
        };
        makePipe(nrPipeExtract, nrExtractFs, nrSrbExtract.get());
        makePipe(nrPipeBlurH, nrBlurHFs, nrSrbBlurH.get());
        makePipe(nrPipeBlurV, nrBlurVFs, nrSrbBlurV.get());
        makePipe(nrPipeRecombine, nrRecombineFs, nrSrbRecombine.get());
    }

    // Smoothness drives the sigma (calibrated in full-res pixels; the blur runs on
    // quarter-res chroma); Strength drives the recombine blend (issue #59).
    const float sigmaQuarter = colorNoiseReductionSigmaPx(smoothness) / 4.0f;
    NrUbuf nb{};
    const QMatrix4x4 cc = rhi->clipSpaceCorrMatrix();
    std::memcpy(nb.clipCorr, cc.constData(), sizeof(nb.clipCorr));
    nb.invChroma[0] = 1.0f / float(ns.chromaA->pixelSize().width());
    nb.invChroma[1] = 1.0f / float(ns.chromaA->pixelSize().height());
    nb.sigma = sigmaQuarter;
    nb.radius = std::clamp(int(std::ceil(3.0f * sigmaQuarter)), 1, 64);
    nb.strength = colorNoiseReductionStrengthMix(strength);
    // On a Y-up framebuffer (OpenGL) the rendered NR targets are V-flipped vs the
    // uploaded image texture; the vertex shader mirrors the sampled V to cancel it.
    nb.flipV = rhi->isYUpInFramebuffer() ? 1 : 0;

    QRhiResourceUpdateBatch* b = rhi->nextResourceUpdateBatch();
    b->updateDynamicBuffer(nrUbuf.get(), 0, sizeof(NrUbuf), &nb);

    nrPass(cb, ns.chromaART.get(), nrPipeExtract.get(), nrSrbExtract.get(), b);   // raw → chroma A
    nrPass(cb, ns.chromaBRT.get(), nrPipeBlurH.get(), nrSrbBlurH.get(), nullptr); // A → B (H)
    nrPass(cb, ns.chromaART.get(), nrPipeBlurV.get(), nrSrbBlurV.get(), nullptr); // B → A (V)
    nrPass(cb, ns.denoisedRT.get(), nrPipeRecombine.get(), nrSrbRecombine.get(), nullptr); // → full
    return ns.denoised.get();
}

// Colour NR is an exact no-op (the pre-pass is skipped, the main pass samples the
// raw slot) unless both controls are positive: Strength 0 blends nothing back, and
// Smoothness 0 is a zero-sigma identity blur (issue #59).
static bool nrActive(const GlobalAdjustment& a) {
    return a.colorNoiseReduction > 0.0f && a.colorNoiseReductionSmoothness > 0.0f;
}

static bool spatialContextActive(const GlobalAdjustment& a) {
    return a.clarity != 0.0f || a.dehaze != 0.0f;
}

void RendererCore::ensureSpatialSlot(int key, QSize fullSize) {
    SpatialSlot& ss = spatialSlot[key];
    if (ss.lumaB && ss.fullSize == fullSize && ss.gen == generation)
        return;
    ss.fullSize = fullSize;
    const QSize quarter(std::max(1, fullSize.width() / 4), std::max(1, fullSize.height() / 4));

    auto make = [&](std::unique_ptr<QRhiTexture>& tex,
                    std::unique_ptr<QRhiTextureRenderTarget>& rt) {
        tex.reset(rhi->newTexture(
            QRhiTexture::RGBA32F,
            quarter,
            1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        tex->create();
        QRhiColorAttachment att(tex.get());
        rt.reset(rhi->newTextureRenderTarget({att}));
        rt->setRenderPassDescriptor(nrRpDesc.get());
        rt->create();
    };
    make(ss.lumaA, ss.lumaART);
    make(ss.lumaB, ss.lumaBRT);
    ss.gen = generation;
    spatialSrbBlurHTex = nullptr;
    spatialSrbBlurVTex = nullptr;
}

QRhiTexture* RendererCore::ensureSpatialContext(QRhiCommandBuffer* cb, int key, QRhiTexture* rawTex) {
    ensureNrResources(); // shares the same RGBA32F render-pass descriptor and nrUbuf layout
    ensureSpatialSlot(key, rawTex->pixelSize());
    SpatialSlot& ss = spatialSlot[key];

    auto bindUbufTex = [&](std::unique_ptr<QRhiShaderResourceBindings>& dst,
                           QRhiTexture*& cachedTex,
                           QRhiTexture* t) {
        if (dst && cachedTex == t)
            return;
        dst.reset(rhi->newShaderResourceBindings());
        dst->setBindings(
            {QRhiShaderResourceBinding::uniformBuffer(
                 0,
                 QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                 spatialUbuf.get()),
             QRhiShaderResourceBinding::sampledTexture(
                 1, QRhiShaderResourceBinding::FragmentStage, t, sampler.get())});
        dst->create();
        cachedTex = t;
    };
    bindUbufTex(spatialSrbExtract, spatialSrbExtractTex, rawTex);
    bindUbufTex(spatialSrbBlurH, spatialSrbBlurHTex, ss.lumaA.get());
    bindUbufTex(spatialSrbBlurV, spatialSrbBlurVTex, ss.lumaB.get());

    if (!spatialPipeExtract) {
        auto makePipe = [&](std::unique_ptr<QRhiGraphicsPipeline>& dst,
                            const QShader& fsStage,
                            QRhiShaderResourceBindings* layout) {
            dst.reset(rhi->newGraphicsPipeline());
            dst->setTopology(QRhiGraphicsPipeline::TriangleStrip);
            dst->setShaderStages(
                {{QRhiShaderStage::Vertex, nrVs}, {QRhiShaderStage::Fragment, fsStage}});
            QRhiVertexInputLayout vl;
            vl.setBindings({{4 * sizeof(float)}});
            vl.setAttributes({
                {0, 0, QRhiVertexInputAttribute::Float2, 0},
                {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
            });
            dst->setVertexInputLayout(vl);
            dst->setShaderResourceBindings(layout);
            dst->setRenderPassDescriptor(nrRpDesc.get());
            dst->create();
        };
        makePipe(spatialPipeExtract, spatialExtractFs, spatialSrbExtract.get());
        makePipe(spatialPipeBlurH, nrBlurHFs, spatialSrbBlurH.get());
        makePipe(spatialPipeBlurV, nrBlurVFs, spatialSrbBlurV.get());
    }

    NrUbuf nb{};
    const QMatrix4x4 cc = rhi->clipSpaceCorrMatrix();
    std::memcpy(nb.clipCorr, cc.constData(), sizeof(nb.clipCorr));
    nb.invChroma[0] = 1.0f / float(ss.lumaA->pixelSize().width());
    nb.invChroma[1] = 1.0f / float(ss.lumaA->pixelSize().height());
    nb.sigma = 2.5f;
    nb.radius = 8;
    nb.flipV = rhi->isYUpInFramebuffer() ? 1 : 0;

    QRhiResourceUpdateBatch* b = rhi->nextResourceUpdateBatch();
    b->updateDynamicBuffer(spatialUbuf.get(), 0, sizeof(NrUbuf), &nb);

    nrPass(cb, ss.lumaART.get(), spatialPipeExtract.get(), spatialSrbExtract.get(), b);
    nrPass(cb, ss.lumaBRT.get(), spatialPipeBlurH.get(), spatialSrbBlurH.get(), nullptr);
    nrPass(cb, ss.lumaART.get(), spatialPipeBlurV.get(), spatialSrbBlurV.get(), nullptr);
    return ss.lumaA.get();
}

void RendererCore::record(
    QRhiCommandBuffer* cb, QRhiRenderTarget* rt, Slot slot, const FrameParams& fp) {
    if (!hasImage(slot)) {
        clear(cb, rt);
        return;
    }
    prepareToneLut(fp.adjustments);
    prepareBrushMasks(fp.adjustments);
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    flushPendingUploads(batch); // creates/recreates the slot's texture
    const int slotIndex = int(slot);
    QRhiTexture* tex = imageTex[slotIndex].get();
    QRhiTexture* sensorTex = sensorClipTex[slotIndex] ? sensorClipTex[slotIndex].get()
                                                      : sensorClipDummyTex.get();
    QRhiTexture* spatialTex = sensorClipDummyTex.get();
    if (nrActive(fp.adjustments)) {
        cb->resourceUpdate(batch); // apply uploads before the NR pre-passes
        tex = ensureDenoised(
            cb,
            slotIndex,
            tex,
            fp.adjustments.colorNoiseReductionSmoothness,
            fp.adjustments.colorNoiseReduction);
        batch = rhi->nextResourceUpdateBatch(); // fresh batch for the main pass
    }
    if (spatialContextActive(fp.adjustments)) {
        cb->resourceUpdate(batch);
        spatialTex = ensureSpatialContext(cb, slotIndex, tex);
        batch = rhi->nextResourceUpdateBatch();
    }
    recordPass(cb, rt, tex, sensorTex, spatialTex, fp, batch);
}

void RendererCore::clear(QRhiCommandBuffer* cb, QRhiRenderTarget* rt) {
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    flushPendingUploads(batch);
    cb->beginPass(rt, kClearColor, {1.0f, 0}, batch);
    cb->endPass();
}

// ── Offscreen render + synchronous readback ──────────────────────────────────

QImage RendererCore::readbackToImage(const QRhiReadbackResult& rr) const {
    const bool isFloat = rr.format == QRhiTexture::RGBA32F;
    const QImage::Format qfmt = isFloat ? QImage::Format_RGBX32FPx4 : QImage::Format_RGBA8888;
    const int bpp = isFloat ? 16 : 4;
    const int w = rr.pixelSize.width();
    const int h = rr.pixelSize.height();
    // Readbacks come out in the backend's framebuffer orientation: bottom-up
    // when Y is up in the framebuffer (OpenGL), top-down elsewhere.
    const bool flip = rhi->isYUpInFramebuffer();

    QImage img(w, h, qfmt);
    const qsizetype rowBytes = qsizetype(w) * bpp;
    const char* src = rr.data.constData();
    for (int y = 0; y < h; ++y)
        std::memcpy(img.scanLine(flip ? h - 1 - y : y), src + y * rowBytes, rowBytes);
    return img;
}

// slotIndex >= 0 samples that image slot (resolved after the upload flush,
// which may recreate it); slotIndex < 0 samples extTex (export's temporary,
// uploaded via extraUpload* in the same flush).
QImage RendererCore::renderOffscreenTex(
    int slotIndex, QRhiTexture* extTex, const FrameParams& fp, QSize size, QRhiTexture::Format fmt) {
    std::unique_ptr<QRhiTexture> target(
        rhi->newTexture(fmt, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!target->create())
        return {};
    QRhiColorAttachment att(target.get());
    std::unique_ptr<QRhiTextureRenderTarget> rt(rhi->newTextureRenderTarget({att}));
    std::unique_ptr<QRhiRenderPassDescriptor> rp(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rp.get());
    rt->create();

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return {};
    prepareToneLut(fp.adjustments);
    prepareBrushMasks(fp.adjustments);
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    flushPendingUploads(batch);
    QRhiTexture* tex = slotIndex >= 0 ? imageTex[slotIndex].get() : extTex;
    QRhiTexture* sensorTex = slotIndex >= 0 && sensorClipTex[slotIndex]
                                 ? sensorClipTex[slotIndex].get()
                                 : sensorClipDummyTex.get();
    QRhiTexture* spatialTex = sensorClipDummyTex.get();
    if (nrActive(fp.adjustments)) {
        // key 2 is the export/extTex scratch; its source texture changes every
        // call, so force a recompute rather than trust the (smoothness,strength) cache.
        const int nrKey = slotIndex >= 0 ? slotIndex : 2;
        if (nrKey == 2)
            nrSlot[2].smoothness = -1.0f;
        cb->resourceUpdate(batch);
        tex = ensureDenoised(
            cb,
            nrKey,
            tex,
            fp.adjustments.colorNoiseReductionSmoothness,
            fp.adjustments.colorNoiseReduction);
        batch = rhi->nextResourceUpdateBatch();
    }
    if (spatialContextActive(fp.adjustments)) {
        const int spatialKey = slotIndex >= 0 ? slotIndex : 2;
        cb->resourceUpdate(batch);
        spatialTex = ensureSpatialContext(cb, spatialKey, tex);
        batch = rhi->nextResourceUpdateBatch();
    }
    recordPass(cb, rt.get(), tex, sensorTex, spatialTex, fp, batch);

    QRhiReadbackResult rr;
    QRhiResourceUpdateBatch* readBatch = rhi->nextResourceUpdateBatch();
    readBatch->readBackTexture(QRhiReadbackDescription(target.get()), &rr);
    cb->resourceUpdate(readBatch);
    rhi->endOffscreenFrame(); // completes the readback

    return readbackToImage(rr);
}

QImage RendererCore::renderOffscreen(
    Slot slot, const FrameParams& fp, QSize size, QRhiTexture::Format fmt) {
    if (!rhi || !hasImage(slot))
        return {};
    return renderOffscreenTex(int(slot), nullptr, fp, size, fmt);
}

QImage RendererCore::renderOffscreen(
    const ImageBuffer& buf, const FrameParams& fp, QSize size, QRhiTexture::Format fmt) {
    if (!rhi || !buf.valid())
        return {};
    std::unique_ptr<QRhiTexture> tex(
        rhi->newTexture(QRhiTexture::RGBA32F, QSize(buf.width, buf.height)));
    if (!tex->create())
        return {};
    ++generation;
    extraUploadTex = tex.get();
    extraUploadData = expandToRgba(buf);
    QImage out = renderOffscreenTex(-1, tex.get(), fp, size, fmt);
    // The srb may reference the temporary texture; never let a future
    // same-address allocation alias it.
    srbImageTex = nullptr;
    return out;
}

// ── Non-blocking offscreen render + async readback (docs/adr/0035) ────────────

RendererCore::ReadbackTarget* RendererCore::ensureReadbackTarget(
    QSize size, QRhiTexture::Format fmt) {
    for (const std::unique_ptr<ReadbackTarget>& t : readbackPool)
        if (t->size == size && t->fmt == fmt)
            return t.get();

    auto t = std::make_unique<ReadbackTarget>();
    t->size = size;
    t->fmt = fmt;
    t->ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Ubuf)));
    if (!t->ubuf->create())
        return nullptr;
    t->tex.reset(
        rhi->newTexture(fmt, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!t->tex->create())
        return nullptr;
    QRhiColorAttachment att(t->tex.get());
    t->rt.reset(rhi->newTextureRenderTarget({att}));
    t->rp.reset(t->rt->newCompatibleRenderPassDescriptor());
    t->rt->setRenderPassDescriptor(t->rp.get());
    if (!t->rt->create())
        return nullptr;
    readbackPool.push_back(std::move(t));
    return readbackPool.back().get();
}

bool RendererCore::recordOffscreenReadback(
    QRhiCommandBuffer* cb,
    Slot slot,
    const FrameParams& fp,
    QSize size,
    QRhiTexture::Format fmt,
    std::function<void(QImage)> onReady) {
    if (!rhi || !hasImage(slot))
        return false;
    ReadbackTarget* t = ensureReadbackTarget(size, fmt);
    if (!t || t->inFlight)
        return false; // back-pressure: drop this refresh, the debounce retries

    // Mirrors renderOffscreenTex's body minus the frame open/close: record the
    // pass into the caller's in-flight `cb`, reusing this frame's cached NR
    // denoised texture (ADR 0034) so the sample matches the live preview.
    prepareToneLut(fp.adjustments);
    prepareBrushMasks(fp.adjustments);
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    flushPendingUploads(batch);
    const int slotIndex = int(slot);
    QRhiTexture* tex = imageTex[slotIndex].get();
    QRhiTexture* sensorTex = sensorClipTex[slotIndex] ? sensorClipTex[slotIndex].get()
                                                      : sensorClipDummyTex.get();
    QRhiTexture* spatialTex = sensorClipDummyTex.get();
    if (nrActive(fp.adjustments)) {
        cb->resourceUpdate(batch);
        tex = ensureDenoised(
            cb,
            slotIndex,
            tex,
            fp.adjustments.colorNoiseReductionSmoothness,
            fp.adjustments.colorNoiseReduction);
        batch = rhi->nextResourceUpdateBatch();
    }
    if (spatialContextActive(fp.adjustments)) {
        cb->resourceUpdate(batch);
        spatialTex = ensureSpatialContext(cb, slotIndex, tex);
        batch = rhi->nextResourceUpdateBatch();
    }
    // Record with the target's own uniform buffer + bindings — never the shared
    // on-screen `ubuf` — so this pass cannot clobber the main pass's uniforms
    // within the frame they share (see ReadbackTarget::ubuf).
    if (!t->srb || t->srbImageTex != tex || t->srbSensorTex != sensorTex
        || t->srbSpatialTex != spatialTex || t->srbGeneration != generation) {
        buildBindings(t->srb, t->ubuf.get(), tex, sensorTex, spatialTex);
        t->srbImageTex = tex;
        t->srbSensorTex = sensorTex;
        t->srbSpatialTex = spatialTex;
        t->srbGeneration = generation;
    }
    recordPassWith(cb, t->rt.get(), fp, batch, t->ubuf.get(), t->srb.get());

    QRhiResourceUpdateBatch* readBatch = rhi->nextResourceUpdateBatch();
    readBatch->readBackTexture(QRhiReadbackDescription(t->tex.get()), &t->rr);
    t->inFlight = true;
    // `t` is a stable pooled address owned by this RendererCore; the pool (and
    // each QRhiReadbackResult) outlives the in-flight window. On teardown the
    // pool is cleared, destroying the result before it can fire — so the capture
    // never dangles. `completed` runs on the GUI thread during frame processing.
    t->rr.completed = [this, t, onReady = std::move(onReady)]() {
        QImage img = readbackToImage(t->rr);
        t->inFlight = false;
        onReady(img);
    };
    cb->resourceUpdate(readBatch);
    return true;
}
