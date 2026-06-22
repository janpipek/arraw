#pragma once
#include "BasicTone.h"
#include "ColorManagement.h"
#include "ImagePipeline.h"
#include <array>
#include <cstddef>
#include <memory>
#include <rhi/qrhi.h>
#include <vector>
#include <QImage>

// std140 mirror of the uniform block in shaders/image.vert and image.frag —
// the three definitions must stay identical (ADR 0006).
struct Ubuf {
    float clipCorr[16];  // QRhi::clipSpaceCorrMatrix(), column-major
    float transform[4];  // (scaleX, scaleY, panX, panY)
    float cropRect[4];   // UV bounds: (left, top, right, bottom)
    float effectRect[4]; // final adjustment crop, including while editing Crop
    float hslHue[8];     // -1..+1 per range (vec4[2] in the shader)
    float hslSat[8];
    float hslLum[8];
    float rotation; // degrees
    float aspect;
    float exposure; // EV stops
    float contrast; // -0.2..+0.2
    float highlights;
    float shadows;
    float whites;
    float blacks;
    float wbGainR; // white-balance per-channel gain (docs/adr/0025), 5500K/tint0 = 1
    float wbGainG;
    float wbGainB;
    float saturation;       // -1..+1
    float vibrance;         // -1..+1
    float postCropVignetteAmount;   // -2..+2 EV at maximum falloff
    float postCropVignetteMidpoint; // 0..1
    float postCropVignetteFeather;  // 0..1
    float grainAmount;      // encoded-value standard deviation, 0..0.08
    float grainSize;        // grain diameter as a fraction of crop long edge
    float grainRoughness;   // 0..1
    quint32 grainSeed;      // deterministic per-image seed
    qint32 useLut;
    qint32 gamutWarn;
    qint32 baseLook;
    qint32 displayEncode;
    qint32 curveInput;
    qint32 hslActive;
    qint32 wbInput;
    qint32 clipWarn; // clipping overlay bits: 1 = highlights, 2 = shadows
    // Local adjustments (docs/adr/0010), 16-mask cap. Flat floats here ↔ vec4[16]
    // arrays in the shaders (tight std140 packing, like hslHue). Layout:
    //   laGeom  = Linear (p0.x, p0.y, p1.x, p1.y) | Radial (cx, cy, rx, ry)
    //   laGeom2 = Radial (angle, feather, invert, spare); unused for Linear
    //   laTone  = legacy tone packing (actual tone comes from LUT row, docs/adr/0031)
    //   laTone2 = (legacy whites, legacy blacks, wbGainR, wbGainG)
    //   laColor = (saturation, vibrance, maskType, wbGainB)  maskType 0=Linear 1=Radial
    float laGeom[64];
    float laTone[64];
    float laTone2[64];
    float laColor[64];
    float laGeom2[64];
    qint32 numLocalAdj;
    qint32 histoRaw;           // 1: emit pre-clamp sRGB-linear for overflow histogram
    qint32 orientQuarterTurns; // coarse Orientation (docs/adr/0028); was pad_
    qint32 orientMirrored;     // 1 = horizontal mirror; was pad_
};

static_assert(sizeof(Ubuf) == 1616);
static_assert(offsetof(Ubuf, effectRect) == 96);
static_assert(offsetof(Ubuf, grainSeed) == 284);
static_assert(offsetof(Ubuf, laGeom) == 320);
static_assert(offsetof(Ubuf, numLocalAdj) == 1600);

// The one place the shader pipeline is recorded (ADR 0006): the widget's
// on-screen pass, the export render, and the histogram samples all go
// through record()/renderOffscreen(). Owns every RHI resource — buffers,
// samplers, LUT and image textures, pipelines per render-pass format.
//
// setImage/setCurveLut/setDisplayLut may be called before initialize():
// data is held pending and uploaded with the next recorded pass.
class RendererCore {
public:
    enum class Slot { Preview, FullRes };

    // What one pass renders. Geometry (rotation) and the adjustment uniforms
    // come from `adjustments`; the rest is per-consumer framing.
    struct FrameParams {
        QVector4D transform{1, 1, 0, 0};
        QRectF cropRect{0, 0, 1, 1};
        float aspect = 1.0f;
        bool baseLook = false;
        bool displayEncode = true;
        bool curveInput = false;
        bool wbInput = false;
        bool useLut = false;
        bool gamutWarn = false;
        bool clipHighlights = false; // sRGB-relative clipping overlay (docs/adr/0009)
        bool clipShadows = false;
        bool histoRaw = false; // emit pre-clamp sRGB-linear for overflow histogram
        GlobalAdjustment adjustments;
    };

    void initialize(QRhi* rhi); // idempotent; re-creates on a new QRhi
    void release();

    bool ready() const { return rhi != nullptr; }

    void setImage(Slot slot, const ImageBuffer& buf); // invalid buf clears
    bool hasImage(Slot slot) const;

    void setCurveLut(const std::array<float, 256 * 4>& rgba);
    void setDisplayLut(const DisplayLut& lut);

    // Record one pass into rt (a frame must be active on cb), sampling slot.
    void record(QRhiCommandBuffer* cb, QRhiRenderTarget* rt, Slot slot, const FrameParams& fp);
    // Clear-only pass for frames with no image yet.
    void clear(QRhiCommandBuffer* cb, QRhiRenderTarget* rt);

    // Offscreen render in a frame of its own, with synchronous readback.
    // RGBA32F → Format_RGBX32FPx4 (linear working space), RGBA8 → Format_RGBA8888.
    QImage renderOffscreen(Slot slot, const FrameParams& fp, QSize size, QRhiTexture::Format fmt);
    QImage renderOffscreen(
        const ImageBuffer& buf, const FrameParams& fp, QSize size, QRhiTexture::Format fmt);

private:
    struct PendingImage {
        QByteArray rgba; // RGB float buffer expanded to RGBA
        QSize size;
    };

    static QByteArray expandToRgba(const ImageBuffer& buf);
    void prepareToneLut(const GlobalAdjustment& adjustment);
    void flushPendingUploads(QRhiResourceUpdateBatch* batch);
    void recordPass(
        QRhiCommandBuffer* cb,
        QRhiRenderTarget* rt,
        QRhiTexture* imageTex,
        const FrameParams& fp,
        QRhiResourceUpdateBatch* batch);
    QImage renderOffscreenTex(
        int slotIndex,
        QRhiTexture* extTex,
        const FrameParams& fp,
        QSize size,
        QRhiTexture::Format fmt);
    QImage readbackToImage(const QRhiReadbackResult& rr) const;
    QRhiGraphicsPipeline* pipelineFor(QRhiRenderPassDescriptor* rpDesc);
    QRhiShaderResourceBindings* bindingsFor(QRhiTexture* imageTex);
    void fillUbuf(Ubuf& ub, const FrameParams& fp) const;

    QRhi* rhi = nullptr;

    std::unique_ptr<QRhiBuffer> vbuf;
    std::unique_ptr<QRhiBuffer> ubuf;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiTexture> toneLutTex;    // 256×17: global + local Basic Tone
    std::unique_ptr<QRhiTexture> curveLutTex;   // 256×1 RGBA32F (L R G B)
    std::unique_ptr<QRhiTexture> displayLutTex; // N³ RGBA32F (1×1×1 dummy when unused)
    std::unique_ptr<QRhiTexture> imageTex[2];   // indexed by Slot

    // One srb, rebuilt when the sampled image texture or a LUT texture object
    // changes; layout is constant so it stays pipeline-compatible.
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    QRhiTexture* srbImageTex = nullptr;
    int srbGeneration = -1;
    int generation = 0; // bumped whenever any texture is (re)created

    // Pipelines cached per render-pass format (widget RGBA8 / export RGBA32F /
    // histogram RGBA8 offscreen).
    std::vector<std::pair<QVector<quint32>, std::unique_ptr<QRhiGraphicsPipeline>>> pipelines;

    QShader vs, fs;

    bool needQuadUpload = true;
    // Transient upload for export's temporary full-res texture, consumed by
    // the next flushPendingUploads.
    QRhiTexture* extraUploadTex = nullptr;
    QByteArray extraUploadData;
    PendingImage pendingImage[2];
    bool pendingImageDirty[2] = {false, false};
    std::array<float, 256 * 4> pendingCurveLut{};
    bool curveLutDirty = false;
    tone::LutAtlas pendingToneLut;
    bool toneLutDirty = false;
    std::vector<std::array<float, 6>> toneLutSource; // tone fields the LUT was built from
    DisplayLut pendingDisplayLut;
    bool displayLutDirty = false;
};
