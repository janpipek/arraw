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
    void setStraightenActive(bool active);
    void setOriginalImageSize(int width, int height);
    void resetView();
    void setZoom(float value);
    void setPixelZoom(float value);
    float zoomFactor() const;
    float pixelZoom() const;
    bool hasKnownOriginalSize() const;

    // Render buf through the full shader pipeline into an offscreen FBO.
    // Returns an sRGB QImage cropped to params.cropRect and scaled to outW×outH.
    QImage renderToImage(const ImageBuffer& buf, const AdjustmentParams& params,
                         int outW, int outH);

signals:
    void fullResNeeded();
    void cropCommitted(const QRectF& cropRect);
    void zoomChanged(float pixelZoom);

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

    // Map texture UV ↔ viewport (matches vertex shader: image-centre rotation + fit)
    QPointF textureUVToViewport(float u, float v) const;
    QPointF viewportToTextureUV(QPointF pos) const;

    bool useViewportCrop() const;
    QRectF rotatedImageViewportBounds() const;
    QRectF textureCropToViewportBounds(const QRectF& texCrop) const;
    QRectF viewportCropToTextureCrop(const QRectF& vpCrop) const;

    // Returns handle index 0-7, or -1 for "inside rect" (move), or -2 for "outside" (rotate)
    int hitTest(QPointF viewportPos) const;
    QPointF handlePos(int i) const;
    void applyCropDrag(QPointF viewportPos);
    void applyCropDragViewport(QPointF viewportPos);

    void drawCropOverlay(QPainter& p) const;
    void drawAlignGrid(QPainter& p) const;
    bool shouldShowAlignGrid() const;

    // Aspect ratio of the region currently shown (accounts for committed crop).
    float displayAspect() const;
    float fitZoom() const;
    float displayOriginalPixelHeight() const;

    // ── GL state ──────────────────────────────────────────────────────────
    std::unique_ptr<QOpenGLShaderProgram> shader;
    std::unique_ptr<QOpenGLTexture>       previewTex;
    std::unique_ptr<QOpenGLTexture>       fullResTex;
    unsigned int vao = 0;
    unsigned int vbo = 0;

    // ── Image state ───────────────────────────────────────────────────────
    AdjustmentParams params;
    float imageAspect = 1.0f;   // width/height of the full uncropped image
    int   originalWidth = 0;
    int   originalHeight = 0;
    bool  hasImage    = false;
    bool  hasFullRes  = false;

    static constexpr float kFullResZoomThreshold = 1.5f;

    // ── View state ────────────────────────────────────────────────────────
    float    zoom      = 1.0f;
    QPointF  pan       = {0, 0};
    QPointF  dragStart;
    bool     dragging     = false;
    bool     showOriginal = false;
    bool     straightenActive = false;

    // ── Crop state ────────────────────────────────────────────────────────
    bool    cropMode       = false;
    QRectF  activeCrop     = {0, 0, 1, 1};   // texture UV rect being edited
    QRectF  cancelCrop     = {0, 0, 1, 1};
    QRectF  activeCropViewport;              // screen-aligned crop when straightening
    QRectF  cancelCropViewport;
    int     cropDragHandle = -2;             // which handle is dragged
    QPointF cropDragStart;
    QRectF  cropDragStartRect;
};
