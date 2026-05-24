#include "ImageViewport.h"
#include <QWheelEvent>
#include <QMouseEvent>
#include <QCoreApplication>

static const float kQuad[] = {
    // pos (NDC)   uv
    -1, -1,        0, 1,
     1, -1,        1, 1,
    -1,  1,        0, 0,
     1,  1,        1, 0,
};

ImageViewport::ImageViewport(QWidget* parent)
    : QOpenGLWidget(parent) {}

ImageViewport::~ImageViewport() {
    makeCurrent();
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    doneCurrent();
}

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
        !prog->link()) {
        return; // keep old shader if reload fails
    }
    shader = std::move(prog);
}

void ImageViewport::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void ImageViewport::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);
    if (!hasImage || !shader || !texture) return;

    shader->bind();
    texture->bind(0);
    shader->setUniformValue("uTexture", 0);

    float aspect    = float(width())  / float(height());
    float imgAspect = float(texture->width()) / float(texture->height());
    float sx = zoom * (imgAspect / aspect);
    float sy = zoom;

    shader->setUniformValue("uTransform", QVector4D(sx, sy, pan.x(), pan.y()));

    shader->setUniformValue("uExposure",    params.exposure);
    shader->setUniformValue("uContrast",    params.contrast    / 100.0f);
    shader->setUniformValue("uHighlights",  params.highlights  / 100.0f);
    shader->setUniformValue("uShadows",     params.shadows     / 100.0f);
    shader->setUniformValue("uWhites",      params.whites      / 100.0f);
    shader->setUniformValue("uBlacks",      params.blacks      / 100.0f);
    shader->setUniformValue("uTemperature", params.temperature / 100.0f);
    shader->setUniformValue("uTint",        params.tint        / 100.0f);
    shader->setUniformValue("uSaturation",  params.saturation  / 100.0f);
    shader->setUniformValue("uVibrance",    params.vibrance    / 100.0f);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    texture->release();
    shader->release();
}

void ImageViewport::setImage(const ImageBuffer& buf) {
    makeCurrent();
    uploadTexture(buf);
    hasImage = buf.valid();
    zoom = 1.0f;
    pan  = {0, 0};
    doneCurrent();
    update();
}

void ImageViewport::setAdjustments(const AdjustmentParams& p) {
    params = p;
    update();
}

void ImageViewport::uploadTexture(const ImageBuffer& buf) {
    texture = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture->setSize(buf.width, buf.height);
    texture->setFormat(QOpenGLTexture::RGB32F);
    texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
    texture->allocateStorage();
    texture->setData(QOpenGLTexture::RGB, QOpenGLTexture::Float32, buf.data.data());
}

void ImageViewport::wheelEvent(QWheelEvent* e) {
    const float factor = e->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
    zoom = qBound(0.05f, zoom * factor, 32.0f);
    update();
}

void ImageViewport::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton ||
        (e->button() == Qt::LeftButton && e->modifiers() & Qt::AltModifier)) {
        dragging  = true;
        dragStart = e->position();
    }
}

void ImageViewport::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging) return;
    QPointF delta = e->position() - dragStart;
    pan.setX(pan.x() + float(delta.x()) / width()  * 2.0f);
    pan.setY(pan.y() - float(delta.y()) / height() * 2.0f);
    dragStart = e->position();
    update();
}

void ImageViewport::mouseReleaseEvent(QMouseEvent*) {
    dragging = false;
}
