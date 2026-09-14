#include "render/RendererCore.h"
#include "core/NoiseReduction.h"
#include "core/Orientation.h"
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
// and the widget palette never drift (ADR 0031). The value is baked into the
// golden-image references (ADR 0005).
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
    nrBilateralHFs = loadShader(QStringLiteral(":/shaders/lum_bilateral_h.frag.qsb"));
    nrBilateralVFs = loadShader(QStringLiteral(":/shaders/lum_bilateral_v.frag.qsb"));
    peakingEdgeFs = loadShader(QStringLiteral(":/shaders/peaking_edge.frag.qsb"));

    vbuf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    vbuf->create();
    needQuadUpload = true;

    ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Ubuf)));
    ubuf->create();

    nrUbuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(NrUbuf)));
    nrUbuf->create();

    nrLumaUbuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(NrUbuf)));
    nrLumaUbuf->create();

    spatialUbuf.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(NrUbuf)));
    spatialUbuf->create();

    focusPeakingSourceUbuf.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Ubuf)));
    focusPeakingSourceUbuf->create();

    peakingEdgeUbuf.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(NrUbuf)));
    peakingEdgeUbuf->create();

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
    srbPeakingMaskTex = nullptr;
    srbGeneration = -1;
    peakingEdgePipe.reset();
    peakingEdgeSrb.reset();
    peakingEdgeSrbTex = nullptr;
    focusPeakingSourceSrb.reset();
    focusPeakingSourceUbuf.reset();
    peakingEdgeUbuf.reset();
    focusPeakingSlot = FocusPeakingSlot{};
    nrPipeExtract.reset();
    nrPipeBlurH.reset();
    nrPipeBlurV.reset();
    nrPipeBilateralH.reset();
    nrPipeBilateralV.reset();
    nrPipeRecombine.reset();
    nrSrbExtract.reset();
    nrSrbBlurH.reset();
    nrSrbBlurV.reset();
    nrSrbBilateralH.reset();
    nrSrbBilateralV.reset();
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
    nrLumaUbuf.reset();
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
    QRhiTexture* spatialTex,
    QRhiTexture* peakingMaskTex) {
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
        QRhiShaderResourceBinding::sampledTexture(
            8, QRhiShaderResourceBinding::FragmentStage, peakingMaskTex, sampler.get()),
    });
    dst->create();
}

QRhiShaderResourceBindings* RendererCore::bindingsFor(
    QRhiTexture* tex, QRhiTexture* sensorTex, QRhiTexture* spatialTex, QRhiTexture* peakingMaskTex) {
    if (srb && srbImageTex == tex && srbSensorClipTex == sensorTex && srbSpatialTex == spatialTex
        && srbPeakingMaskTex == peakingMaskTex && srbGeneration == generation)
        return srb.get();

    buildBindings(srb, ubuf.get(), tex, sensorTex, spatialTex, peakingMaskTex);
    srbImageTex = tex;
    srbSensorClipTex = sensorTex;
    srbSpatialTex = spatialTex;
    srbPeakingMaskTex = peakingMaskTex;
    srbGeneration = generation;
    return srb.get();
}

QRhiGraphicsPipeline* RendererCore::pipelineFor(
    QRhiRenderPassDescriptor* rpDesc, QRhiShaderResourceBindings* bindings) {
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
    // Only fixes the pipeline's resource-binding *layout* (docs/adr/0006's "the
    // pipeline carries only the bindings layout" — see recordPassWith); any
    // layout-compatible srb can be bound per draw afterwards. Must be the
    // caller's actual `bindings`, not always the shared on-screen `srb` member —
    // that member may not exist yet the first time a given render-pass-descriptor
    // format is requested via a caller (e.g. Focus Peaking's source pass, docs/adr/0058)
    // other than the on-screen recordPass()/bindingsFor() path.
    pipe->setShaderResourceBindings(bindings);
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
    const auto wb = WhiteBalance{a.temperature, a.tint}.gain();
    ub.wbGainR = wb[0];
    ub.wbGainG = wb[1];
    ub.wbGainB = wb[2];
    ub.saturation = g.saturation;
    ub.vibrance = g.vibrance;
    // Black & White (docs/adr/0048): the treatment toggle plus the raw -100..+100
    // mixer weights (the shader divides by 100, mirroring colour::applyBlackAndWhite).
    ub.convertToGrayscale = a.convertToGrayscale ? 1 : 0;
    for (int i = 0; i < 8; ++i)
        ub.bwMix[i] = a.bwMix[i];
    // Colour Grading (docs/adr/0052): raw values passed through (hue in degrees,
    // sat/blending 0..100, balance -100..+100); the shader mirrors the CPU
    // normalisation in colour::applyColourGrading. Packed as vec4[2]:
    //   [0..3] = shadow(hue,sat), midtone(hue,sat)
    //   [4..7] = highlight(hue,sat), balance, blending
    ub.colorGrade[0] = a.colorGradeHue[0];
    ub.colorGrade[1] = a.colorGradeSat[0];
    ub.colorGrade[2] = a.colorGradeHue[1];
    ub.colorGrade[3] = a.colorGradeSat[1];
    ub.colorGrade[4] = a.colorGradeHue[2];
    ub.colorGrade[5] = a.colorGradeSat[2];
    ub.colorGrade[6] = a.colorGradeBalance;
    ub.colorGrade[7] = a.colorGradeBlending;
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
    ub.maskOverlay = fp.maskOverlay < n ? fp.maskOverlay : -1; // tint the edited mask (adr 0047)
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
        const WhiteBalance localWb{
            WhiteBalance::kNeutralKelvin + la.temperature * kLocalTempKelvinPerUnit, la.tint};
        const auto lwb = localWb.gain();
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
    QRhiGraphicsPipeline* pipe = pipelineFor(rt->renderPassDescriptor(), bindings);

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
    QRhiTexture* peakingMaskTex,
    const FrameParams& fp,
    QRhiResourceUpdateBatch* batch) {
    recordPassWith(
        cb, rt, fp, batch, ubuf.get(), bindingsFor(tex, sensorTex, spatialTex, peakingMaskTex));
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
    // Luma bilateral ping-pong runs at the slot's native (full) resolution, since
    // luminance noise is high-frequency and the quarter-res reduction would average
    // it away before filtering (docs/adr/0046).
    make(ns.lumaA, ns.lumaART, fullSize);
    make(ns.lumaB, ns.lumaBRT, fullSize);
    ns.smoothness = -1.0f; // textures changed → force a re-run
    ns.strength = -1.0f;
    ns.lumaAmount = -1.0f;
    ns.lumaDetail = -1.0f;
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
    QRhiCommandBuffer* cb,
    int key,
    QRhiTexture* rawTex,
    float smoothness,
    float strength,
    float lumaAmount,
    float lumaDetail) {
    ensureNrResources();
    ensureNrSlot(key, rawTex->pixelSize());
    NrSlot& ns = nrSlot[key];

    if (ns.smoothness == smoothness && ns.strength == strength && ns.lumaAmount == lumaAmount
        && ns.lumaDetail == lumaDetail && ns.gen == generation)
        return ns.denoised.get(); // cached — reused for pan/zoom/other edits, no work
    ns.smoothness = smoothness;
    ns.strength = strength;
    ns.lumaAmount = lumaAmount;
    ns.lumaDetail = lumaDetail;

    const bool chroma = colorNoiseReductionActive(strength, smoothness);
    const bool luma = luminanceNoiseReductionActive(lumaAmount);

    // SRBs point at the live textures; rebuilt each run (only on param/texture
    // change), kept as members so they outlive command-buffer submission. The blur
    // legs read the chroma-context buffer, the bilateral legs the luma-context one.
    auto bindUbufTex = [&](std::unique_ptr<QRhiShaderResourceBindings>& dst,
                           QRhiBuffer* ub,
                           QRhiTexture* t) {
        dst.reset(rhi->newShaderResourceBindings());
        dst->setBindings(
            {QRhiShaderResourceBinding::uniformBuffer(
                 0,
                 QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                 ub),
             QRhiShaderResourceBinding::sampledTexture(
                 1, QRhiShaderResourceBinding::FragmentStage, t, sampler.get())});
        dst->create();
    };
    bindUbufTex(nrSrbExtract, nrUbuf.get(), rawTex);
    bindUbufTex(nrSrbBlurH, nrUbuf.get(), ns.chromaA.get());
    bindUbufTex(nrSrbBlurV, nrUbuf.get(), ns.chromaB.get());
    bindUbufTex(nrSrbBilateralH, nrLumaUbuf.get(), rawTex); // raw → Y, blurred to lumaA
    bindUbufTex(nrSrbBilateralV, nrLumaUbuf.get(), ns.lumaA.get());
    // Recombine reads raw + the blurred chroma ratio (chromaA) + the bilateral luma
    // (lumaB), and blends each half by its weight (docs/adr/0046).
    nrSrbRecombine.reset(rhi->newShaderResourceBindings());
    nrSrbRecombine->setBindings(
        {QRhiShaderResourceBinding::uniformBuffer(
             0,
             QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
             nrUbuf.get()),
         QRhiShaderResourceBinding::sampledTexture(
             1, QRhiShaderResourceBinding::FragmentStage, rawTex, sampler.get()),
         QRhiShaderResourceBinding::sampledTexture(
             2, QRhiShaderResourceBinding::FragmentStage, ns.chromaA.get(), sampler.get()),
         QRhiShaderResourceBinding::sampledTexture(
             3, QRhiShaderResourceBinding::FragmentStage, ns.lumaB.get(), sampler.get())});
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
        makePipe(nrPipeBilateralH, nrBilateralHFs, nrSrbBilateralH.get());
        makePipe(nrPipeBilateralV, nrBilateralVFs, nrSrbBilateralV.get());
        makePipe(nrPipeRecombine, nrRecombineFs, nrSrbRecombine.get());
    }

    const QMatrix4x4 cc = rhi->clipSpaceCorrMatrix();
    // On a Y-up framebuffer (OpenGL) the rendered NR targets are V-flipped vs the
    // uploaded image texture; the vertex shader mirrors the sampled V to cancel it.
    const qint32 flipV = rhi->isYUpInFramebuffer() ? 1 : 0;

    // Chroma-context buffer: drives the quarter-res blur and carries both recombine
    // blends (Strength and Amount), which the recombine pass reads from it.
    const float sigmaQuarter = colorNoiseReductionSigmaPx(smoothness) / 4.0f;
    NrUbuf nb{};
    std::memcpy(nb.clipCorr, cc.constData(), sizeof(nb.clipCorr));
    nb.invChroma[0] = 1.0f / float(ns.chromaA->pixelSize().width());
    nb.invChroma[1] = 1.0f / float(ns.chromaA->pixelSize().height());
    nb.sigma = sigmaQuarter;
    nb.radius = std::clamp(int(std::ceil(3.0f * sigmaQuarter)), 1, 64);
    nb.flipV = flipV;
    nb.strength = chroma ? colorNoiseReductionStrengthMix(strength) : 0.0f;
    nb.amount = luma ? luminanceNoiseReductionAmountMix(lumaAmount) : 0.0f;
    QRhiResourceUpdateBatch* b = rhi->nextResourceUpdateBatch();
    b->updateDynamicBuffer(nrUbuf.get(), 0, sizeof(NrUbuf), &nb);

    if (chroma) {
        nrPass(cb, ns.chromaART.get(), nrPipeExtract.get(), nrSrbExtract.get(), b); // raw → A
        b = nullptr;
        nrPass(cb, ns.chromaBRT.get(), nrPipeBlurH.get(), nrSrbBlurH.get(), nullptr); // A → B (H)
        nrPass(cb, ns.chromaART.get(), nrPipeBlurV.get(), nrSrbBlurV.get(), nullptr); // B → A (V)
    }

    if (luma) {
        // Luma-context buffer: bilateral over full-res Y. Spatial sigma is the fixed
        // reach in full-res px, halved on the half-res Preview slot ([0]); Detail
        // drives the perceptual range sigma (docs/adr/0046).
        const float spatialSigma = kLuminanceNoiseReductionSpatialSigmaPx
                                   * (key == 0 ? 0.5f : 1.0f);
        NrUbuf lb{};
        std::memcpy(lb.clipCorr, cc.constData(), sizeof(lb.clipCorr));
        lb.invChroma[0] = 1.0f / float(ns.lumaA->pixelSize().width());
        lb.invChroma[1] = 1.0f / float(ns.lumaA->pixelSize().height());
        lb.sigma = spatialSigma;
        lb.radius = std::clamp(int(std::ceil(3.0f * spatialSigma)), 1, 64);
        lb.flipV = flipV;
        lb.rangeSigma = luminanceNoiseReductionRangeSigma(lumaDetail);
        QRhiResourceUpdateBatch* lbatch = rhi->nextResourceUpdateBatch();
        lbatch->updateDynamicBuffer(nrLumaUbuf.get(), 0, sizeof(NrUbuf), &lb);
        nrPass(cb, ns.lumaART.get(), nrPipeBilateralH.get(), nrSrbBilateralH.get(), lbatch);
        nrPass(cb, ns.lumaBRT.get(), nrPipeBilateralV.get(), nrSrbBilateralV.get(), nullptr);
    }

    nrPass(cb, ns.denoisedRT.get(), nrPipeRecombine.get(), nrSrbRecombine.get(), b); // → full
    return ns.denoised.get();
}

// Colour NR is an exact no-op (the pre-pass is skipped, the main pass samples the
// raw slot) unless at least one half is active. The activation rules are
// single-sourced in NoiseReduction.h so the renderer and tests agree (docs/adr/0046).
static bool nrActive(const GlobalAdjustment& a) {
    return colorNoiseReductionActive(a.colorNoiseReduction, a.colorNoiseReductionSmoothness)
           || luminanceNoiseReductionActive(a.luminanceNoiseReduction);
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

// ── Focus Peaking (docs/adr/0058) ────────────────────────────────────────────

void RendererCore::ensureFocusPeakingSlot(QSize fullSize) {
    if (focusPeakingSlot.mask && focusPeakingSlot.size == fullSize
        && focusPeakingSlot.gen == generation)
        return;
    focusPeakingSlot.size = fullSize;

    if (!focusPeakingMaskRpDesc) {
        // peaking_edge.frag only ever writes a binary 0/1 value to the red
        // channel, so R8 is ~16x smaller than RGBA32F here — but a render-pass
        // descriptor built from an RGBA32F throwaway target (like nrRpDesc)
        // isn't safely reusable for an R8 target: backends that bake
        // attachment format into pipeline/render-pass objects at creation time
        // (Metal, notably) require a matching descriptor. Fall back to
        // RGBA32F, sharing nrRpDesc, if R8 render targets aren't supported.
        focusPeakingMaskFormat
            = rhi->isTextureFormatSupported(QRhiTexture::R8, QRhiTexture::RenderTarget)
            ? QRhiTexture::R8
            : QRhiTexture::RGBA32F;
        if (focusPeakingMaskFormat == QRhiTexture::RGBA32F) {
            ensureNrResources();
            focusPeakingMaskRpDesc.reset(); // fall through: reuse nrRpDesc directly below
        } else {
            std::unique_ptr<QRhiTexture> tmp(rhi->newTexture(
                focusPeakingMaskFormat, QSize(1, 1), 1, QRhiTexture::RenderTarget));
            tmp->create();
            QRhiColorAttachment att(tmp.get());
            std::unique_ptr<QRhiTextureRenderTarget> tmpRt(rhi->newTextureRenderTarget({att}));
            focusPeakingMaskRpDesc.reset(tmpRt->newCompatibleRenderPassDescriptor());
        }
    }
    QRhiRenderPassDescriptor* maskRpDesc
        = focusPeakingMaskRpDesc ? focusPeakingMaskRpDesc.get() : nrRpDesc.get();

    auto make = [&](std::unique_ptr<QRhiTexture>& tex,
                    std::unique_ptr<QRhiTextureRenderTarget>& rt,
                    QRhiTexture::Format format,
                    QRhiRenderPassDescriptor* rpDesc) {
        tex.reset(rhi->newTexture(
            format, fullSize, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        tex->create();
        QRhiColorAttachment att(tex.get());
        rt.reset(rhi->newTextureRenderTarget({att}));
        rt->setRenderPassDescriptor(rpDesc);
        rt->create();
    };
    make(focusPeakingSlot.source, focusPeakingSlot.sourceRT, QRhiTexture::RGBA32F, nrRpDesc.get());
    make(focusPeakingSlot.mask, focusPeakingSlot.maskRT, focusPeakingMaskFormat, maskRpDesc);
    focusPeakingSlot.gen = generation;
    peakingEdgeSrbTex = nullptr; // force the edge pass's srb to rebuild against the new source
    peakingEdgePipe.reset(); // must match maskRT's (possibly newly chosen) render-pass descriptor
}

QRhiTexture* RendererCore::ensureFocusPeakingMask(
    QRhiCommandBuffer* cb, const FrameParams& fp, FocusPeakingSensitivity sensitivity) {
    QRhiTexture* srcTex
        = imageTex[int(Slot::FullRes)].get(); // always FullRes (docs/adr/0058), never the preview
    if (!srcTex)
        return sensorClipDummyTex.get(); // caller already checked hasImage(Slot::FullRes)

    // Pass 1 below re-renders the *entire* develop pipeline, so the cache key
    // must cover essentially all of fp.adjustments, not just a few fields like
    // ensureDenoised's — GlobalAdjustment's defaulted operator== does that
    // correctly by construction. rawSrcTex is captured now, before
    // ensureDenoised/ensureSpatialContext below may reassign srcTex, since the
    // cache must invalidate on the *original* image changing, not on their
    // internal (already-cached) intermediate textures changing identity.
    QRhiTexture* const rawSrcTex = srcTex;
    FocusPeakingSlot& fps = focusPeakingSlot;
    if (fps.cacheValid && fps.mask && fps.cachedRawSrcTex == rawSrcTex
        && fps.cachedSensitivity == sensitivity && fps.cachedCropRect == fp.cropRect
        && fps.cachedGen == generation && fps.cachedAdjustments == fp.adjustments)
        return fps.mask.get(); // nothing relevant changed since last frame

    ensureNrResources(); // shares nrRpDesc + NrUbuf layout this pass reuses

    QRhiTexture* spatialTex = sensorClipDummyTex.get();
    if (nrActive(fp.adjustments)) {
        // Key 1 (FullRes) — shared with the on-screen path's own FullRes NR cache
        // when the user is also zoomed to 100%+, so the pre-pass runs once, not
        // twice, in that common case.
        srcTex = ensureDenoised(
            cb,
            1,
            srcTex,
            fp.adjustments.colorNoiseReductionSmoothness,
            fp.adjustments.colorNoiseReduction,
            fp.adjustments.luminanceNoiseReduction,
            fp.adjustments.luminanceNoiseReductionDetail);
    }
    if (spatialContextActive(fp.adjustments))
        spatialTex = ensureSpatialContext(cb, 1, srcTex);

    // imageTex[FullRes] is stored in *native* buffer layout (docs/adr/0029) —
    // image.vert remaps the oriented display-frame UV to native-buffer UV, it
    // never physically rotates the texture. The source/mask targets must be
    // sized (and the source pass's geometry driven) in the *oriented*,
    // *cropped* frame — the same frame renderToImage/renderClipSample already
    // use — or a 90°/270° Orientation renders the whole oriented image into a
    // native-shaped (transposed) target, producing a diagonal-flipped result.
    const QSize nativeSize = srcTex->pixelSize();
    int orientedW = nativeSize.width();
    int orientedH = nativeSize.height();
    if (orient::swapsAspect(fp.adjustments.orientation))
        std::swap(orientedW, orientedH);
    const QRectF& cr = fp.cropRect;
    const int cropW = std::max(1, int(cr.width() * orientedW + 0.5f));
    const int cropH = std::max(1, int(cr.height() * orientedH + 0.5f));
    ensureFocusPeakingSlot(QSize(cropW, cropH));

    // Pass 1: the full develop pipeline, overlays off, soft-proof-independent
    // (docs/adr/0058) — reuses image.frag/image.vert unmodified, just rendered
    // into an offscreen full-res target instead of the on-screen widget target.
    // transform/aspect are reset to a fixed, resolution-independent mapping
    // (matching renderToImage/renderClipSample) rather than inherited from the
    // caller: the on-screen fp.transform encodes the *on-screen viewport's*
    // current zoom/pan/window-aspect, which has nothing to do with how the
    // full-res source/mask targets are shaped.
    FrameParams fpSource = fp;
    fpSource.transform = QVector4D(1.0f, 1.0f, 0.0f, 0.0f);
    fpSource.aspect = float(orientedW) / float(orientedH);
    fpSource.useLut = false; // never the soft-proof LUT — peaking stays display-independent
    fpSource.gamutWarn = false;
    fpSource.clipHighlights = false;
    fpSource.clipShadows = false;
    fpSource.sensorClip = false;
    fpSource.focusPeaking = false; // no-op anyway (dummy mask below), but explicit
    fpSource.maskOverlay = -1;
    fpSource.curveInput = false;
    fpSource.wbInput = false;
    fpSource.histoRaw = false;
    fpSource.displayEncode = true;

    QRhiResourceUpdateBatch* sourceBatch = rhi->nextResourceUpdateBatch();
    // Follows bindingsFor's identity-cache idiom (the main on-screen pass):
    // only rebuild when the sampled textures actually changed identity.
    if (!focusPeakingSourceSrb || focusPeakingSourceSrbTex != srcTex
        || focusPeakingSourceSrbSpatialTex != spatialTex) {
        buildBindings(
            focusPeakingSourceSrb,
            focusPeakingSourceUbuf.get(),
            srcTex,
            sensorClipDummyTex.get(),
            spatialTex,
            sensorClipDummyTex.get()); // dummy peaking mask: this pass must not self-reference
        focusPeakingSourceSrbTex = srcTex;
        focusPeakingSourceSrbSpatialTex = spatialTex;
    }
    recordPassWith(
        cb,
        focusPeakingSlot.sourceRT.get(),
        fpSource,
        sourceBatch,
        focusPeakingSourceUbuf.get(),
        focusPeakingSourceSrb.get());

    // Pass 2: Sobel gradient magnitude + threshold (shaders/peaking_edge.frag).
    // Follows ensureSpatialContext/ensureDenoised's exact idiom: the srb is
    // rebuilt whenever the sampled texture changes, but the pipeline object is
    // created exactly once (its srb pointer only fixes the bindings *layout*).
    if (!peakingEdgeSrb || peakingEdgeSrbTex != focusPeakingSlot.source.get()) {
        peakingEdgeSrb.reset(rhi->newShaderResourceBindings());
        peakingEdgeSrb->setBindings(
            {QRhiShaderResourceBinding::uniformBuffer(
                 0,
                 QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                 peakingEdgeUbuf.get()),
             QRhiShaderResourceBinding::sampledTexture(
                 1,
                 QRhiShaderResourceBinding::FragmentStage,
                 focusPeakingSlot.source.get(),
                 sampler.get())});
        peakingEdgeSrb->create();
        peakingEdgeSrbTex = focusPeakingSlot.source.get();
    }
    if (!peakingEdgePipe) {
        peakingEdgePipe.reset(rhi->newGraphicsPipeline());
        peakingEdgePipe->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        peakingEdgePipe->setShaderStages(
            {{QRhiShaderStage::Vertex, nrVs}, {QRhiShaderStage::Fragment, peakingEdgeFs}});
        QRhiVertexInputLayout vl;
        vl.setBindings({{4 * sizeof(float)}});
        vl.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
        });
        peakingEdgePipe->setVertexInputLayout(vl);
        peakingEdgePipe->setShaderResourceBindings(peakingEdgeSrb.get());
        // maskRT's descriptor (R8, or RGBA32F where unsupported) — never
        // nrRpDesc unconditionally: this pipeline renders into maskRT, and a
        // pipeline's render-pass descriptor must match what it draws into.
        peakingEdgePipe->setRenderPassDescriptor(
            focusPeakingMaskRpDesc ? focusPeakingMaskRpDesc.get() : nrRpDesc.get());
        peakingEdgePipe->create();
    }

    NrUbuf nb{};
    const QMatrix4x4 cc = rhi->clipSpaceCorrMatrix();
    std::memcpy(nb.clipCorr, cc.constData(), sizeof(nb.clipCorr));
    const QSize srcSize = focusPeakingSlot.source->pixelSize();
    nb.invChroma[0] = 1.0f / float(srcSize.width());
    nb.invChroma[1] = 1.0f / float(srcSize.height());
    // Unlike the other nr.vert consumers, this pass's source is not a raw
    // backend-native NR-chain texture — it's focusPeakingSlot.source, rendered
    // by image.vert/image.frag, which already compensates Y-up backends via
    // kQuad's own baked-in V flip (see image.vert's header comment). Applying
    // nr.vert's usual flipV on top double-corrects, sampling row (1-v) instead
    // of v and mirroring the mask vertically (docs/adr/0058 follow-up).
    nb.flipV = 0;
    nb.strength = kFocusPeakingThresholds[int(sensitivity)];

    QRhiResourceUpdateBatch* edgeBatch = rhi->nextResourceUpdateBatch();
    edgeBatch->updateDynamicBuffer(peakingEdgeUbuf.get(), 0, sizeof(NrUbuf), &nb);
    nrPass(cb, focusPeakingSlot.maskRT.get(), peakingEdgePipe.get(), peakingEdgeSrb.get(), edgeBatch);

    fps.cachedRawSrcTex = rawSrcTex;
    fps.cachedSensitivity = sensitivity;
    fps.cachedCropRect = fp.cropRect;
    fps.cachedGen = generation;
    fps.cachedAdjustments = fp.adjustments;
    fps.cacheValid = true;
    return focusPeakingSlot.mask.get();
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
    QRhiTexture* peakingMaskTex = sensorClipDummyTex.get();
    if (nrActive(fp.adjustments)) {
        cb->resourceUpdate(batch); // apply uploads before the NR pre-passes
        tex = ensureDenoised(
            cb,
            slotIndex,
            tex,
            fp.adjustments.colorNoiseReductionSmoothness,
            fp.adjustments.colorNoiseReduction,
            fp.adjustments.luminanceNoiseReduction,
            fp.adjustments.luminanceNoiseReductionDetail);
        batch = rhi->nextResourceUpdateBatch(); // fresh batch for the main pass
    }
    if (spatialContextActive(fp.adjustments)) {
        cb->resourceUpdate(batch);
        spatialTex = ensureSpatialContext(cb, slotIndex, tex);
        batch = rhi->nextResourceUpdateBatch();
    }
    if (fp.focusPeaking && hasImage(Slot::FullRes)) {
        cb->resourceUpdate(batch);
        peakingMaskTex = ensureFocusPeakingMask(cb, fp, fp.focusPeakingSensitivity);
        batch = rhi->nextResourceUpdateBatch();
    }
    recordPass(cb, rt, tex, sensorTex, spatialTex, peakingMaskTex, fp, batch);
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
    QRhiTexture* peakingMaskTex = sensorClipDummyTex.get();
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
            fp.adjustments.colorNoiseReduction,
            fp.adjustments.luminanceNoiseReduction,
            fp.adjustments.luminanceNoiseReductionDetail);
        batch = rhi->nextResourceUpdateBatch();
    }
    if (spatialContextActive(fp.adjustments)) {
        const int spatialKey = slotIndex >= 0 ? slotIndex : 2;
        cb->resourceUpdate(batch);
        spatialTex = ensureSpatialContext(cb, spatialKey, tex);
        batch = rhi->nextResourceUpdateBatch();
    }
    if (fp.focusPeaking && hasImage(Slot::FullRes)) {
        cb->resourceUpdate(batch);
        peakingMaskTex = ensureFocusPeakingMask(cb, fp, fp.focusPeakingSensitivity);
        batch = rhi->nextResourceUpdateBatch();
    }
    recordPass(cb, rt.get(), tex, sensorTex, spatialTex, peakingMaskTex, fp, batch);

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
    // Focus Peaking never leaks into this readback (docs/adr/0058), same as
    // clipWarn/sensorClipWarn — always the dummy, regardless of fp.focusPeaking.
    QRhiTexture* peakingMaskTex = sensorClipDummyTex.get();
    if (nrActive(fp.adjustments)) {
        cb->resourceUpdate(batch);
        tex = ensureDenoised(
            cb,
            slotIndex,
            tex,
            fp.adjustments.colorNoiseReductionSmoothness,
            fp.adjustments.colorNoiseReduction,
            fp.adjustments.luminanceNoiseReduction,
            fp.adjustments.luminanceNoiseReductionDetail);
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
        || t->srbSpatialTex != spatialTex || t->srbPeakingMaskTex != peakingMaskTex
        || t->srbGeneration != generation) {
        buildBindings(t->srb, t->ubuf.get(), tex, sensorTex, spatialTex, peakingMaskTex);
        t->srbImageTex = tex;
        t->srbSensorTex = sensorTex;
        t->srbSpatialTex = spatialTex;
        t->srbPeakingMaskTex = peakingMaskTex;
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
