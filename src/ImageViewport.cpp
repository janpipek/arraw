#include "ImageViewport.h"
#include "ImagePipeline.h"
#include <algorithm>
#include <cmath>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

// CPU mirror of the rotation in image.vert (keep in sync). UV space is not
// square, so x is scaled by the image aspect before rotating to make the
// rotation isotropic in pixel space, then scaled back.
static QPointF rotateTexUV(float u, float v, float degrees, float aspect, float cx, float cy) {
    float dx = (u - cx) * aspect;
    float dy = v - cy;
    const float rad = degrees * float(M_PI) / 180.0f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float rx = c * dx - s * dy;
    const float ry = s * dx + c * dy;
    return {rx / aspect + cx, ry + cy};
}

// Short ratio tag shown in the crop readout, oriented to the current toggle.
static QString aspectLabel(crop::AspectPreset preset, bool landscape) {
    switch (preset) {
    case crop::AspectPreset::Free:
        return {};
    case crop::AspectPreset::Original:
        return QStringLiteral("Orig");
    case crop::AspectPreset::Square:
        return QStringLiteral("1:1");
    case crop::AspectPreset::R2x3:
        return landscape ? QStringLiteral("3:2") : QStringLiteral("2:3");
    case crop::AspectPreset::R3x4:
        return landscape ? QStringLiteral("4:3") : QStringLiteral("3:4");
    case crop::AspectPreset::R4x5:
        return landscape ? QStringLiteral("5:4") : QStringLiteral("4:5");
    case crop::AspectPreset::R16x9:
        return landscape ? QStringLiteral("16:9") : QStringLiteral("9:16");
    }
    return {};
}

// QRhiWidget content cannot be painted over with QPainter (unlike
// QOpenGLWidget), so the crop overlay and align grid live on a transparent,
// mouse-transparent child that the viewport repaints alongside itself.
class ViewportOverlay : public QWidget {
public:
    explicit ViewportOverlay(ImageViewport* parent)
        : QWidget(parent),
          viewport(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!viewport->hasImage)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        viewport->paintOverlay(p);
    }

private:
    ImageViewport* viewport;
};

ImageViewport::ImageViewport(QWidget* parent)
    : QRhiWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    overlay = new ViewportOverlay(this);

    histoTimer.setSingleShot(true);
    histoTimer.setInterval(150);
    connect(&histoTimer, &QTimer::timeout, this, &ImageViewport::renderHistograms);
}

ImageViewport::~ImageViewport() = default;

void ImageViewport::update() {
    QRhiWidget::update();
    overlay->update();
}

// ── RHI lifecycle ─────────────────────────────────────────────────────────────

void ImageViewport::initialize(QRhiCommandBuffer*) {
    core.initialize(rhi());
}

void ImageViewport::releaseResources() {
    core.release();
}

void ImageViewport::resizeEvent(QResizeEvent* e) {
    QRhiWidget::resizeEvent(e);
    overlay->setGeometry(rect());
    emit zoomChanged(pixelZoom());
}

// ── Rendering ─────────────────────────────────────────────────────────────────

float ImageViewport::displayAspect() const {
    if (!hasImage || cropMode() || showOriginal)
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
    if (cropMode() || showOriginal)
        return float(originalHeight);
    return float(originalHeight) * float(params.cropRect.height());
}

RendererCore::Slot ImageViewport::activeSlot() const {
    if (hasFullRes && core.hasImage(RendererCore::Slot::FullRes) && zoom >= kFullResZoomThreshold)
        return RendererCore::Slot::FullRes;
    return RendererCore::Slot::Preview;
}

void ImageViewport::ensureCurveLut() {
    if (!curveLutDirty)
        return;
    const auto lumaLUT = computeCurveLUT(params.curveLuma.points);
    const auto redLUT = computeCurveLUT(params.curveR.points);
    const auto grnLUT = computeCurveLUT(params.curveG.points);
    const auto bluLUT = computeCurveLUT(params.curveB.points);

    std::array<float, 256 * 4> rgba{};
    for (int i = 0; i < 256; ++i) {
        rgba[i * 4 + 0] = lumaLUT[i];
        rgba[i * 4 + 1] = redLUT[i];
        rgba[i * 4 + 2] = grnLUT[i];
        rgba[i * 4 + 3] = bluLUT[i];
    }
    core.setCurveLut(rgba);
    curveLutDirty = false;
}

void ImageViewport::render(QRhiCommandBuffer* cb) {
    if (!hasImage) {
        core.clear(cb, renderTarget());
        return;
    }
    ensureCurveLut();

    const AdjustmentParams& p = showOriginal ? AdjustmentParams{} : params;
    const float viewportAspect = float(width()) / float(height());

    RendererCore::FrameParams fp;
    fp.transform = QVector4D(zoom * (displayAspect() / viewportAspect), zoom, pan.x(), pan.y());
    fp.cropRect = cropMode() ? QRectF(0, 0, 1, 1) : p.cropRect;
    fp.aspect = imageAspect;
    fp.baseLook = useBaseLook && !showOriginal;
    fp.displayEncode = true;
    fp.curveInput = false;
    fp.useLut = useDisplayLut;
    fp.gamutWarn = gamutWarn;
    fp.clipHighlights = clipHighlights;
    fp.clipShadows = clipShadows;
    fp.adjustments = p;

    core.record(cb, renderTarget(), activeSlot(), fp);
}

void ImageViewport::paintOverlay(QPainter& p) const {
    if (cropMode()) {
        drawCropOverlay(p);
    } else if (shouldShowAlignGrid()) {
        drawAlignGrid(p);
        if (tool == ActiveTool::Straighten && straightenDragging)
            drawStraightenLine(p);
    }
}

// ── Coordinate mapping ────────────────────────────────────────────────────────

// Display-frame UV ↔ viewport: the fit/pan transform without rotation. The
// crop is axis-aligned in this frame; the shader rotates the sampled image
// underneath it (crop after rotation), so the overlay must not rotate.
QPointF ImageViewport::cropUVToViewport(float u, float v) const {
    const float viewportAspect = float(width()) / float(height());
    const float sx = zoom * (displayAspect() / viewportAspect);
    const float sy = zoom;
    const float ndcX = (u * 2.0f - 1.0f) * sx + float(pan.x());
    const float ndcY = (1.0f - 2.0f * v) * sy + float(pan.y());
    return {(ndcX + 1.0f) * width() / 2.0f, (1.0f - ndcY) * height() / 2.0f};
}

QPointF ImageViewport::viewportToCropUV(QPointF pos) const {
    const float viewportAspect = float(width()) / float(height());
    const float sx = zoom * (displayAspect() / viewportAspect);
    const float sy = zoom;
    const float ndcX = float(pos.x()) * 2.0f / width() - 1.0f;
    const float ndcY = 1.0f - float(pos.y()) * 2.0f / height();
    const float u = ((ndcX - float(pan.x())) / sx + 1.0f) / 2.0f;
    const float v = (1.0f - (ndcY - float(pan.y())) / sy) / 2.0f;
    return {u, v};
}

// A display-frame point shows real image pixels iff, after the shader's
// rotation, it lands inside the source [0,1] (rotateTexUV mirrors image.vert).
bool ImageViewport::cropInsideImage(const QRectF& cropUV) const {
    if (std::abs(params.rotation) <= 0.0001f)
        return true; // no rotation: the whole display frame is real
    const QPointF corners[4] = {
        cropUV.topLeft(),
        cropUV.topRight(),
        cropUV.bottomRight(),
        cropUV.bottomLeft(),
    };
    const float eps = 1e-4f;
    for (const QPointF& c : corners) {
        const QPointF s
            = rotateTexUV(float(c.x()), float(c.y()), params.rotation, imageAspect, 0.5f, 0.5f);
        if (s.x() < -eps || s.x() > 1.0f + eps || s.y() < -eps || s.y() > 1.0f + eps)
            return false;
    }
    return true;
}

// Largest centred crop of the full-image aspect that avoids the empty corners a
// rotation leaves behind. The full frame (0,0,1,1) scaled about its centre keeps
// the aspect, so binary-search the scale that just fits.
QRectF ImageViewport::maxInscribedCrop() const {
    if (cropInsideImage({0, 0, 1, 1}))
        return {0, 0, 1, 1};
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 24; ++i) {
        const double s = 0.5 * (lo + hi);
        const QRectF r((1.0 - s) / 2.0, (1.0 - s) / 2.0, s, s);
        if (cropInsideImage(r))
            lo = s;
        else
            hi = s;
    }
    const double s = lo;
    return {(1.0 - s) / 2.0, (1.0 - s) / 2.0, s, s};
}

// ── Straighten alignment grid (viewport-fixed horizontals / verticals) ────────

bool ImageViewport::shouldShowAlignGrid() const {
    if (!hasImage || cropMode() || showOriginal)
        return false;
    if (straightenActive || tool == ActiveTool::Straighten)
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
    const float l = float(activeCrop.left());
    const float r = float(activeCrop.right());
    const float t = float(activeCrop.top());
    const float b = float(activeCrop.bottom());
    const float cx = (l + r) * 0.5f;
    const float cy = (t + b) * 0.5f;
    const QPointF uvs[kHandleCount]
        = {{l, t}, {cx, t}, {r, t}, {r, cy}, {r, b}, {cx, b}, {l, b}, {l, cy}};
    return cropUVToViewport(float(uvs[i].x()), float(uvs[i].y()));
}

int ImageViewport::hitTest(QPointF pos) const {
    for (int i = 0; i < kHandleCount; ++i) {
        QPointF d = pos - handlePos(i);
        if (d.x() * d.x() + d.y() * d.y() < kHandleRadius * kHandleRadius * 4)
            return i;
    }
    if (activeCrop.contains(viewportToCropUV(pos)))
        return -1;
    return -2;
}

void ImageViewport::applyCropDrag(QPointF viewportPos) {
    QPointF uv = viewportToCropUV(viewportPos);
    float u = float(std::clamp(uv.x(), 0.0, 1.0));
    float v = float(std::clamp(uv.y(), 0.0, 1.0));

    // Under an Aspect Ratio Lock, corner drags preserve the ratio (anchored at
    // the opposite corner) and edge handles are inactive; the move handle (-1)
    // still translates via the free path below.
    const double lockRatio = crop::presetRatio(cropAspect, cropLandscape, imageAspect);
    if (lockRatio > 0.0 && cropDragHandle >= 0) {
        const QRectF locked = crop::lockedResize(
            cropDragHandle, cropDragStartRect, QPointF(u, v), lockRatio, imageAspect);
        if (cropInsideImage(locked)) {
            activeCrop = locked;
            update();
        }
        return;
    }

    QRectF r = cropDragStartRect;
    const float kMinSize = 0.02f;

    switch (cropDragHandle) {
    case 0:
        r.setTopLeft(
            {std::min(u, float(r.right()) - kMinSize), std::min(v, float(r.bottom()) - kMinSize)});
        break;
    case 1:
        r.setTop(std::min(v, float(r.bottom()) - kMinSize));
        break;
    case 2:
        r.setTopRight(
            {std::max(u, float(r.left()) + kMinSize), std::min(v, float(r.bottom()) - kMinSize)});
        break;
    case 3:
        r.setRight(std::max(u, float(r.left()) + kMinSize));
        break;
    case 4:
        r.setBottomRight(
            {std::max(u, float(r.left()) + kMinSize), std::max(v, float(r.top()) + kMinSize)});
        break;
    case 5:
        r.setBottom(std::max(v, float(r.top()) + kMinSize));
        break;
    case 6:
        r.setBottomLeft(
            {std::min(u, float(r.right()) - kMinSize), std::max(v, float(r.top()) + kMinSize)});
        break;
    case 7:
        r.setLeft(std::min(u, float(r.right()) - kMinSize));
        break;
    case -1: { // move
        QPointF startUV = viewportToCropUV(cropDragStart);
        float du = float(uv.x() - startUV.x());
        float dv = float(uv.y() - startUV.y());
        float w = float(cropDragStartRect.width());
        float h = float(cropDragStartRect.height());
        float nl = std::clamp(float(cropDragStartRect.left()) + du, 0.0f, 1.0f - w);
        float nt = std::clamp(float(cropDragStartRect.top()) + dv, 0.0f, 1.0f - h);
        r = QRectF(nl, nt, w, h);
        break;
    }
    default:
        return;
    }
    // Constrain to the rotated image: reject candidates that would pull a corner
    // into the empty area, so the edge "sticks" at the boundary instead.
    if (cropInsideImage(r)) {
        activeCrop = r;
        update();
    }
}

void ImageViewport::drawCropOverlay(QPainter& p) const {
    const QRectF cropVP
        = QRectF(
              cropUVToViewport(float(activeCrop.left()), float(activeCrop.top())),
              cropUVToViewport(float(activeCrop.right()), float(activeCrop.bottom())))
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
        p.drawRect(QRectF(
            hp.x() - kHandleRadius, hp.y() - kHandleRadius, kHandleRadius * 2, kHandleRadius * 2));
    }

    // Live readout: output pixel size (shared with export via cropPixelSize) and,
    // when locked, the ratio tag. Pixel size needs the known full-res dimensions.
    QString label;
    if (hasKnownOriginalSize()) {
        const QSize px = crop::cropPixelSize(originalWidth, originalHeight, activeCrop);
        label = QStringLiteral("%1 × %2").arg(px.width()).arg(px.height());
    }
    if (cropAspect != crop::AspectPreset::Free) {
        const QString tag = aspectLabel(cropAspect, cropLandscape);
        label = label.isEmpty() ? tag : label + QStringLiteral("  ·  ") + tag;
    }
    if (!label.isEmpty()) {
        const QFontMetrics fm(p.font());
        const QRectF textRect = fm.boundingRect(label);
        const qreal padX = 8.0;
        const qreal padY = 4.0;
        const qreal boxW = textRect.width() + 2 * padX;
        const qreal boxH = textRect.height() + 2 * padY;
        // Centred just inside the crop's top edge; nudged down if it would clip.
        qreal boxX = cropVP.center().x() - boxW / 2.0;
        qreal boxY = cropVP.top() + 8.0;
        boxX = std::clamp(boxX, 0.0, qreal(width()) - boxW);
        boxY = std::clamp(boxY, 0.0, qreal(height()) - boxH);
        const QRectF box(boxX, boxY, boxW, boxH);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(box, 4.0, 4.0);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
    }
}

// ── Public setters ────────────────────────────────────────────────────────────

void ImageViewport::setImage(const ImageBuffer& buf, bool baseLook) {
    imageAspect = buf.valid() ? float(buf.width) / float(buf.height) : 1.0f;
    hasImage = buf.valid();
    hasFullRes = false;
    useBaseLook = baseLook;
    core.setImage(RendererCore::Slot::Preview, buf);
    core.setImage(RendererCore::Slot::FullRes, {});
    resetView();
    histoTimer.start();
    update();
}

void ImageViewport::setFullResImage(const ImageBuffer& buf) {
    if (buf.valid())
        setOriginalImageSize(buf.width, buf.height);
    hasFullRes = buf.valid();
    core.setImage(RendererCore::Slot::FullRes, buf);
    if (zoom >= kFullResZoomThreshold)
        update();
}

void ImageViewport::setAdjustments(const AdjustmentParams& p) {
    if (p.curveLuma != params.curveLuma || p.curveR != params.curveR || p.curveG != params.curveG
        || p.curveB != params.curveB)
        curveLutDirty = true;
    params = p;
    if (!cropMode())
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

void ImageViewport::setAspectLock(crop::AspectPreset preset, bool landscape) {
    cropAspect = preset;
    cropLandscape = landscape;
    if (!cropMode())
        return;
    // Reshape the current crop to the new ratio: shrink-to-fit keeps it inside
    // the already-valid rectangle, so the rotation guard is satisfied for free.
    const double ratio = crop::presetRatio(preset, landscape, imageAspect);
    if (ratio > 0.0) {
        const QRectF r = crop::fitRatioInside(activeCrop, ratio, imageAspect);
        if (cropInsideImage(r))
            activeCrop = r;
    }
    update();
}

// ── Active-tool state machine ─────────────────────────────────────────────────

void ImageViewport::setActiveTool(ActiveTool t) {
    if (tool == t)
        return;
    // Commit-on-leave: switching away (or toggling off) keeps the pending edit
    // of the tool we are leaving. Esc — via cancelActiveTool — is the discard path.
    if (tool == ActiveTool::Crop)
        commitCrop();

    tool = t;
    straightenDragging = false;
    if (tool == ActiveTool::Crop)
        enterCrop();

    setCursor(
        tool == ActiveTool::Straighten || tool == ActiveTool::WhiteBalance ? Qt::CrossCursor
                                                                           : Qt::ArrowCursor);
    emit activeToolChanged(tool);
    update();
}

void ImageViewport::cancelActiveTool() {
    if (tool == ActiveTool::None)
        return;
    if (tool == ActiveTool::Crop)
        activeCrop = cancelCrop;
    straightenDragging = false;
    tool = ActiveTool::None;
    setCursor(Qt::ArrowCursor);
    emit activeToolChanged(tool);
    update();
}

void ImageViewport::enterCrop() {
    cancelCrop = params.cropRect;
    activeCrop = params.cropRect;
    // The Aspect Ratio Lock is transient: every fresh crop starts unconstrained.
    cropAspect = crop::AspectPreset::Free;
    cropLandscape = true;
    // A rotation set before cropping (e.g. via Straighten) can leave the stored
    // crop covering empty corners — pull it in to the largest valid rectangle.
    if (!cropInsideImage(activeCrop))
        activeCrop = maxInscribedCrop();
}

void ImageViewport::commitCrop() {
    params.cropRect = activeCrop;
    emit cropCommitted(params.cropRect);
    emit zoomChanged(pixelZoom());
}

// Turn the drawn line into an absolute rotation. The line is folded to its
// nearest axis (auto horizontal/vertical), and the deviation from that axis is
// the correction. +rotation rotates content clockwise on screen (increasing a
// feature's screen angle), so the leveling correction is -deviation.
void ImageViewport::applyStraightenLine() {
    const QPointF d = straightenEnd - straightenStart;
    if (d.manhattanLength() < 8.0) // ignore taps / tiny drags
        return;
    double angle = std::atan2(d.y(), d.x()) * 180.0 / M_PI; // screen space, y down
    while (angle <= -90.0)
        angle += 180.0; // a line has no direction: fold to (-90,90]
    while (angle > 90.0)
        angle -= 180.0;
    const double deviation = std::abs(angle) <= 45.0
                                 ? angle                               // treat as horizontal
                                 : angle - (angle > 0 ? 90.0 : -90.0); // from vertical
    // Rotating by +deviation on top of the current rotation brings the drawn
    // line onto its axis (the line the user traced along the horizon goes level).
    const float newRotation = std::clamp(float(params.rotation + deviation), -45.0f, 45.0f);
    emit rotationCommitted(newRotation);
}

void ImageViewport::drawStraightenLine(QPainter& p) const {
    p.setPen(QPen(QColor(255, 220, 80), 1.5));
    p.drawLine(straightenStart, straightenEnd);
    p.setBrush(QColor(255, 220, 80));
    p.drawEllipse(straightenStart, 3.0, 3.0);
    p.drawEllipse(straightenEnd, 3.0, 3.0);
}

// Read the pre-WB pixel value the shader produces under `pos` (GPU tap, same
// readback rationale as the histograms — docs/adr/0004) and invert the additive
// white-balance model from image.frag to the temperature/tint that neutralise it.
bool ImageViewport::sampleWhiteBalance(QPointF pos, float& kelvin, float& tintOut) {
    if (!hasImage || !core.ready())
        return false;
    ensureCurveLut();

    // Render the current on-screen framing with the pre-WB tap at 1:1 viewport
    // pixels, so the clicked point maps straight to a texel (no UV inversion).
    const float viewportAspect = float(width()) / float(height());
    RendererCore::FrameParams fp;
    fp.transform = QVector4D(zoom * (displayAspect() / viewportAspect), zoom, pan.x(), pan.y());
    fp.cropRect = cropMode() ? QRectF(0, 0, 1, 1) : params.cropRect;
    fp.aspect = imageAspect;
    fp.baseLook = useBaseLook;
    fp.displayEncode = false;
    fp.wbInput = true;
    fp.adjustments = params;

    const QImage tap = core.renderOffscreen(activeSlot(), fp, size(), QRhiTexture::RGBA32F);
    if (tap.isNull())
        return false;

    // Average a small neighbourhood for noise robustness.
    const int x0 = int(pos.x()), y0 = int(pos.y()), rad = 2;
    double sr = 0, sg = 0, sb = 0;
    int n = 0;
    for (int y = y0 - rad; y <= y0 + rad; ++y) {
        if (y < 0 || y >= tap.height())
            continue;
        const auto* px = reinterpret_cast<const float*>(tap.constScanLine(y));
        for (int x = x0 - rad; x <= x0 + rad; ++x) {
            if (x < 0 || x >= tap.width())
                continue;
            sr += px[x * 4 + 0];
            sg += px[x * 4 + 1];
            sb += px[x * 4 + 2];
            ++n;
        }
    }
    if (n == 0)
        return false;
    const double r = sr / n, g = sg / n, b = sb / n;
    if (r + g + b < 1e-4) // clicked off the image (black) — ignore
        return false;

    // Invert image.frag's additive WB (keep these constants in sync with it):
    //   applyTemperature: t=(K-5500)/5500; r += t*0.15; b -= t*0.15
    //   applyTint:        g += (tint/100)*0.05
    // Solve r==g==b: t balances R/B; the grey level is r1=(r+b)/2; tint lifts G.
    const double t = (b - r) / 0.3; // 0.3 = 2*0.15
    const double r1 = 0.5 * (r + b);
    kelvin = float(std::clamp(5500.0 * (1.0 + t), 2000.0, 12000.0));
    tintOut = float(std::clamp((r1 - g) * 2000.0, -100.0, 100.0)); // 2000 = 100/0.05
    return true;
}

void ImageViewport::setOriginalImageSize(int width, int height) {
    originalWidth = width;
    originalHeight = height;
    emit zoomChanged(pixelZoom());
}

void ImageViewport::setDisplayLut(const DisplayLut& lut) {
    if (!lut.valid()) {
        clearDisplayLut();
        return;
    }
    core.setDisplayLut(lut);
    useDisplayLut = true;
    update();
}

void ImageViewport::clearDisplayLut() {
    useDisplayLut = false;
    update();
}

void ImageViewport::setGamutWarning(bool on) {
    if (gamutWarn == on)
        return;
    gamutWarn = on;
    update();
}

void ImageViewport::setClipWarnings(bool highlights, bool shadows) {
    if (clipHighlights == highlights && clipShadows == shadows)
        return;
    clipHighlights = highlights;
    clipShadows = shadows;
    update();
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

// ── Offscreen export render ───────────────────────────────────────────────────

QImage ImageViewport::renderToImage(
    const ImageBuffer& buf, const AdjustmentParams& p, int outW, int outH) {
    if (!core.ready() || !buf.valid())
        return {};

    // Ensure the curve LUT is current (params may have changed since last paint)
    ensureCurveLut();

    const QRectF& cr = p.cropRect;
    const int cropW = qMax(1, int(cr.width() * buf.width + 0.5f));
    const int cropH = qMax(1, int(cr.height() * buf.height + 0.5f));

    // Offscreen target at cropped pixel size. Float format: the readback stays
    // in linear working space; the output transform happens on the CPU (lcms2).
    RendererCore::FrameParams fp;
    fp.transform = QVector4D(1.0f, 1.0f, 0.0f, 0.0f);
    fp.cropRect = cr;
    fp.aspect = float(buf.width) / float(buf.height);
    fp.baseLook = true;
    fp.displayEncode = false;
    fp.curveInput = false;
    fp.useLut = false;
    fp.gamutWarn = false;
    fp.clipHighlights = false; // overlays never leak into the export readback
    fp.clipShadows = false;
    fp.adjustments = p;

    QImage result = core.renderOffscreen(buf, fp, QSize(cropW, cropH), QRhiTexture::RGBA32F);
    if (result.isNull())
        return {};

    // Scale to requested output dimensions while still in linear light —
    // gamma-space scaling darkens fine detail.
    if (result.width() != outW || result.height() != outH)
        result = result.scaled(outW, outH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    return result;
}

QImage ImageViewport::renderClipSample(
    const ImageBuffer& buf, const AdjustmentParams& p, bool clipHighlights, bool clipShadows) {
    if (!core.ready() || !buf.valid())
        return {};

    ensureCurveLut();

    // The on-screen display path (sRGB encode, monitor/proof LUT off) so the
    // clipping overlay actually runs — renderToImage uses the linear export
    // path where it is forced off. Used by the clipping golden test (adr/0009).
    const QRectF& cr = p.cropRect;
    const int cropW = qMax(1, int(cr.width() * buf.width + 0.5f));
    const int cropH = qMax(1, int(cr.height() * buf.height + 0.5f));

    RendererCore::FrameParams fp;
    fp.transform = QVector4D(1.0f, 1.0f, 0.0f, 0.0f);
    fp.cropRect = cr;
    fp.aspect = float(buf.width) / float(buf.height);
    fp.baseLook = true;
    fp.displayEncode = true;
    fp.curveInput = false;
    fp.useLut = false;
    fp.gamutWarn = false;
    fp.clipHighlights = clipHighlights;
    fp.clipShadows = clipShadows;
    fp.adjustments = p;

    return core.renderOffscreen(buf, fp, QSize(cropW, cropH), QRhiTexture::RGBA32F);
}

// ── Histogram readback (docs/adr/0004) ────────────────────────────────────────

// Render the preview texture through the real shader twice into a small
// offscreen target — once with curveInput (pipeline stops after tone regions,
// gamma-encoded) and once full — and hand both samples to whoever bins them.
void ImageViewport::renderHistograms() {
    if (!hasImage || !core.ready() || !core.hasImage(RendererCore::Slot::Preview))
        return;

    ensureCurveLut();

    const QRectF& cr = params.cropRect;
    const float aspect = imageAspect * float(cr.width() / cr.height());
    const int w = 256;
    const int h = std::clamp(int(w / aspect + 0.5f), 16, 1024);

    RendererCore::FrameParams fp;
    fp.transform = QVector4D(1.0f, 1.0f, 0.0f, 0.0f);
    fp.cropRect = cr;
    fp.aspect = imageAspect;
    fp.baseLook = useBaseLook;
    // Display transform on, monitor/proof LUT off: the panel histogram shows
    // output-sRGB values regardless of soft-proofing.
    fp.displayEncode = true;
    fp.useLut = false;
    fp.gamutWarn = false;
    fp.clipHighlights = false; // overlays never poison the histogram readback
    fp.clipShadows = false;
    fp.adjustments = params;

    fp.curveInput = true;
    const QImage curveInput
        = core.renderOffscreen(RendererCore::Slot::Preview, fp, QSize(w, h), QRhiTexture::RGBA8);

    fp.curveInput = false;
    const QImage finalSample
        = core.renderOffscreen(RendererCore::Slot::Preview, fp, QSize(w, h), QRhiTexture::RGBA8);

    emit histogramsReady(finalSample, curveInput);
}

// ── Input events ──────────────────────────────────────────────────────────────

void ImageViewport::wheelEvent(QWheelEvent* e) {
    const float factor = e->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
    setZoom(zoom * factor);
}

void ImageViewport::mousePressEvent(QMouseEvent* e) {
    if (cropMode() && e->button() == Qt::LeftButton) {
        cropDragHandle = hitTest(e->position());
        cropDragStart = e->position();
        cropDragStartRect = activeCrop;
        return;
    }
    if (tool == ActiveTool::Straighten && e->button() == Qt::LeftButton) {
        straightenDragging = true;
        straightenStart = straightenEnd = e->position();
        update();
        return;
    }
    if (tool == ActiveTool::WhiteBalance && e->button() == Qt::LeftButton) {
        float kelvin, tintOut;
        if (sampleWhiteBalance(e->position(), kelvin, tintOut))
            emit whiteBalanceCommitted(kelvin, tintOut);
        return; // tool stays active for further picks
    }
    if (e->button() == Qt::MiddleButton
        || (e->button() == Qt::LeftButton && e->modifiers() & Qt::AltModifier)) {
        dragging = true;
        dragStart = e->position();
    }
}

void ImageViewport::mouseMoveEvent(QMouseEvent* e) {
    if (cropMode() && (e->buttons() & Qt::LeftButton) && cropDragHandle > -2) {
        applyCropDrag(e->position());
        return;
    }
    if (tool == ActiveTool::Straighten && straightenDragging) {
        straightenEnd = e->position();
        update();
        return;
    }
    if (!dragging)
        return;
    // pan is in NDC units (viewport spans -1..1), hence the ×2 / size.
    QPointF delta = e->position() - dragStart;
    pan.setX(pan.x() + float(delta.x()) / width() * 2.0f);
    pan.setY(pan.y() - float(delta.y()) / height() * 2.0f);
    dragStart = e->position();
    update();
}

void ImageViewport::mouseReleaseEvent(QMouseEvent* e) {
    if (cropMode() && e->button() == Qt::LeftButton) {
        cropDragHandle = -2;
        return;
    }
    if (tool == ActiveTool::Straighten && straightenDragging && e->button() == Qt::LeftButton) {
        straightenDragging = false;
        applyStraightenLine(); // tool stays active; redraw the line to retry
        update();
        return;
    }
    dragging = false;
}

void ImageViewport::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) {
        QRhiWidget::keyPressEvent(e);
        return;
    }

    switch (e->key()) {
    case Qt::Key_Backslash:
        showOriginal = true;
        update();
        break;
    case Qt::Key_C:
        setActiveTool(cropMode() ? ActiveTool::None : ActiveTool::Crop);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (tool != ActiveTool::None)
            commitActiveTool(); // commit-on-leave
        break;
    case Qt::Key_Escape:
        if (tool != ActiveTool::None)
            cancelActiveTool();
        break;
    default:
        QRhiWidget::keyPressEvent(e);
    }
}

void ImageViewport::keyReleaseEvent(QKeyEvent* e) {
    if (!e->isAutoRepeat() && e->key() == Qt::Key_Backslash) {
        showOriginal = false;
        update();
    } else {
        QRhiWidget::keyReleaseEvent(e);
    }
}
