#pragma once
#include "ImagePipeline.h"
#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <memory>

class ImageViewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit ImageViewport(QWidget* parent = nullptr);
    ~ImageViewport() override;

    void setImage(const ImageBuffer& preview);
    void setFullResImage(const ImageBuffer& fullRes);
    void setAdjustments(const AdjustmentParams& p);

    // Render buf through the full shader pipeline into an offscreen FBO.
    // Returns an sRGB QImage cropped to params.cropRect and scaled to outW×outH.
    QImage renderToImage(const ImageBuffer& buf, const AdjustmentParams& params,
                         int outW, int outH);

signals:
    void fullResNeeded();
    void cropCommitted(const QRectF& cropRect);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void paintEvent(QPaintEvent* e) override;

    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;

private:
    // Crop handles: TL, TC, TR, MR, BR, BC, BL, ML
    static constexpr int kHandleCount = 8;
    static constexpr float kHandleRadius = 6.0f;

    void uploadTexture(const ImageBuffer& buf, std::unique_ptr<QOpenGLTexture>& target);
    void reloadShaders();
    QOpenGLTexture* activeTexture() const;

    // Coordinate mapping between normalised UV [0..1] and viewport pixels
    QPointF uvToViewport(float u, float v) const;
    QPointF viewportToUV(QPointF pos) const;

    // Returns handle index 0-7, or -1 for "inside rect" (move), or -2 for "outside" (rotate)
    int hitTest(QPointF viewportPos) const;
    QPointF handlePos(int i) const;         // viewport coords of handle i
    void applyCropDrag(QPointF viewportPos);

    void drawCropOverlay(QPainter& p) const;

    // Aspect ratio of the region currently shown (accounts for committed crop).
    float displayAspect() const;

    // ── GL state ──────────────────────────────────────────────────────────
    std::unique_ptr<QOpenGLShaderProgram> shader;
    std::unique_ptr<QOpenGLTexture>       previewTex;
    std::unique_ptr<QOpenGLTexture>       fullResTex;
    unsigned int vao = 0;
    unsigned int vbo = 0;

    // ── Image state ───────────────────────────────────────────────────────
    AdjustmentParams params;
    float imageAspect = 1.0f;   // width/height of the full uncropped image
    bool  hasImage    = false;
    bool  hasFullRes  = false;

    static constexpr float kFullResZoomThreshold = 1.5f;

    // ── View state ────────────────────────────────────────────────────────
    float    zoom      = 1.0f;
    QPointF  pan       = {0, 0};
    QPointF  dragStart;
    bool     dragging     = false;
    bool     showOriginal = false;

    // ── Crop state ────────────────────────────────────────────────────────
    bool    cropMode       = false;
    QRectF  activeCrop     = {0, 0, 1, 1};   // rect being edited
    QRectF  cancelCrop     = {0, 0, 1, 1};   // restored on Escape
    int     cropDragHandle = -2;             // which handle is dragged
    QPointF cropDragStart;
    QRectF  cropDragStartRect;
};
