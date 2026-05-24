#include "ImageViewport.h"
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QCoreApplication>
#include <QPaintEvent>

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

    float aspect = float(width()) / float(height());
    float sx = zoom * (imageAspect / aspect);
    float sy = zoom;
    shader->setUniformValue("uTransform", QVector4D(sx, sy, pan.x(), pan.y()));

    const AdjustmentParams& p = showOriginal ? AdjustmentParams{} : params;
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

QPointF ImageViewport::uvToViewport(float u, float v) const {
    float aspect = float(width()) / float(height());
    float sx = zoom * (imageAspect / aspect);
    float sy = zoom;
    float ndcX = (u * 2.0f - 1.0f) * sx + float(pan.x());
    float ndcY = (1.0f - 2.0f * v) * sy + float(pan.y());
    return QPointF(double((ndcX + 1.0f) * width()  / 2.0f),
                   double((1.0f - ndcY) * height() / 2.0f));
}

QPointF ImageViewport::viewportToUV(QPointF pos) const {
    float aspect = float(width()) / float(height());
    float sx = zoom * (imageAspect / aspect);
    float sy = zoom;
    float ndcX = float(pos.x()) * 2.0f / width()  - 1.0f;
    float ndcY = 1.0f - float(pos.y()) * 2.0f / height();
    float u = ((ndcX - float(pan.x())) / sx + 1.0f) / 2.0f;
    float v = (1.0f - (ndcY - float(pan.y())) / sy) / 2.0f;
    return QPointF(double(u), double(v));
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
    return uvToViewport(float(uvs[i].x()), float(uvs[i].y()));
}

int ImageViewport::hitTest(QPointF pos) const {
    // Check handles first
    for (int i = 0; i < kHandleCount; ++i) {
        QPointF d = pos - handlePos(i);
        if (d.x()*d.x() + d.y()*d.y() < kHandleRadius * kHandleRadius * 4)
            return i;
    }
    // Inside rect = move
    QPointF uv = viewportToUV(pos);
    if (activeCrop.contains(uv))
        return -1;
    // Outside = (reserved for rotate, not yet implemented)
    return -2;
}

void ImageViewport::applyCropDrag(QPointF viewportPos) {
    QPointF uv = viewportToUV(viewportPos);
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
        QPointF startUV = viewportToUV(cropDragStart);
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
    QPointF tl = uvToViewport(float(activeCrop.left()),  float(activeCrop.top()));
    QPointF br = uvToViewport(float(activeCrop.right()), float(activeCrop.bottom()));
    QRectF cropVP(tl, br);

    // Darkened areas outside crop rect
    QColor shade(0, 0, 0, 140);
    p.fillRect(QRectF(0, 0, width(), cropVP.top()),               shade);
    p.fillRect(QRectF(0, cropVP.bottom(), width(), height()),      shade);
    p.fillRect(QRectF(0, cropVP.top(), cropVP.left(), cropVP.height()), shade);
    p.fillRect(QRectF(cropVP.right(), cropVP.top(),
                      width() - cropVP.right(), cropVP.height()),  shade);

    // Rule-of-thirds grid
    p.setPen(QPen(QColor(255, 255, 255, 60), 0.5));
    for (int i = 1; i < 3; ++i) {
        float x = float(cropVP.left() + cropVP.width()  * i / 3.0);
        float y = float(cropVP.top()  + cropVP.height() * i / 3.0);
        p.drawLine(QPointF(x, cropVP.top()), QPointF(x, cropVP.bottom()));
        p.drawLine(QPointF(cropVP.left(), y), QPointF(cropVP.right(), y));
    }

    // Crop border
    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(cropVP);

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
