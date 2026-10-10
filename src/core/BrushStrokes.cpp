#include "BrushStrokes.h"

#include <LocalAdjustments.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace arraw {

namespace {

constexpr std::uint64_t fnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t fnvPrime = 0x100000001b3ULL;

/// Folds the bytes of a value into an FNV-1a hash.
std::uint64_t foldBytes(std::uint64_t hash, std::uint64_t value, int bytes) noexcept {
    for (int i = 0; i < bytes; ++i) {
        hash ^= (value >> (8 * i)) & 0xFFU;
        hash *= fnvPrime;
    }
    return hash;
}

/// The bits of a float, with minus zero as plus zero because they compare equal.
std::uint32_t bitsOf(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value + 0.0F);
}

/// Hashes one stroke's style and points.
std::uint64_t hashOf(const Stroke& stroke) noexcept {
    std::uint64_t hash = fnvOffset;
    hash = foldBytes(hash, bitsOf(stroke.radius), 4);
    hash = foldBytes(hash, bitsOf(stroke.hardness), 4);
    hash = foldBytes(hash, bitsOf(stroke.flow), 4);
    hash = foldBytes(hash, stroke.erase ? 1 : 0, 1);
    hash = foldBytes(hash, stroke.points.size(), 8);
    for (const SensorPoint& point : stroke.points) {
        hash = foldBytes(hash, bitsOf(point.u), 4);
        hash = foldBytes(hash, bitsOf(point.v), 4);
    }
    return hash;
}

/// Folds a stroke's hash into a list's hash, in order.
std::uint64_t mix(std::uint64_t hash, std::uint64_t strokeHash) noexcept {
    return foldBytes(hash, strokeHash, 8);
}

/// Dab spacing as a fraction of the radius (ADR 044 section 6).
constexpr double dabSpacingFactor = 0.25;

[[noreturn]] void refuse(const char* field) {
    throw std::invalid_argument(std::string("stroke ") + field + " is out of range");
}

void requireFinite(float value, const char* field) {
    if (!std::isfinite(value)) {
        refuse(field);
    }
}

/// Refuses a mask that would pass a budget.
[[noreturn]] void refuseMask(const char* what) {
    throw std::invalid_argument(std::string("a mask would pass its ") + what);
}

} // namespace

StrokeBudget& StrokeBudget::operator+=(const StrokeBudget& other) noexcept {
    points += other.points;
    sweptArea += other.sweptArea;
    dabs += other.dabs;
    return *this;
}

bool StrokeBudget::withinMaskLimits() const noexcept {
    return points <= maximumPointsPerMask && sweptArea <= maximumSweptAreaPerMask &&
           dabs <= maximumDabsPerMask;
}

StrokeBudget budgetOf(const Stroke& stroke) noexcept {
    double length = 0.0;
    for (std::size_t i = 1; i < stroke.points.size(); ++i) {
        const double du = static_cast<double>(stroke.points[i].u) - stroke.points[i - 1].u;
        const double dv = static_cast<double>(stroke.points[i].v) - stroke.points[i - 1].v;
        length += std::sqrt(du * du + dv * dv);
    }
    const double radius = stroke.radius;
    return {stroke.points.size(), length * radius, length / (dabSpacingFactor * radius)};
}

void validate(const Stroke& stroke) {
    requireFinite(stroke.radius, "radius");
    requireFinite(stroke.hardness, "hardness");
    requireFinite(stroke.flow, "flow");
    if (stroke.radius < minimumBrushRadius || stroke.radius > maximumBrushRadius) {
        refuse("radius");
    }
    if (stroke.hardness < 0.0F || stroke.hardness > 1.0F) {
        refuse("hardness");
    }
    if (stroke.flow < 0.0F || stroke.flow > 1.0F) {
        refuse("flow");
    }
    if (stroke.points.empty() || stroke.points.size() > maximumStrokePoints) {
        refuse("points");
    }
    for (const SensorPoint& point : stroke.points) {
        requireFinite(point.u, "point");
        requireFinite(point.v, "point");
        if (point.u < minimumMaskPosition || point.u > maximumMaskPosition ||
            point.v < minimumMaskPosition || point.v > maximumMaskPosition) {
            refuse("point");
        }
    }
    if (!budgetOf(stroke).withinMaskLimits()) {
        refuse("length");
    }
}

Stroke normalised(Stroke stroke) {
    requireFinite(stroke.radius, "radius");
    requireFinite(stroke.hardness, "hardness");
    requireFinite(stroke.flow, "flow");
    if (stroke.points.empty() || stroke.points.size() > maximumStrokePoints) {
        refuse("points");
    }
    stroke.radius = std::clamp(stroke.radius, minimumBrushRadius, maximumBrushRadius);
    stroke.hardness = std::clamp(stroke.hardness, 0.0F, 1.0F);
    stroke.flow = std::clamp(stroke.flow, 0.0F, 1.0F);
    for (SensorPoint& point : stroke.points) {
        requireFinite(point.u, "point");
        requireFinite(point.v, "point");
        point.u = std::clamp(point.u, minimumMaskPosition, maximumMaskPosition);
        point.v = std::clamp(point.v, minimumMaskPosition, maximumMaskPosition);
    }
    if (!budgetOf(stroke).withinMaskLimits()) {
        refuse("length");
    }
    return stroke;
}

std::uint64_t StrokeList::seedFor(std::uint32_t rasteriser) noexcept {
    return foldBytes(fnvOffset, rasteriser, 4);
}

StrokeList::StrokeList() : hash_(seedFor(brushRasteriserVersion)) {}

void StrokeList::push(std::shared_ptr<const Stroke> stroke) {
    budget_ += budgetOf(*stroke);
    hash_ = mix(hash_, hashOf(*stroke));
    strokes_.push_back(std::move(stroke));
}

StrokeList::StrokeList(std::vector<Stroke> strokes, std::uint32_t rasteriser)
    : rasteriser_(rasteriser), hash_(seedFor(rasteriser)) {
    if (strokes.size() > maximumStrokesPerMask) {
        throw std::invalid_argument("a mask holds at most " +
                                    std::to_string(maximumStrokesPerMask) + " strokes");
    }
    strokes_.reserve(strokes.size());
    for (Stroke& stroke : strokes) {
        validate(stroke);
        push(std::make_shared<const Stroke>(std::move(stroke)));
        if (!budget_.withinMaskLimits()) {
            refuseMask(budget_.points > maximumPointsPerMask         ? "points"
                       : budget_.sweptArea > maximumSweptAreaPerMask ? "swept area"
                                                                     : "dabs");
        }
    }
}

bool StrokeList::accepts(const Stroke& stroke) const noexcept {
    if (strokes_.size() >= maximumStrokesPerMask) {
        return false;
    }
    StrokeBudget total = budget_;
    total += budgetOf(stroke);
    return total.withinMaskLimits();
}

std::shared_ptr<const StrokeList> StrokeList::appended(Stroke stroke) const {
    if (strokes_.size() >= maximumStrokesPerMask) {
        throw std::invalid_argument("a mask holds at most " +
                                    std::to_string(maximumStrokesPerMask) + " strokes");
    }
    validate(stroke);
    StrokeBudget total = budget_;
    total += budgetOf(stroke);
    if (!total.withinMaskLimits()) {
        refuseMask(total.points > maximumPointsPerMask         ? "points"
                   : total.sweptArea > maximumSweptAreaPerMask ? "swept area"
                                                               : "dabs");
    }
    auto next = std::make_shared<StrokeList>();
    next->rasteriser_ = rasteriser_;
    next->strokes_ = strokes_;
    next->budget_ = budget_;
    next->hash_ = hash_;
    next->push(std::make_shared<const Stroke>(std::move(stroke)));
    return next;
}

bool operator==(const StrokeList& a, const StrokeList& b) {
    if (a.rasteriser_ != b.rasteriser_ || a.strokes_.size() != b.strokes_.size() ||
        a.hash_ != b.hash_) {
        return false;
    }
    for (std::size_t i = 0; i < a.strokes_.size(); ++i) {
        if (a.strokes_[i] != b.strokes_[i] && *a.strokes_[i] != *b.strokes_[i]) {
            return false;
        }
    }
    return true;
}

} // namespace arraw
