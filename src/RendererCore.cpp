#include "RendererCore.h"
#include <algorithm>
#include <cstring>
#include <variant>
#include <QFile>

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

static const QColor kClearColor = QColor::fromRgbF(0.15f, 0.15f, 0.15f);

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

    vbuf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    vbuf->create();
    needQuadUpload = true;

    ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Ubuf)));
    ubuf->create();

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
    srbGeneration = -1;
    imageTex[0].reset();
    imageTex[1].reset();
    displayLutTex.reset();
    curveLutTex.reset();
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

bool RendererCore::hasImage(Slot slot) const {
    const int i = int(slot);
    return pendingImageDirty[i] || imageTex[i];
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

QRhiShaderResourceBindings* RendererCore::bindingsFor(QRhiTexture* tex) {
    if (srb && srbImageTex == tex && srbGeneration == generation)
        return srb.get();

    srb.reset(rhi->newShaderResourceBindings());
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, tex, sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, curveLutTex.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            3, QRhiShaderResourceBinding::FragmentStage, displayLutTex.get(), sampler.get()),
    });
    srb->create();
    srbImageTex = tex;
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
// Normalise the nine shared scalars to shader-uniform units. Used by both the
// global fill and each local adjustment, so the mapping lives exactly once.
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
    ub.temperature = a.temperature;
    ub.tint = g.tint;
    ub.saturation = g.saturation;
    ub.vibrance = g.vibrance;

    ub.useLut = fp.useLut ? 1 : 0;
    ub.gamutWarn = fp.gamutWarn ? 1 : 0;
    ub.baseLook = fp.baseLook ? 1 : 0;
    ub.displayEncode = fp.displayEncode ? 1 : 0;
    ub.curveInput = fp.curveInput ? 1 : 0;
    ub.hslActive = hslActive ? 1 : 0;
    ub.wbInput = fp.wbInput ? 1 : 0;
    ub.clipWarn = (fp.clipHighlights ? 1 : 0) | (fp.clipShadows ? 2 : 0);
    ub.histoRaw = fp.histoRaw ? 1 : 0;

    // Local adjustments (docs/adr/0010): pack into the parallel vec4 arrays,
    // honouring the 16-mask cap. Deltas use the same scaling as the global path;
    // local temperature is a relative -100..100 shift normalised like the global
    // Kelvin path (÷100 → the shader's tempShift t).
    const int n = std::min<int>(int(a.localAdjustments.size()), 16);
    ub.numLocalAdj = n;
    for (int i = 0; i < n; ++i) {
        const LocalAdjustment& la = a.localAdjustments[i];
        const SharedUniform d = toUniform(la);
        const int k = i * 4;

        float maskType = 0.0f; // 0 = Linear, 1 = Radial
        if (const auto* m = std::get_if<LinearMask>(&la.mask)) {
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
        ub.laTone2[k + 2] = la.temperature / 100.0f;
        ub.laTone2[k + 3] = d.tint;
        ub.laColor[k + 0] = d.saturation;
        ub.laColor[k + 1] = d.vibrance;
        ub.laColor[k + 2] = maskType;
        ub.laColor[k + 3] = 0.0f;
    }
}

// ── Pass recording — the single point the pipeline is drawn (ADR 0006) ───────

// `batch` must already contain the pending uploads (flushPendingUploads can
// recreate image textures, so the sampled texture is resolved only afterwards).
void RendererCore::recordPass(
    QRhiCommandBuffer* cb,
    QRhiRenderTarget* rt,
    QRhiTexture* tex,
    const FrameParams& fp,
    QRhiResourceUpdateBatch* batch) {
    Ubuf ub{};
    fillUbuf(ub, fp);
    batch->updateDynamicBuffer(ubuf.get(), 0, sizeof(Ubuf), &ub);

    QRhiShaderResourceBindings* bindings = bindingsFor(tex);
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

void RendererCore::record(
    QRhiCommandBuffer* cb, QRhiRenderTarget* rt, Slot slot, const FrameParams& fp) {
    if (!hasImage(slot)) {
        clear(cb, rt);
        return;
    }
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    flushPendingUploads(batch); // creates/recreates the slot's texture
    recordPass(cb, rt, imageTex[int(slot)].get(), fp, batch);
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
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    flushPendingUploads(batch);
    QRhiTexture* tex = slotIndex >= 0 ? imageTex[slotIndex].get() : extTex;
    recordPass(cb, rt.get(), tex, fp, batch);

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
