#pragma once

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <array>

namespace arraw::app {

/// Zoom levels the View menu and the status bar offer, as device pixels per
/// photograph pixel; an exact doubling chain, so Zoom In and Out stay on it.
inline constexpr std::array<double, 5> zoomPresets = {0.25, 0.5, 1.0, 2.0, 4.0};

/// Largest zoom, in device pixels per photograph pixel.
inline constexpr double maxZoom = 32.0;

/// @brief Finds the preset a zoom is, to within half a percentage point.
/// @param zoom Device pixels per photograph pixel.
/// @return Index into ::arraw::app::zoomPresets, or -1 if no preset matches.
[[nodiscard]] int matchingZoomPreset(double zoom);

/// @brief Names a zoom for the status bar button.
/// @param zoom Device pixels per photograph pixel.
/// @param fit Whether the view is fitting the whole frame.
/// @return "Fit", or "1:1" at exactly one, or a rounded percentage.
[[nodiscard]] QString zoomLabel(double zoom, bool fit);

/// @brief Names a preset by its percentage, as the menus list it.
/// @param zoom Device pixels per photograph pixel.
[[nodiscard]] QString zoomPercentLabel(double zoom);

/// @brief Where the developed frame lies in a view, as a value.
///
/// Everything is in device pixels: the frame is the developed (cropped) photograph
/// at full resolution, the view is the widget's area times its pixel ratio, and
/// the zoom is device pixels per frame pixel, so 1.0 shows one image pixel per
/// screen pixel. The centre is the point of the frame, in fractions of its
/// sides, at the middle of the view.
///
/// A transform is always valid: construction clamps the zoom and the centre, so
/// the frame never leaves a view it is larger than, and is centred in one it is
/// smaller than. Changes return a new transform. It is plain maths, so it is
/// tested without widgets.
class ViewTransform {
public:
    /// @brief Makes a transform, clamping the zoom and the centre.
    /// @param frame Size of the developed frame at full resolution.
    /// @param view Size of the view.
    /// @param zoom Device pixels per frame pixel, brought within the limits.
    /// @param centre Frame point at the view's middle, in fractions of the frame.
    ViewTransform(QSizeF frame, QSizeF view, double zoom, QPointF centre = {0.5, 0.5});

    /// @brief Makes the transform that fits the whole frame in the view.
    static ViewTransform fitted(QSizeF frame, QSizeF view);

    /// @brief Gives the largest zoom that shows the whole frame, but never above 1.
    ///
    /// A small photograph is not enlarged to fit. 1 if either size is empty.
    [[nodiscard]] static double fitZoom(QSizeF frame, QSizeF view);

    /// @brief Gives the smallest zoom: the fit, or a quarter if that is smaller.
    [[nodiscard]] static double minZoom(QSizeF frame, QSizeF view);

    /// @brief Gives the zoom, within the limits.
    [[nodiscard]] double zoom() const noexcept {
        return zoom_;
    }
    /// @brief Gives the frame point at the view's middle, in fractions of the frame.
    [[nodiscard]] QPointF centre() const noexcept {
        return centre_;
    }
    /// @brief Gives the frame size.
    [[nodiscard]] QSizeF frame() const noexcept {
        return frame_;
    }
    /// @brief Gives the view size.
    [[nodiscard]] QSizeF view() const noexcept {
        return view_;
    }
    /// @brief Tells whether the zoom is the fit one for this frame and view.
    [[nodiscard]] bool isFit() const;

    /// @brief Maps a view position to a frame point.
    /// @param position Device pixels from the view's top-left corner.
    /// @return Fractions of the frame, not clipped: outside [0, 1] is off the photograph.
    [[nodiscard]] QPointF frameFromView(QPointF position) const;

    /// @brief Maps a frame point to a view position.
    /// @param point Fractions of the frame.
    /// @return Device pixels from the view's top-left corner.
    [[nodiscard]] QPointF viewFromFrame(QPointF point) const;

    /// @brief Changes the zoom, keeping the frame point under a view position there.
    ///
    /// Where the clamped centre cannot keep it (a frame smaller than the view),
    /// the frame stays centred instead.
    /// @param zoom New zoom, brought within the limits.
    /// @param position Device pixels from the view's top-left corner.
    [[nodiscard]] ViewTransform zoomedAbout(double zoom, QPointF position) const;

    /// @brief Moves the frame with a drag.
    /// @param delta How far the pointer moved, in device pixels; the frame follows it.
    [[nodiscard]] ViewTransform panned(QPointF delta) const;

    /// @brief Gives the part of the frame in view, as fractions clipped to [0, 1].
    [[nodiscard]] QRectF visibleRegion() const;

    /// @brief Gives the part of the frame in view, in whole frame pixels.
    ///
    /// The visible region snapped outward, so everything seen is covered, and at
    /// least one pixel.
    [[nodiscard]] QRect visiblePixels() const;

    /// @brief Gives the size to render visiblePixels() at.
    ///
    /// The region's pixels times the zoom, or times 1 above it: a view
    /// magnifies a render, so a render never exceeds the photograph's own pixels.
    /// At least 1x1.
    [[nodiscard]] QSize outputSize() const;

private:
    QSizeF frame_;
    QSizeF view_;
    double zoom_;
    QPointF centre_;
};

} // namespace arraw::app
