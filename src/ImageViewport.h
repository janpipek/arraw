#pragma once
#include "ImagePipeline.h"
#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QPointF>
#include <memory>

class ImageViewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit ImageViewport(QWidget* parent = nullptr);
    ~ImageViewport() override;

    void setImage(const ImageBuffer& preview);
    void setFullResImage(const ImageBuffer& fullRes);
    void setAdjustments(const AdjustmentParams& p);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;

private:
    void uploadTexture(const ImageBuffer& buf, std::unique_ptr<QOpenGLTexture>& target);
    void reloadShaders();
    QOpenGLTexture* activeTexture() const;

    std::unique_ptr<QOpenGLShaderProgram> shader;
    std::unique_ptr<QOpenGLTexture>       previewTex;
    std::unique_ptr<QOpenGLTexture>       fullResTex;

    AdjustmentParams params;

    float    zoom      = 1.0f;
    QPointF  pan       = {0, 0};
    QPointF  dragStart;
    bool     dragging  = false;
    bool     showOriginal = false;  // \ key held: render with no adjustments

    // Full-res is uploaded lazily when zoom crosses this threshold
    static constexpr float kFullResZoomThreshold = 1.5f;

    unsigned int vao = 0;
    unsigned int vbo = 0;

    bool hasImage   = false;
    bool hasFullRes = false;
};
