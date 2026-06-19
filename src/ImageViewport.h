#pragma once
#include "ColorManagement.h"
#include "CropGeometry.h"
#include "ImagePipeline.h"
#include "RendererCore.h"
#include "Spot.h"
#include "ViewportGeometry.h"
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QRhiWidget>
#include <QTimer>

class ViewportOverlay;

/**
 * GPU-backed image view and on-image editing surface.
 *
 * ImageViewport displays the current preview/full-resolution textures through
 * RendererCore and handles gestures for crop, straighten, white balance, local
 * mask geometry, and spot handles. It owns interaction state and renderer
 * resources, but the durable develop state is committed back to DevelopSession
 * through MainWindow.
 */
class ImageViewport : public QRhiWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ImageViewport)
public:
    // Mutually-exclusive viewport tools. Crop edits a rectangle; Straighten
    // draws a level line; WhiteBalance samples a neutral pixel. Only one is
    // active at a time — the toolbar is a thin control surface over this.
    enum class ActiveTool { None, Crop, Straighten, WhiteBalance, LocalMask, SpotTool };
    Q_ENUM(ActiveTool)

    // Which part of a Spot the user is dragging in SpotTool mode.
    enum class SpotHandle { None, Destination, Source };

    explicit ImageViewport(QWidget* parent = nullptr);
    ~ImageViewport() override;

    void setImage(const ImageBuffer& preview, bool baseLookEnabled = false);
    void setFullResImage(const ImageBuffer& fullRes);
    void setAdjustments(const GlobalAdjustment& p);
    void setStraightenActive(bool active);

    // Constrain the crop being edited to a fixed aspect ratio. Reshapes the
    // current crop to the new ratio immediately (shrink-to-fit, centred). No
    // effect unless the crop tool is active. The constraint persists with the
    // crop (crs:CropConstrainAspectRatio); re-entering crop on a constrained
    // image re-locks to the stored rectangle's ratio.
    void setAspectLock(crop::AspectPreset preset, bool landscape);

    // The preset/orientation the active crop's lock corresponds to, for the
    // aspect menu to reflect a restored lock. Free when unlocked; matched=false
    // for a custom ratio with no named preset.
    crop::PresetMatch currentLockMatch() const;

    // Active-tool state machine. setActiveTool switches tools, committing any
    // pending edit of the tool being left (commit-on-leave); commitActiveTool
    // is setActiveTool(None); cancelActiveTool discards the pending edit (Esc).
    ActiveTool activeTool() const { return tool; }

    void setActiveTool(ActiveTool t);

    void commitActiveTool() { setActiveTool(ActiveTool::None); }

    void cancelActiveTool();
    void setOriginalImageSize(int width, int height);

    // Which local adjustment the LocalMask tool edits on the image (-1 = none).
    // Driven by the Masks panel selection.
    void setActiveLocalAdjustment(int index);

    // Spot-tool overlay: set the list the viewport draws and hit-tests.
    // Does not rebuild the spotted image buffer — that is MainWindow's job.
    void setSpots(const std::vector<Spot>& spots);

    bool spotToolMode() const { return tool == ActiveTool::SpotTool; }

    void resetView();
    void setZoom(float value);
    void setPixelZoom(float value);
    float zoomFactor() const;
    float pixelZoom() const;
    bool hasKnownOriginalSize() const;

    // True once the RHI exists (first frame after show) — required before
    // renderToImage can run.
    bool rendererReady() const { return core.ready(); }

    // Display LUT (soft-proofing / monitor profile). Affects preview only —
    // export renders with the display encode disabled. The upload happens
    // lazily with the next recorded pass, so these are safe to call before
    // the widget is first shown.
    void setDisplayLut(const DisplayLut& lut);
    void clearDisplayLut();
    void setGamutWarning(bool on);
    // Clipping overlay (docs/adr/0009): paint highlight clips red, shadow clips
    // blue on the on-screen preview. View state only — never exported.
    void setClipWarnings(bool highlights, bool shadows);

    // Render buf through the full shader pipeline into an offscreen target.
    // Returns a *linear working-space* float QImage (Format_RGBX32FPx4),
    // cropped to params.cropRect and scaled to outW×outH; the caller applies
    // the output transform (toOutputImage) before saving.
    QImage renderToImage(const ImageBuffer& buf, const GlobalAdjustment& params, int outW, int outH);

    // Render buf through the on-screen display path (sRGB encode, LUT off) with
    // the clipping overlay forced on — the export path forces it off, so this is
    // the testable entry for the overlay (docs/adr/0009). Returns a display-
    // encoded float QImage (Format_RGBX32FPx4) at the cropped pixel size.
    QImage renderClipSample(
        const ImageBuffer& buf,
        const GlobalAdjustment& params,
        bool clipHighlights,
        bool clipShadows);

signals:
    void fullResNeeded();
    void cropCommitted(const QRectF& cropRect, bool constrained);
    void rotationCommitted(float degrees);                // Straighten tool result
    void whiteBalanceCommitted(float kelvin, float tint); // WB picker result
    void activeToolChanged(ImageViewport::ActiveTool tool);
    // Live geometry of the active mask (either type) while its handles are dragged.
    void localMaskChanged(int index, const Mask& mask);
    // A handle drag finished — fold it into one undo step.
    void localMaskEditFinished();

    // SpotTool signals. All coordinates are original buffer pixels (docs/adr/0017).
    void spotRequested(QPointF destBufPx); // user clicked to place a new spot
    void spotHandleChanged(int idx, SpotHandle, QPointF newBufPx);   // drag (live)
    void spotHandleCommitted(int idx, SpotHandle, QPointF newBufPx); // mouse release
    void zoomChanged(float pixelZoom);
    // Small shader-rendered samples for histogramming (docs/adr/0004):
    // finalSample is the full pipeline, curveInputSample stops after tone
    // regions and is gamma-encoded — what the tone curve receives.
    void histogramsReady(const QImage& finalSample, const QImage& curveInputSample);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
    void resizeEvent(QResizeEvent* e) override;

    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;

private:
    friend class ViewportOverlay;

    // Crop handles: TL, TC, TR, MR, BR, BC, BL, ML
    static constexpr int kHandleCount = 8;
    static constexpr float kHandleRadius = 6.0f;

    // Shadows QWidget::update: also repaints the QPainter overlay child
    // (QRhiWidget content cannot be painted over directly).
    void update();

    void ensureCurveLut();
    void renderHistograms();
    RendererCore::Slot activeSlot() const;
    void paintOverlay(QPainter& p) const;

    // Crop lives in the rotated *display frame*: cropRect is an axis-aligned
    // rectangle of what you see, and the shader applies rotation underneath
    // (crop after rotation). These map that frame ↔ viewport with the fit/pan
    // but NOT the rotation — an axis-aligned crop stays axis-aligned on screen.
    QPointF cropUVToViewport(float u, float v) const;
    QPointF viewportToCropUV(QPointF pos) const;

    // True when every corner of a display-frame crop maps onto real image
    // pixels (no empty corner left by the rotation). The shader rotation is the
    // arbiter, so this mirrors it.
    bool cropInsideImage(const QRectF& cropUV) const;
    // Largest centred, full-aspect crop that fits inside the rotated image.
    QRectF maxInscribedCrop() const;

    // Returns handle index 0-7, or -1 for "inside rect" (move), or -2 for "outside" (rotate)
    int hitTest(QPointF viewportPos) const;
    QPointF handlePos(int i) const;
    void applyCropDrag(QPointF viewportPos);

    void drawCropOverlay(QPainter& p) const;
    void drawAlignGrid(QPainter& p) const;
    bool shouldShowAlignGrid() const;

    // Local-mask tool: edit the active Linear mask's handles on the image.
    bool localMaskMode() const { return tool == ActiveTool::LocalMask; }

    const LinearMask* activeLinearMask() const;
    const RadialMask* activeRadialMask() const;
    QPointF localHandleViewport(LinearHandle h) const; // p0/p1/center → screen
    LinearHandle hitTestLocalMask(QPointF viewportPos) const;
    RadialHandle hitTestRadialMask(QPointF viewportPos) const;
    void drawLocalMaskOverlay(QPainter& p) const; // Linear
    void drawRadialMaskOverlay(QPainter& p) const;

    // SpotTool coordinate transforms and overlay.
    QPointF viewportToBufferPixel(QPointF viewportPos) const;
    QPointF bufferPixelToViewport(QPointF bufPx) const;
    double bufRadiusToViewport(QPointF centerBufPx, double radius) const;
    SpotHandle hitTestSpot(QPointF viewportPos, int& outIdx) const;
    void drawSpotOverlay(QPainter& p) const;

    // Crop is just one of the active tools; this keeps the crop code readable.
    bool cropMode() const { return tool == ActiveTool::Crop; }

    void enterCrop();  // snapshot + seed the editable crop rect
    void commitCrop(); // write activeCrop into params and emit cropCommitted

    // Straighten: turn the drawn line into a rotation (auto horizontal/vertical).
    void applyStraightenLine();
    void drawStraightenLine(QPainter& p) const;

    // WB picker: read the pre-WB pixel value under pos (GPU tap, docs/adr/0004)
    // and invert the additive WB model into kelvin/tint. False if pos is off-image.
    bool sampleWhiteBalance(QPointF pos, float& kelvin, float& tintOut);

    // Aspect ratio of the region currently shown (accounts for committed crop).
    float displayAspect() const;
    float fitZoom() const;
    float displayOriginalPixelHeight() const;
    viewport::Geometry geometry() const;

    // ── Render state ──────────────────────────────────────────────────────
    RendererCore core;
    bool curveLutDirty = true;
    bool useDisplayLut = false;
    bool gamutWarn = false;
    bool clipHighlights = false;
    bool clipShadows = false;
    ViewportOverlay* overlay = nullptr;

    // ── Image state ───────────────────────────────────────────────────────
    GlobalAdjustment params;
    float imageAspect = 1.0f; // width/height of the full uncropped image
    int originalWidth = 0;
    int originalHeight = 0;
    bool hasImage = false;
    bool hasFullRes = false;
    bool useBaseLook = false;

    // The preview is half-res, so its texels start to magnify around zoom 2×.
    // Request the full-res texture a bit earlier so it is ready in time.
    static constexpr float kFullResZoomThreshold = 1.5f;

    QTimer histoTimer; // debounces histogram readbacks during slider drags

    // ── View state ────────────────────────────────────────────────────────
    float zoom = 1.0f;
    QPointF pan = {0, 0};
    QPointF dragStart;
    bool dragging = false;
    bool showOriginal = false;
    bool straightenActive = false;

    // ── Tool state ────────────────────────────────────────────────────────
    ActiveTool tool = ActiveTool::None;

    // Local-mask editing: which mask is active, and the handle being dragged.
    static constexpr float kMaskHandleRadius = 8.0f;
    int activeLocalAdj = -1;
    LinearHandle localDragHandle = LinearHandle::None;
    RadialHandle radialDragHandle = RadialHandle::None;

    // SpotTool state.
    std::vector<Spot> spots;
    int spotDragIdx = -1;
    SpotHandle spotDragHandle = SpotHandle::None;

    // Straighten: endpoints of the level line being dragged (viewport pixels).
    bool straightenDragging = false;
    QPointF straightenStart;
    QPointF straightenEnd;

    // ── Crop state ────────────────────────────────────────────────────────
    // The crop being edited is axis-aligned in the display frame (see
    // cropUVToViewport). cancelCrop is the snapshot restored on Escape.
    QRectF activeCrop = {0, 0, 1, 1}; // display-frame UV rect being edited
    QRectF cancelCrop = {0, 0, 1, 1};
    int cropDragHandle = -2; // which handle is dragged
    QPointF cropDragStart;
    QRectF cropDragStartRect;

    // Aspect Ratio Lock: the pixel width:height the crop is constrained to, or 0
    // when free. Set from the menu (a preset) or restored from a constrained
    // crop on entry. When non-zero, corner drags preserve it and edges go inert.
    double lockedRatio = 0.0;
};
