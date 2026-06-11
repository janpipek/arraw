#include "ImageViewport.h"
#include "ImagePipeline.h"
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QCoreApplication>
#include <QPaintEvent>
#include <QOpenGLFramebufferObject>
#include <QColorSpace>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

static QVector4D cropUniform(const QRectF& cr) {
    return {float(cr.left()), float(cr.top()),
            float(cr.right()), float(cr.bottom())};
}

// CPU mirror of the rotation in image.vert (keep in sync). UV space is not
// square, so x is scaled by the image aspect before rotating to make the
// rotation isotropic in pixel space, then scaled back.
static QPointF rotateTexUV(float u, float v, float degrees, float aspect,
                           float cx, float cy) {
    float dx = (u - cx) * aspect;
    float dy = v - cy;
    const float rad = degrees * float(M_PI) / 180.0f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float rx = c * dx - s * dy;
    const float ry = s * dx + c * dy;
    return {rx / aspect + cx, ry + cy};
}

// Fullscreen quad, interleaved (x, y, u, v). V is flipped (bottom vertices get
// v=1) because GL's NDC Y axis points up while image row 0 is the top.
static const float kQuad[] = {
    -1, -1,   0, 1,
     1, -1,   1, 1,
    -1,  1,   0, 0,
     1,  1,   1, 0,
};

ImageViewport::ImageViewport(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);

    histoTimer.setSingleShot(true);
    histoTimer.setInterval(150);
    connect(&histoTimer, &QTimer::timeout, this, &ImageViewport::renderHistograms);
}

ImageViewport::~ImageViewport() {
    makeCurrent();
    if (vao)           glDeleteVertexArrays(1, &vao);
    if (vbo)           glDeleteBuffers(1, &vbo);
    if (curveLutTex)   glDeleteTextures(1, &curveLutTex);
    if (displayLutTex) glDeleteTextures(1, &displayLutTex);
    doneCurrent();
}

// ── GL setup ─────────────────────────────────────────────────────────────────

void ImageViewport::initializeGL() {
    QOpenGLFunctions_3_3_Core::initializeOpenGLFunctions();
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    // Curve LUT texture: 256×1 RGBA32F (channels: luma, red, green, blue)
    glGenTextures(1, &curveLutTex);
    glBindTexture(GL_TEXTURE_2D, curveLutTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    reloadShaders();
    uploadCurveLUT();

    // Textures handed over before the context existed (setImage called
    // before the widget was first shown).
    if (pendingPreview.valid()) {
        previewTex = createTexture(pendingPreview);
        pendingPreview = {};
    }
    if (pendingFullRes.valid()) {
        fullResTex = createTexture(pendingFullRes);
        pendingFullRes = {};
    }
}

void ImageViewport::reloadShaders() {
    auto prog = std::make_unique<QOpenGLShaderProgram>();
    const QString base = QCoreApplication::applicationDirPath() + "/shaders/";
    if (!prog->addShaderFromSourceFile(QOpenGLShader::Vertex,   base + "image.vert") ||
        !prog->addShaderFromSourceFile(QOpenGLShader::Fragment, base + "image.frag") ||
        !prog->link())
        return;
    shader = std::move(prog);
}

void ImageViewport::uploadCurveLUT() {
    const auto lumaLUT = computeCurveLUT(params.curveLuma.points);
    const auto redLUT  = computeCurveLUT(params.curveR.points);
    const auto grnLUT  = computeCurveLUT(params.curveG.points);
    const auto bluLUT  = computeCurveLUT(params.curveB.points);

    std::array<float, 256 * 4> rgba{};
    for (int i = 0; i < 256; ++i) {
        rgba[i*4+0] = lumaLUT[i];
        rgba[i*4+1] = redLUT[i];
        rgba[i*4+2] = grnLUT[i];
        rgba[i*4+3] = bluLUT[i];
    }

    glBindTexture(GL_TEXTURE_2D, curveLutTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, 1, 0, GL_RGBA, GL_FLOAT, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    curveLutDirty = false;
}

void ImageViewport::uploadDisplayLut() {
    if (pendingLut.valid()) {
        if (!displayLutTex)
            glGenTextures(1, &displayLutTex);
        glBindTexture(GL_TEXTURE_3D, displayLutTex);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA32F,
                     pendingLut.size, pendingLut.size, pendingLut.size,
                     0, GL_RGBA, GL_FLOAT, pendingLut.data.data());
        glBindTexture(GL_TEXTURE_3D, 0);
        pendingLut = {};
    }
    displayLutDirty = false;
}

void ImageViewport::setDisplayLut(const DisplayLut& lut) {
    if (!lut.valid()) { clearDisplayLut(); return; }
    pendingLut      = lut;
    displayLutDirty = true;
    useDisplayLut   = true;
    update();
}

void ImageViewport::clearDisplayLut() {
    useDisplayLut   = false;
    displayLutDirty = false;
    pendingLut      = {};
    update();
}

void ImageViewport::setGamutWarning(bool on) {
    if (gamutWarn == on)
        return;
    gamutWarn = on;
    update();
}

void ImageViewport::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    emit zoomChanged(pixelZoom());
}

// ── Rendering ─────────────────────────────────────────────────────────────────

float ImageViewport::displayAspect() const {
    if (!hasImage || cropMode || showOriginal)
        return imageAspect;
    const QRectF& cr = params.cropRect;
    return imageAspect * float(cr.width() / cr.height());
}

float ImageViewport::fitZoom() const {
    if (width() <= 0 || height() <= 0)
        return 1.0f;

    const float aspect = displayAspect();
    if (aspect <= 0.0f)
        return 1.0f;

    const float viewportAspect = float(width()) / float(height());
    return std::min(1.0f, viewportAspect / aspect);
}

float ImageViewport::displayOriginalPixelHeight() const {
    if (!hasKnownOriginalSize())
        return 0.0f;
    if (cropMode || showOriginal)
        return float(originalHeight);
    return float(originalHeight) * float(params.cropRect.height());
}

QOpenGLTexture* ImageViewport::activeTexture() const {
    if (hasFullRes && fullResTex && zoom >= kFullResZoomThreshold)
        return fullResTex.get();
    return previewTex.get();
}

static void setHslUniforms(QOpenGLShaderProgram* shader, const AdjustmentParams& p) {
    float hu[8], sa[8], lu[8];
    bool active = false;
    for (int i = 0; i < 8; ++i) {
        hu[i] = p.hslHue[i] / 100.0f;
        sa[i] = p.hslSat[i] / 100.0f;
        lu[i] = p.hslLum[i] / 100.0f;
        if (p.hslHue[i] != 0.0f || p.hslSat[i] != 0.0f || p.hslLum[i] != 0.0f)
            active = true;
    }
    shader->setUniformValueArray("uHslHue", hu, 8, 1);
    shader->setUniformValueArray("uHslSat", sa, 8, 1);
    shader->setUniformValueArray("uHslLum", lu, 8, 1);
    shader->setUniformValue("uHslActive", active);
}

// Uniforms shared between the live preview (paintGL) and export (renderToImage).
static void setAdjustmentUniforms(QOpenGLShaderProgram* shader, const AdjustmentParams& p) {
    shader->setUniformValue("uRotation",    p.rotation);
    shader->setUniformValue("uExposure",    p.exposure);
    shader->setUniformValue("uContrast",    p.contrast    / kToneSliderToUniform);
    shader->setUniformValue("uHighlights",  p.highlights  / kToneSliderToUniform);
    shader->setUniformValue("uShadows",     p.shadows     / kToneSliderToUniform);
    shader->setUniformValue("uWhites",      p.whites      / kToneSliderToUniform);
    shader->setUniformValue("uBlacks",      p.blacks      / kToneSliderToUniform);
    shader->setUniformValue("uTemperature", p.temperature);
    shader->setUniformValue("uTint",        p.tint        / 100.0f);
    shader->setUniformValue("uSaturation",  p.saturation  / 100.0f);
    shader->setUniformValue("uVibrance",    p.vibrance    / 100.0f);
    setHslUniforms(shader, p);
}

void ImageViewport::paintGL() {
    if (curveLutDirty)   uploadCurveLUT();
    if (displayLutDirty) uploadDisplayLut();

    glClear(GL_COLOR_BUFFER_BIT);
    auto* tex = activeTexture();
    if (!hasImage || !shader || !tex) return;

    shader->bind();
    tex->bind(0);
    shader->setUniformValue("uTexture", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, curveLutTex);
    shader->setUniformValue("uCurveLUT", 1);
    // uLut3D must always point at its own unit: a sampler3D left at the
    // default 0 would alias uTexture's unit and fail program validation.
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_3D, displayLutTex);
    shader->setUniformValue("uLut3D", 2);
    glActiveTexture(GL_TEXTURE0);
    shader->setUniformValue("uUseLut",    useDisplayLut);
    shader->setUniformValue("uGamutWarn", gamutWarn);

    const float viewportAspect = float(width()) / float(height());
    const float sx = zoom * (displayAspect() / viewportAspect);
    const float sy = zoom;
    shader->setUniformValue("uTransform", QVector4D(sx, sy, pan.x(), pan.y()));

    const AdjustmentParams& p = showOriginal ? AdjustmentParams{} : params;
    const QRectF crop = cropMode ? QRectF(0, 0, 1, 1) : p.cropRect;
    shader->setUniformValue("uCropRect",      cropUniform(crop));
    shader->setUniformValue("uAspect",        imageAspect);
    shader->setUniformValue("uBaseLook",      useBaseLook && !showOriginal);
    shader->setUniformValue("uDisplayEncode", true);
    shader->setUniformValue("uCurveInput",    false);
    setAdjustmentUniforms(shader.get(), p);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    tex->release();
    shader->release();
}

void ImageViewport::paintEvent(QPaintEvent* e) {
    QOpenGLWidget::paintEvent(e);   // calls paintGL
    if (!hasImage)
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (cropMode)
        drawCropOverlay(p);
    else if (shouldShowAlignGrid())
        drawAlignGrid(p);
}

// ── Coordinate mapping ────────────────────────────────────────────────────────

QPointF ImageViewport::textureUVToViewport(float u, float v) const {
    if (!showOriginal && std::abs(params.rotation) > 0.0001f) {
        const QPointF r = rotateTexUV(u, v, params.rotation, imageAspect, 0.5f, 0.5f);
        u = float(r.x());
        v = float(r.y());
    }

    const float viewportAspect = float(width()) / float(height());
    const float sx = zoom * (displayAspect() / viewportAspect);
    const float sy = zoom;
    const float ndcX = (u * 2.0f - 1.0f) * sx + float(pan.x());
    const float ndcY = (1.0f - 2.0f * v) * sy + float(pan.y());
    return {(ndcX + 1.0f) * width()  / 2.0f,
            (1.0f - ndcY) * height() / 2.0f};
}

QPointF ImageViewport::viewportToTextureUV(QPointF pos) const {
    const float viewportAspect = float(width()) / float(height());
    const float sx = zoom * (displayAspect() / viewportAspect);
    const float sy = zoom;
    const float ndcX = float(pos.x()) * 2.0f / width()  - 1.0f;
    const float ndcY = 1.0f - float(pos.y()) * 2.0f / height();
    float u = ((ndcX - float(pan.x())) / sx + 1.0f) / 2.0f;
    float v = (1.0f - (ndcY - float(pan.y())) / sy) / 2.0f;

    if (!showOriginal && std::abs(params.rotation) > 0.0001f) {
        const QPointF r = rotateTexUV(u, v, -params.rotation, imageAspect, 0.5f, 0.5f);
        u = float(r.x());
        v = float(r.y());
    }

    return {u, v};
}

// When the image is rotated, the crop rect the user drags must stay
// screen-axis-aligned, but an axis-aligned rect in texture UV would appear
// tilted. So while straightening, the crop is edited in viewport coordinates
// (activeCropViewport) and only converted back to UV on commit.
bool ImageViewport::useViewportCrop() const {
    return cropMode && std::abs(params.rotation) > 0.01f;
}

QRectF ImageViewport::rotatedImageViewportBounds() const {
    const QPointF corners[4] = {
        textureUVToViewport(0.f, 0.f), textureUVToViewport(1.f, 0.f),
        textureUVToViewport(1.f, 1.f), textureUVToViewport(0.f, 1.f),
    };
    QRectF bounds;
    for (const QPointF& c : corners)
        bounds |= QRectF(c, QSizeF(0, 0));
    return bounds.normalized();
}

QRectF ImageViewport::textureCropToViewportBounds(const QRectF& tex) const {
    const QPointF corners[4] = {
        textureUVToViewport(float(tex.left()),  float(tex.top())),
        textureUVToViewport(float(tex.right()), float(tex.top())),
        textureUVToViewport(float(tex.right()), float(tex.bottom())),
        textureUVToViewport(float(tex.left()),  float(tex.bottom())),
    };
    QRectF bounds;
    for (const QPointF& c : corners)
        bounds |= QRectF(c, QSizeF(0, 0));
    return bounds.normalized();
}

QRectF ImageViewport::viewportCropToTextureCrop(const QRectF& vp) const {
    const QPointF corners[4] = {
        viewportToTextureUV(vp.topLeft()),
        viewportToTextureUV(vp.topRight()),
        viewportToTextureUV(vp.bottomRight()),
        viewportToTextureUV(vp.bottomLeft()),
    };
    float minU = 1.f, minV = 1.f, maxU = 0.f, maxV = 0.f;
    for (const QPointF& c : corners) {
        minU = std::min(minU, float(c.x()));
        maxU = std::max(maxU, float(c.x()));
        minV = std::min(minV, float(c.y()));
        maxV = std::max(maxV, float(c.y()));
    }
    minU = std::clamp(minU, 0.f, 1.f);
    maxU = std::clamp(maxU, 0.f, 1.f);
    minV = std::clamp(minV, 0.f, 1.f);
    maxV = std::clamp(maxV, 0.f, 1.f);
    return {minU, minV, maxU - minU, maxV - minV};
}

// ── Straighten alignment grid (viewport-fixed horizontals / verticals) ────────

bool ImageViewport::shouldShowAlignGrid() const {
    if (!hasImage || cropMode || showOriginal)
        return false;
    if (straightenActive)
        return true;
    return std::abs(params.rotation) > 0.01f;
}

void ImageViewport::drawAlignGrid(QPainter& p) const {
    const int w = width();
    const int h = height();
    const int cx = w / 2;
    const int cy = h / 2;
    const int hSpacing = qMax(32, h / 10);
    const int vSpacing = qMax(32, w / 10);

    const QPen minorPen(QColor(255, 255, 255, 45), 0.5, Qt::DashLine);
    const QPen majorPen(QColor(255, 255, 255, 110), 1.0, Qt::DashLine);

    // Horizontal lines — primary guides for horizon / level alignment
    p.setPen(minorPen);
    for (int y = cy - hSpacing; y > 0; y -= hSpacing)
        p.drawLine(0, y, w, y);
    for (int y = cy + hSpacing; y < h; y += hSpacing)
        p.drawLine(0, y, w, y);

    p.setPen(majorPen);
    p.drawLine(0, cy, w, cy);

    // Vertical references
    p.setPen(minorPen);
    for (int x = cx - vSpacing; x > 0; x -= vSpacing)
        p.drawLine(x, 0, x, h);
    for (int x = cx + vSpacing; x < w; x += vSpacing)
        p.drawLine(x, 0, x, h);

    p.setPen(majorPen);
    p.drawLine(cx, 0, cx, h);
}

// ── Crop overlay ──────────────────────────────────────────────────────────────

// Handle positions in order: TL, TC, TR, MR, BR, BC, BL, ML
QPointF ImageViewport::handlePos(int i) const {
    if (useViewportCrop()) {
        const QRectF& r = activeCropViewport;
        const QPointF pts[kHandleCount] = {
            r.topLeft(), {r.center().x(), r.top()}, r.topRight(),
            {r.right(), r.center().y()},
            r.bottomRight(), {r.center().x(), r.bottom()}, r.bottomLeft(),
            {r.left(), r.center().y()},
        };
        return pts[i];
    }

    const float l = float(activeCrop.left());
    const float r = float(activeCrop.right());
    const float t = float(activeCrop.top());
    const float b = float(activeCrop.bottom());
    const float cx = (l + r) * 0.5f;
    const float cy = (t + b) * 0.5f;
    const QPointF uvs[kHandleCount] = {
        {l, t}, {cx, t}, {r, t},
        {r, cy},
        {r, b}, {cx, b}, {l, b},
        {l, cy}
    };
    return textureUVToViewport(float(uvs[i].x()), float(uvs[i].y()));
}

int ImageViewport::hitTest(QPointF pos) const {
    for (int i = 0; i < kHandleCount; ++i) {
        QPointF d = pos - handlePos(i);
        if (d.x()*d.x() + d.y()*d.y() < kHandleRadius * kHandleRadius * 4)
            return i;
    }
    if (useViewportCrop()) {
        if (activeCropViewport.contains(pos))
            return -1;
    } else if (activeCrop.contains(viewportToTextureUV(pos))) {
        return -1;
    }
    return -2;
}

void ImageViewport::applyCropDragViewport(QPointF pos) {
    QRectF r = cropDragStartRect;
    const qreal kMin = 24.0;
    const QRectF limits = rotatedImageViewportBounds();

    switch (cropDragHandle) {
    case 0: r.setTopLeft({std::min(pos.x(), r.right() - kMin),
                          std::min(pos.y(), r.bottom() - kMin)}); break;
    case 1: r.setTop(std::min(pos.y(), r.bottom() - kMin)); break;
    case 2: r.setTopRight({std::max(pos.x(), r.left() + kMin),
                           std::min(pos.y(), r.bottom() - kMin)}); break;
    case 3: r.setRight(std::max(pos.x(), r.left() + kMin)); break;
    case 4: r.setBottomRight({std::max(pos.x(), r.left() + kMin),
                              std::max(pos.y(), r.top() + kMin)}); break;
    case 5: r.setBottom(std::max(pos.y(), r.top() + kMin)); break;
    case 6: r.setBottomLeft({std::min(pos.x(), r.right() - kMin),
                             std::max(pos.y(), r.top() + kMin)}); break;
    case 7: r.setLeft(std::min(pos.x(), r.right() - kMin)); break;
    case -1:
        r = cropDragStartRect.translated(pos - cropDragStart);
        break;
    default:
        return;
    }

    activeCropViewport = r.intersected(limits).normalized();
    if (activeCropViewport.width() < kMin || activeCropViewport.height() < kMin)
        activeCropViewport = cropDragStartRect;
    update();
}

void ImageViewport::applyCropDrag(QPointF viewportPos) {
    if (useViewportCrop()) {
        applyCropDragViewport(viewportPos);
        return;
    }
    QPointF uv = viewportToTextureUV(viewportPos);
    float u = float(std::clamp(uv.x(), 0.0, 1.0));
    float v = float(std::clamp(uv.y(), 0.0, 1.0));

    QRectF r = cropDragStartRect;
    const float kMinSize = 0.02f;

    switch (cropDragHandle) {
    case 0: r.setTopLeft    ({std::min(u, float(r.right())  - kMinSize), std::min(v, float(r.bottom()) - kMinSize)}); break;
    case 1: r.setTop        (std::min(v, float(r.bottom())  - kMinSize)); break;
    case 2: r.setTopRight   ({std::max(u, float(r.left())   + kMinSize), std::min(v, float(r.bottom()) - kMinSize)}); break;
    case 3: r.setRight      (std::max(u, float(r.left())    + kMinSize)); break;
    case 4: r.setBottomRight({std::max(u, float(r.left())   + kMinSize), std::max(v, float(r.top())    + kMinSize)}); break;
    case 5: r.setBottom     (std::max(v, float(r.top())     + kMinSize)); break;
    case 6: r.setBottomLeft ({std::min(u, float(r.right())  - kMinSize), std::max(v, float(r.top())    + kMinSize)}); break;
    case 7: r.setLeft       (std::min(u, float(r.right())   - kMinSize)); break;
    case -1: { // move
        QPointF startUV = viewportToTextureUV(cropDragStart);
        float du = float(uv.x() - startUV.x());
        float dv = float(uv.y() - startUV.y());
        float w  = float(cropDragStartRect.width());
        float h  = float(cropDragStartRect.height());
        float nl = std::clamp(float(cropDragStartRect.left()) + du, 0.0f, 1.0f - w);
        float nt = std::clamp(float(cropDragStartRect.top())  + dv, 0.0f, 1.0f - h);
        r = QRectF(nl, nt, w, h);
        break;
    }
    default: return;
    }
    activeCrop = r;
    update();
}

void ImageViewport::drawCropOverlay(QPainter& p) const {
    const QRectF cropVP = useViewportCrop()
        ? activeCropViewport
        : QRectF(textureUVToViewport(float(activeCrop.left()), float(activeCrop.top())),
                 textureUVToViewport(float(activeCrop.right()), float(activeCrop.bottom())))
              .normalized();

    QPainterPath outside;
    outside.addRect(QRectF(0, 0, width(), height()));
    QPainterPath inside;
    inside.addRect(cropVP);
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 140));

    p.setPen(QPen(QColor(255, 255, 255, 60), 0.5));
    for (int i = 1; i < 3; ++i) {
        const qreal t = i / 3.0;
        const qreal x = cropVP.left() + cropVP.width() * t;
        const qreal y = cropVP.top() + cropVP.height() * t;
        p.drawLine(QPointF(x, cropVP.top()), QPointF(x, cropVP.bottom()));
        p.drawLine(QPointF(cropVP.left(), y), QPointF(cropVP.right(), y));
    }

    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(cropVP);

    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::white);
    for (int i = 0; i < kHandleCount; ++i) {
        QPointF hp = handlePos(i);
        p.drawRect(QRectF(hp.x() - kHandleRadius, hp.y() - kHandleRadius,
                          kHandleRadius * 2, kHandleRadius * 2));
    }
}

// ── Public setters ────────────────────────────────────────────────────────────

void ImageViewport::setImage(const ImageBuffer& buf, bool baseLook) {
    imageAspect = buf.valid() ? float(buf.width) / float(buf.height) : 1.0f;
    hasImage  = buf.valid();
    hasFullRes = false;
    useBaseLook = baseLook;
    if (context()) {
        makeCurrent();
        previewTex = createTexture(buf);
        doneCurrent();
    } else {
        // No GL context yet (widget not shown) — upload in initializeGL.
        pendingPreview = buf;
    }
    resetView();
    histoTimer.start();
    update();
}

void ImageViewport::setFullResImage(const ImageBuffer& buf) {
    if (buf.valid())
        setOriginalImageSize(buf.width, buf.height);
    hasFullRes = buf.valid();
    if (context()) {
        makeCurrent();
        fullResTex = createTexture(buf);
        doneCurrent();
    } else {
        pendingFullRes = buf;
    }
    if (zoom >= kFullResZoomThreshold)
        update();
}

void ImageViewport::setAdjustments(const AdjustmentParams& p) {
    if (p.curveLuma != params.curveLuma || p.curveR != params.curveR ||
        p.curveG != params.curveG || p.curveB != params.curveB)
        curveLutDirty = true;
    params = p;
    if (!cropMode)
        activeCrop = p.cropRect;
    if (hasImage)
        histoTimer.start();
    emit zoomChanged(pixelZoom());
    update();
}

void ImageViewport::setStraightenActive(bool active) {
    if (straightenActive == active)
        return;
    straightenActive = active;
    update();
}

void ImageViewport::setOriginalImageSize(int width, int height) {
    originalWidth = width;
    originalHeight = height;
    emit zoomChanged(pixelZoom());
}

void ImageViewport::resetView() {
    setZoom(fitZoom());
    pan = {0, 0};
    update();
}

void ImageViewport::setZoom(float value) {
    const float newZoom = qBound(0.05f, value, 32.0f);
    if (newZoom >= kFullResZoomThreshold && zoom < kFullResZoomThreshold && !hasFullRes)
        emit fullResNeeded();

    // Emit even when the zoom value is unchanged: callers (e.g. resetView
    // after an image load) rely on this to refresh the zoom label.
    if (std::abs(newZoom - zoom) < 0.0001f) {
        emit zoomChanged(pixelZoom());
        return;
    }

    zoom = newZoom;
    emit zoomChanged(pixelZoom());
    update();
}

void ImageViewport::setPixelZoom(float value) {
    const float displayHeight = displayOriginalPixelHeight();
    if (displayHeight <= 0.0f || height() <= 0)
        return;
    setZoom(value * displayHeight / float(height()));
}

float ImageViewport::zoomFactor() const {
    return zoom;
}

float ImageViewport::pixelZoom() const {
    const float displayHeight = displayOriginalPixelHeight();
    if (displayHeight <= 0.0f || height() <= 0)
        return 0.0f;
    return zoom * float(height()) / displayHeight;
}

bool ImageViewport::hasKnownOriginalSize() const {
    return originalWidth > 0 && originalHeight > 0;
}

std::unique_ptr<QOpenGLTexture> ImageViewport::createTexture(const ImageBuffer& buf) {
    auto tex = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    tex->setSize(buf.width, buf.height);
    tex->setFormat(QOpenGLTexture::RGB32F);
    tex->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
    tex->allocateStorage();
    tex->setData(QOpenGLTexture::RGB, QOpenGLTexture::Float32, buf.data.data());
    return tex;
}

// ── Offscreen export render ───────────────────────────────────────────────────

QImage ImageViewport::renderToImage(const ImageBuffer& buf,
                                     const AdjustmentParams& p,
                                     int outW, int outH)
{
    makeCurrent();

    // Temporary texture for the full-res buffer
    const auto exportTex = createTexture(buf);

    const QRectF& cr = p.cropRect;
    const int cropW = qMax(1, int(cr.width()  * buf.width  + 0.5f));
    const int cropH = qMax(1, int(cr.height() * buf.height + 0.5f));

    // Offscreen FBO at cropped pixel size. Float format: the readback stays in
    // linear working space; the output transform happens on the CPU (lcms2).
    QOpenGLFramebufferObjectFormat fboFmt;
    fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fboFmt.setInternalTextureFormat(GL_RGBA32F);
    QOpenGLFramebufferObject fbo(cropW, cropH, fboFmt);
    fbo.bind();

    glViewport(0, 0, cropW, cropH);
    glClear(GL_COLOR_BUFFER_BIT);

    // Ensure curve LUT is current (params may have changed since last paintGL)
    if (curveLutDirty) uploadCurveLUT();

    shader->bind();
    exportTex->bind(0);
    shader->setUniformValue("uTexture", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, curveLutTex);
    shader->setUniformValue("uCurveLUT", 1);
    glActiveTexture(GL_TEXTURE0);
    shader->setUniformValue("uLut3D",  2);      // unused (uDisplayEncode off), but
    shader->setUniformValue("uUseLut", false);  // must not alias a 2D sampler unit

    shader->setUniformValue("uTransform", QVector4D(1.0f, 1.0f, 0.0f, 0.0f));
    const float texAspect = float(buf.width) / float(buf.height);
    shader->setUniformValue("uCropRect",      cropUniform(cr));
    shader->setUniformValue("uAspect",        texAspect);
    shader->setUniformValue("uBaseLook",      true);
    shader->setUniformValue("uDisplayEncode", false);
    shader->setUniformValue("uCurveInput",    false);
    setAdjustmentUniforms(shader.get(), p);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    exportTex->release();
    shader->release();

    // Read back as linear working-space floats. GL rows are bottom-up, so
    // flip in place (Format_RGBX32FPx4 scanlines are tightly packed).
    QImage result(cropW, cropH, QImage::Format_RGBX32FPx4);
    glReadPixels(0, 0, cropW, cropH, GL_RGBA, GL_FLOAT, result.bits());

    fbo.release();
    glViewport(0, 0, width(), height());
    doneCurrent();

    const qsizetype stride = result.bytesPerLine();
    std::vector<uchar> tmp(stride);
    for (int y = 0; y < cropH / 2; ++y) {
        uchar* top = result.scanLine(y);
        uchar* bot = result.scanLine(cropH - 1 - y);
        memcpy(tmp.data(), top, stride);
        memcpy(top, bot, stride);
        memcpy(bot, tmp.data(), stride);
    }

    // Scale to requested output dimensions while still in linear light —
    // gamma-space scaling darkens fine detail.
    if (result.width() != outW || result.height() != outH)
        result = result.scaled(outW, outH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    return result;
}

// ── Histogram readback (docs/adr/0004) ────────────────────────────────────────

// Render the preview texture through the real shader twice into a small FBO —
// once with uCurveInput (pipeline stops after tone regions, gamma-encoded) and
// once full — and hand both samples to whoever bins them.
void ImageViewport::renderHistograms() {
    if (!hasImage || !shader || !previewTex)
        return;

    makeCurrent();
    if (curveLutDirty) uploadCurveLUT();

    const QRectF& cr = params.cropRect;
    const float aspect = imageAspect * float(cr.width() / cr.height());
    const int w = 256;
    const int h = std::clamp(int(w / aspect + 0.5f), 16, 1024);

    QOpenGLFramebufferObjectFormat fboFmt;
    fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fboFmt.setInternalTextureFormat(GL_RGBA8);
    QOpenGLFramebufferObject fbo(w, h, fboFmt);
    fbo.bind();
    glViewport(0, 0, w, h);

    shader->bind();
    previewTex->bind(0);
    shader->setUniformValue("uTexture", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, curveLutTex);
    shader->setUniformValue("uCurveLUT", 1);
    glActiveTexture(GL_TEXTURE0);
    shader->setUniformValue("uLut3D",  2);      // uUseLut is off, but the sampler
    shader->setUniformValue("uUseLut", false);  // must not alias a 2D sampler unit

    shader->setUniformValue("uTransform", QVector4D(1.0f, 1.0f, 0.0f, 0.0f));
    shader->setUniformValue("uCropRect",  cropUniform(cr));
    shader->setUniformValue("uAspect",    imageAspect);
    shader->setUniformValue("uBaseLook",  useBaseLook);
    // Display transform on, monitor/proof LUT off: the panel histogram shows
    // output-sRGB values regardless of soft-proofing.
    shader->setUniformValue("uDisplayEncode", true);
    setAdjustmentUniforms(shader.get(), params);
    glBindVertexArray(vao);

    shader->setUniformValue("uCurveInput", true);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const QImage curveInput = fbo.toImage();

    shader->setUniformValue("uCurveInput", false);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const QImage finalSample = fbo.toImage();

    previewTex->release();
    shader->release();
    fbo.release();
    glViewport(0, 0, width(), height());
    doneCurrent();

    emit histogramsReady(finalSample, curveInput);
}

// ── Input events ──────────────────────────────────────────────────────────────

void ImageViewport::wheelEvent(QWheelEvent* e) {
    const float factor = e->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
    setZoom(zoom * factor);
}

void ImageViewport::mousePressEvent(QMouseEvent* e) {
    if (cropMode && e->button() == Qt::LeftButton) {
        cropDragHandle    = hitTest(e->position());
        cropDragStart     = e->position();
        cropDragStartRect = useViewportCrop() ? activeCropViewport : activeCrop;
        return;
    }
    if (e->button() == Qt::MiddleButton ||
        (e->button() == Qt::LeftButton && e->modifiers() & Qt::AltModifier)) {
        dragging  = true;
        dragStart = e->position();
    }
}

void ImageViewport::mouseMoveEvent(QMouseEvent* e) {
    if (cropMode && (e->buttons() & Qt::LeftButton) && cropDragHandle > -2) {
        applyCropDrag(e->position());
        return;
    }
    if (!dragging) return;
    // pan is in NDC units (viewport spans -1..1), hence the ×2 / size.
    QPointF delta = e->position() - dragStart;
    pan.setX(pan.x() + float(delta.x()) / width()  * 2.0f);
    pan.setY(pan.y() - float(delta.y()) / height() * 2.0f);
    dragStart = e->position();
    update();
}

void ImageViewport::mouseReleaseEvent(QMouseEvent* e) {
    if (cropMode && e->button() == Qt::LeftButton) {
        cropDragHandle = -2;
        return;
    }
    dragging = false;
}

void ImageViewport::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) { QOpenGLWidget::keyPressEvent(e); return; }

    switch (e->key()) {
    case Qt::Key_Backslash:
        showOriginal = true;
        update();
        break;
    case Qt::Key_C:
        if (!cropMode) {
            cropMode   = true;
            cancelCrop = params.cropRect;
            activeCrop = params.cropRect;
            if (useViewportCrop()) {
                activeCropViewport = textureCropToViewportBounds(params.cropRect);
                cancelCropViewport = activeCropViewport;
            }
            update();
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (cropMode) {
            cropMode = false;
            if (std::abs(params.rotation) > 0.01f)
                activeCrop = viewportCropToTextureCrop(activeCropViewport);
            params.cropRect = activeCrop;
            emit cropCommitted(params.cropRect);
            emit zoomChanged(pixelZoom());
            update();
        }
        break;
    case Qt::Key_Escape:
        if (cropMode) {
            cropMode   = false;
            activeCrop = cancelCrop;
            if (std::abs(params.rotation) > 0.01f)
                activeCropViewport = cancelCropViewport;
            update();
        }
        break;
    default:
        QOpenGLWidget::keyPressEvent(e);
    }
}

void ImageViewport::keyReleaseEvent(QKeyEvent* e) {
    if (!e->isAutoRepeat() && e->key() == Qt::Key_Backslash) {
        showOriginal = false;
        update();
    } else {
        QOpenGLWidget::keyReleaseEvent(e);
    }
}
