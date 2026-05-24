#include "ImageViewport.h"
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QCoreApplication>
#include <QPaintEvent>
#include <QOpenGLFramebufferObject>
#include <QColorSpace>
#include <QPainterPath>
#include <cmath>

static QVector4D cropUniform(const QRectF& cr) {
    return {float(cr.left()), float(cr.top()),
            float(cr.right()), float(cr.bottom())};
}

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
}

ImageViewport::~ImageViewport() {
    makeCurrent();
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
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

    reloadShaders();
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

void ImageViewport::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

// ── Rendering ─────────────────────────────────────────────────────────────────

float ImageViewport::displayAspect() const {
    if (!hasImage || cropMode || showOriginal)
        return imageAspect;
    const QRectF& cr = params.cropRect;
    return imageAspect * float(cr.width() / cr.height());
}

QOpenGLTexture* ImageViewport::activeTexture() const {
    if (hasFullRes && fullResTex && zoom >= kFullResZoomThreshold)
        return fullResTex.get();
    return previewTex.get();
}

void ImageViewport::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);
    auto* tex = activeTexture();
    if (!hasImage || !shader || !tex) return;

    shader->bind();
    tex->bind(0);
    shader->setUniformValue("uTexture", 0);

    const float viewportAspect = float(width()) / float(height());
    const float sx = zoom * (displayAspect() / viewportAspect);
    const float sy = zoom;
    shader->setUniformValue("uTransform", QVector4D(sx, sy, pan.x(), pan.y()));

    const AdjustmentParams& p = showOriginal ? AdjustmentParams{} : params;
    // While editing, show full image + overlay; after Enter, apply committed crop
    const QRectF crop = cropMode ? QRectF(0, 0, 1, 1) : p.cropRect;
    shader->setUniformValue("uCropRect",    cropUniform(crop));
    shader->setUniformValue("uAspect",      imageAspect);
    shader->setUniformValue("uRotation",    p.rotation);
    shader->setUniformValue("uExposure",    p.exposure);
    shader->setUniformValue("uContrast",    p.contrast    / 100.0f);
    shader->setUniformValue("uHighlights",  p.highlights  / 100.0f);
    shader->setUniformValue("uShadows",     p.shadows     / 100.0f);
    shader->setUniformValue("uWhites",      p.whites      / 100.0f);
    shader->setUniformValue("uBlacks",      p.blacks      / 100.0f);
    shader->setUniformValue("uTemperature", p.temperature);
    shader->setUniformValue("uTint",        p.tint        / 100.0f);
    shader->setUniformValue("uSaturation",  p.saturation  / 100.0f);
    shader->setUniformValue("uVibrance",    p.vibrance    / 100.0f);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    tex->release();
    shader->release();
}

void ImageViewport::paintEvent(QPaintEvent* e) {
    QOpenGLWidget::paintEvent(e);   // calls paintGL
    if (cropMode && hasImage) {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        drawCropOverlay(p);
    }
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

// ── Crop overlay ──────────────────────────────────────────────────────────────

// Handle positions in order: TL, TC, TR, MR, BR, BC, BL, ML
QPointF ImageViewport::handlePos(int i) const {
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
    // Check handles first
    for (int i = 0; i < kHandleCount; ++i) {
        QPointF d = pos - handlePos(i);
        if (d.x()*d.x() + d.y()*d.y() < kHandleRadius * kHandleRadius * 4)
            return i;
    }
    // Inside rect = move
    QPointF uv = viewportToTextureUV(pos);
    if (activeCrop.contains(uv))
        return -1;
    // Outside = (reserved for rotate, not yet implemented)
    return -2;
}

void ImageViewport::applyCropDrag(QPointF viewportPos) {
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
    const float l = float(activeCrop.left());
    const float r = float(activeCrop.right());
    const float t = float(activeCrop.top());
    const float b = float(activeCrop.bottom());
    const QPolygonF cropPoly{
        textureUVToViewport(l, t),
        textureUVToViewport(r, t),
        textureUVToViewport(r, b),
        textureUVToViewport(l, b),
    };

    // Darken everything outside the (possibly rotated) crop quad
    QPainterPath outside;
    outside.addRect(QRectF(0, 0, width(), height()));
    QPainterPath inside;
    inside.addPolygon(cropPoly);
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 140));

    // Rule-of-thirds grid (interpolate between opposite edges of the crop quad)
    p.setPen(QPen(QColor(255, 255, 255, 60), 0.5));
    for (int i = 1; i < 3; ++i) {
        const qreal t = i / 3.0;
        p.drawLine(cropPoly[0] + (cropPoly[1] - cropPoly[0]) * t,
                   cropPoly[3] + (cropPoly[2] - cropPoly[3]) * t);
        p.drawLine(cropPoly[0] + (cropPoly[3] - cropPoly[0]) * t,
                   cropPoly[1] + (cropPoly[2] - cropPoly[1]) * t);
    }

    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(cropPoly);

    // Handles
    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::white);
    for (int i = 0; i < kHandleCount; ++i) {
        QPointF hp = handlePos(i);
        p.drawRect(QRectF(hp.x() - kHandleRadius, hp.y() - kHandleRadius,
                          kHandleRadius * 2, kHandleRadius * 2));
    }
}

// ── Public setters ────────────────────────────────────────────────────────────

void ImageViewport::setImage(const ImageBuffer& buf) {
    imageAspect = buf.valid() ? float(buf.width) / float(buf.height) : 1.0f;
    makeCurrent();
    uploadTexture(buf, previewTex);
    hasImage  = buf.valid();
    hasFullRes = false;
    zoom = 1.0f;
    pan  = {0, 0};
    doneCurrent();
    update();
}

void ImageViewport::setFullResImage(const ImageBuffer& buf) {
    makeCurrent();
    uploadTexture(buf, fullResTex);
    hasFullRes = buf.valid();
    doneCurrent();
    if (zoom >= kFullResZoomThreshold)
        update();
}

void ImageViewport::setAdjustments(const AdjustmentParams& p) {
    params = p;
    if (!cropMode)
        activeCrop = p.cropRect;
    update();
}

void ImageViewport::uploadTexture(const ImageBuffer& buf,
                                   std::unique_ptr<QOpenGLTexture>& target) {
    target = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    target->setSize(buf.width, buf.height);
    target->setFormat(QOpenGLTexture::RGB32F);
    target->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
    target->allocateStorage();
    target->setData(QOpenGLTexture::RGB, QOpenGLTexture::Float32, buf.data.data());
}

// ── Offscreen export render ───────────────────────────────────────────────────

QImage ImageViewport::renderToImage(const ImageBuffer& buf,
                                     const AdjustmentParams& p,
                                     int outW, int outH)
{
    makeCurrent();

    // Temporary texture for the full-res buffer
    QOpenGLTexture exportTex(QOpenGLTexture::Target2D);
    exportTex.setSize(buf.width, buf.height);
    exportTex.setFormat(QOpenGLTexture::RGB32F);
    exportTex.setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
    exportTex.allocateStorage();
    exportTex.setData(QOpenGLTexture::RGB, QOpenGLTexture::Float32, buf.data.data());

    const QRectF& cr = p.cropRect;
    const int cropW = qMax(1, int(cr.width()  * buf.width  + 0.5f));
    const int cropH = qMax(1, int(cr.height() * buf.height + 0.5f));

    // Offscreen FBO at cropped pixel size
    QOpenGLFramebufferObjectFormat fboFmt;
    fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fboFmt.setInternalTextureFormat(GL_RGBA8);
    QOpenGLFramebufferObject fbo(cropW, cropH, fboFmt);
    fbo.bind();

    glViewport(0, 0, cropW, cropH);
    glClear(GL_COLOR_BUFFER_BIT);

    shader->bind();
    exportTex.bind(0);
    shader->setUniformValue("uTexture", 0);

    // Identity transform: crop region fills the FBO
    shader->setUniformValue("uTransform", QVector4D(1.0f, 1.0f, 0.0f, 0.0f));
    const float texAspect = float(buf.width) / float(buf.height);
    shader->setUniformValue("uCropRect",    cropUniform(cr));
    shader->setUniformValue("uAspect",      texAspect);
    shader->setUniformValue("uRotation",    p.rotation);
    shader->setUniformValue("uExposure",    p.exposure);
    shader->setUniformValue("uContrast",    p.contrast    / 100.0f);
    shader->setUniformValue("uHighlights",  p.highlights  / 100.0f);
    shader->setUniformValue("uShadows",     p.shadows     / 100.0f);
    shader->setUniformValue("uWhites",      p.whites      / 100.0f);
    shader->setUniformValue("uBlacks",      p.blacks      / 100.0f);
    shader->setUniformValue("uTemperature", p.temperature);
    shader->setUniformValue("uTint",        p.tint        / 100.0f);
    shader->setUniformValue("uSaturation",  p.saturation  / 100.0f);
    shader->setUniformValue("uVibrance",    p.vibrance    / 100.0f);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    exportTex.release();
    shader->release();

    // Read back — toImage() returns ARGB32 with Y=0 at top (Qt flips automatically)
    QImage result = fbo.toImage();

    fbo.release();
    glViewport(0, 0, width(), height());
    doneCurrent();

    result = result.convertToFormat(QImage::Format_RGB888);

    // Scale to requested output dimensions
    if (result.width() != outW || result.height() != outH)
        result = result.scaled(outW, outH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    result.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return result;
}

// ── Input events ──────────────────────────────────────────────────────────────

void ImageViewport::wheelEvent(QWheelEvent* e) {
    const float factor = e->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
    const float newZoom = qBound(0.05f, zoom * factor, 32.0f);
    if (newZoom >= kFullResZoomThreshold && zoom < kFullResZoomThreshold && !hasFullRes)
        emit fullResNeeded();
    zoom = newZoom;
    update();
}

void ImageViewport::mousePressEvent(QMouseEvent* e) {
    if (cropMode && e->button() == Qt::LeftButton) {
        cropDragHandle    = hitTest(e->position());
        cropDragStart     = e->position();
        cropDragStartRect = activeCrop;
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
            update();
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (cropMode) {
            cropMode = false;
            // Commit crop back into params and notify
            params.cropRect = activeCrop;
            emit cropCommitted(params.cropRect);
            update();
        }
        break;
    case Qt::Key_Escape:
        if (cropMode) {
            cropMode   = false;
            activeCrop = cancelCrop;
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
