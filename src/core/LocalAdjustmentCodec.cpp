#include "LocalAdjustmentCodec.h"

#include <SettingDescriptors.h>
#include <ShortestDecimal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

using namespace arraw;

namespace {

constexpr std::array<std::string_view, 6> topKeys{"id",      "type",    "name",
                                                  "enabled", "opacity", "invert"};
constexpr std::array<std::string_view, 4> linearKeys{"fromX", "fromY", "toX", "toY"};
constexpr std::array<std::string_view, 1> brushKeys{"rasteriser"};
constexpr std::array<std::string_view, 6> radialKeys{"centreX", "centreY", "radiusX",
                                                     "radiusY", "angle",   "feather"};

/// @brief The largest id a mask may have: the counter above it must still fit.
constexpr double largestId = std::numeric_limits<std::uint32_t>::max() - 1.0;

/// @brief Why an entry is dropped, thrown to the point that reports it.
struct Drop {
    std::string reason;
};

/// @brief Finds the last field of a key, which is the one a document that repeats a key means.
const FieldValue* find(const FieldList& fields, std::string_view key) {
    for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
        if (it->first == key) {
            return &it->second;
        }
    }
    return nullptr;
}

/// @brief Reads an id if the field holds a usable one.
std::optional<std::uint32_t> idOf(const FieldValue* field) {
    if (field == nullptr || !field->number || !std::isfinite(*field->number) ||
        *field->number != std::trunc(*field->number) || *field->number < 1.0 ||
        *field->number > largestId) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*field->number);
}

/// @brief Wraps degrees into [-180, 180).
double wrapDegrees(double degrees) {
    double wrapped = std::fmod(degrees + 180.0, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped - 180.0;
}

/// @brief Reads one mask, with the warnings that belong to it.
class MaskReader {
public:
    MaskReader(std::size_t position, const MaskFields& fields, DiagnosticLog& log,
               const std::optional<std::filesystem::path>& subject)
        : position_(position), fields_(fields), log_(log), subject_(subject) {}

    /// @brief Reads the mask, or reports why it was dropped.
    std::optional<LocalAdjustment> read() {
        idText_ = idText(fields_);
        typeText_ = typeText(fields_);
        try {
            return build();
        } catch (const Drop& drop) {
            log_.record(
                {.notice = Notice::LocalAdjustmentDropped,
                 .severity = Severity::Warning,
                 .subject = subject_,
                 .values = {static_cast<double>(position_), idText_, typeText_, drop.reason}});
            return std::nullopt;
        }
    }

    /// @brief Spells the id of an entry for a warning; empty when it has none that reads.
    static std::string idText(const MaskFields& fields) {
        const auto id = idOf(find(fields.top, "id"));
        return id ? std::to_string(*id) : std::string{};
    }

    /// @brief Spells the type of an entry for a warning; empty when it has none.
    static std::string typeText(const MaskFields& fields) {
        const FieldValue* type = find(fields.top, "type");
        return type != nullptr && type->text ? *type->text : std::string{};
    }

private:
    LocalAdjustment build() {
        if (!fields_.problem.empty()) {
            throw Drop{fields_.problem};
        }
        const FieldValue* type = find(fields_.top, "type");
        if (type == nullptr) {
            throw Drop{"it has no type"};
        }
        if (!type->text || type->structured) {
            throw Drop{"its type is not text"};
        }
        if (*type->text != "linear" && *type->text != "radial" && *type->text != "brush") {
            throw Drop{std::format("the type '{}' is not one this arraw knows", *type->text)};
        }
        const FieldValue* idField = find(fields_.top, "id");
        if (idField == nullptr) {
            throw Drop{"it has no id"};
        }
        const auto id = idOf(idField);
        if (!id) {
            throw Drop{"its id is not a whole number from 1 to 4294967294"};
        }
        LocalAdjustment adjustment;
        adjustment.id = LocalAdjustmentId{*id};
        for (const auto& [key, value] : fields_.top) {
            if (std::ranges::find(topKeys, key) == topKeys.end()) {
                ignored(key);
            }
        }
        if (const FieldValue* name = find(fields_.top, "name")) {
            if (name->structured || !name->text) {
                throw Drop{"its name is not text"};
            }
            adjustment.name = storableMaskName(*name->text);
        }
        adjustment.enabled = flag("enabled", true);
        adjustment.invert = flag("invert", false);
        if (const FieldValue* opacity = find(fields_.top, "opacity")) {
            adjustment.opacity = clamped(number(opacity, "opacity"), 0.0, 1.0, "opacity");
        }
        if (*type->text != "brush" && (fields_.strokes || !fields_.strokesProblem.empty())) {
            ignored("strokes");
        }
        if (*type->text == "linear") {
            adjustment.shape = linear();
        } else if (*type->text == "radial") {
            adjustment.shape = radial();
        } else {
            adjustment.shape = brush();
        }
        deltas(adjustment.deltas);
        return adjustment;
    }

    bool flag(std::string_view key, bool otherwise) {
        const FieldValue* field = find(fields_.top, key);
        if (field == nullptr) {
            return otherwise;
        }
        if (field->structured || !field->flag) {
            throw Drop{std::format("'{}' is not true or false", key)};
        }
        return *field->flag;
    }

    /// @brief Reads a finite number from a field, or drops the mask.
    static double number(const FieldValue* field, std::string_view key) {
        if (field == nullptr) {
            throw Drop{std::format("'{}' is missing", key)};
        }
        if (field->structured || !field->number) {
            throw Drop{std::format("'{}' is not a number", key)};
        }
        if (!std::isfinite(*field->number)) {
            throw Drop{std::format("'{}' is not a finite number", key)};
        }
        return *field->number;
    }

    /// @brief Reports a value that was brought into its range.
    void clampedNote(std::string_view key, double value, double used) {
        log_.record(
            {.notice = Notice::SettingClamped,
             .severity = Severity::Warning,
             .subject = subject_,
             .values = {std::format("localAdjustments[{}].{}", position_, key), value, used}});
    }

    /// @brief Brings a number into a range, reporting it if it had to move.
    float clamped(double value, double low, double high, std::string_view key) {
        const double used = std::clamp(value, low, high);
        if (used != value) {
            clampedNote(key, value, used);
        }
        return shortestFloat(used);
    }

    void ignored(std::string_view key) {
        log_.record({.notice = Notice::LocalAdjustmentFieldIgnored,
                     .severity = Severity::Warning,
                     .subject = subject_,
                     .values = {static_cast<double>(position_), idText_, std::string(key)}});
    }

    /// @brief Reads a position, clamping it.
    float position(std::string_view key) {
        return clamped(number(find(fields_.geometry, key), key), minimumMaskPosition,
                       maximumMaskPosition, key);
    }

    /// @brief Reports every geometry field that is not among a kind's.
    template <std::size_t N> void ignoreOtherGeometry(const std::array<std::string_view, N>& own) {
        for (const auto& [key, value] : fields_.geometry) {
            if (std::ranges::find(own, key) == own.end()) {
                ignored(key);
            }
        }
    }

    LinearMask linear() {
        ignoreOtherGeometry(linearKeys);
        LinearMask mask;
        mask.from = {position("fromX"), position("fromY")};
        mask.to = {position("toX"), position("toY")};
        if (std::hypot(static_cast<double>(mask.to.u) - mask.from.u,
                       static_cast<double>(mask.to.v) - mask.from.v) < minimumMaskExtent) {
            throw Drop{"its ends are closer together than a mask needs"};
        }
        return mask;
    }

    float radius(std::string_view key) {
        const double value = number(find(fields_.geometry, key), key);
        // Compared as the float the mask holds, which is what the minimum is.
        const float held = shortestFloat(std::clamp(value, -1e30, 1e30));
        if (held < minimumMaskExtent) {
            throw Drop{std::format("'{}' is below the {} a mask needs", key, minimumMaskExtent)};
        }
        if (held > maximumMaskRadius) {
            clampedNote(key, value, maximumMaskRadius);
            return maximumMaskRadius;
        }
        return held;
    }

    RadialMask radial() {
        ignoreOtherGeometry(radialKeys);
        RadialMask mask;
        mask.centre = {position("centreX"), position("centreY")};
        mask.radiusX = radius("radiusX");
        mask.radiusY = radius("radiusY");
        const double angle = number(find(fields_.geometry, "angle"), "angle");
        const double wrapped = wrapDegrees(angle);
        if (wrapped != angle) {
            clampedNote("angle", angle, wrapped);
        }
        mask.angle = shortestFloat(wrapped);
        if (mask.angle >= 180.0F) {
            mask.angle = -180.0F;
        }
        mask.feather =
            clamped(number(find(fields_.geometry, "feather"), "feather"), 0.0, 1.0, "feather");
        return mask;
    }

    /// @brief How many strokes of each kind of trouble a brush had.
    struct StrokeTally {
        std::size_t unreadable = 0; ///< Strokes that could not be read.
        std::size_t pointsCut = 0;  ///< Strokes kept with points cut.
        std::size_t pastCap = 0;    ///< Strokes past the cap.
        std::size_t pastBudget = 0; ///< Strokes past a budget.
        std::size_t budgetFrom = 0; ///< Position (from 1) of the first stroke past a budget.
        std::string budgetName;     ///< Name of the budget that was passed.
        std::size_t radius = 0;     ///< Strokes kept with a radius clamped.
        std::size_t hardness = 0;   ///< Strokes kept with a hardness clamped.
        std::size_t flow = 0;       ///< Strokes kept with a flow clamped.
        std::size_t positions = 0;  ///< Strokes kept with a position clamped.
    };

    /// @brief Names the budget a stroke total passes.
    static std::string budgetOverrun(const StrokeBudget& total) {
        return total.points > maximumPointsPerMask         ? "points"
               : total.sweptArea > maximumSweptAreaPerMask ? "swept-area"
                                                           : "dab";
    }

    /// @brief Reports a count of strokes trimmed, once.
    void trimmed(std::size_t count, std::string words) {
        if (count == 0) {
            return;
        }
        log_.record({.notice = Notice::BrushStrokesTrimmed,
                     .severity = Severity::Warning,
                     .subject = subject_,
                     .values = {static_cast<double>(position_), idText_, static_cast<double>(count),
                                std::move(words)}});
    }

    /// @brief Words a count of strokes ("1 stroke", "3 strokes").
    static std::string strokesWord(std::size_t count) {
        return count == 1 ? "1 stroke" : std::format("{} strokes", count);
    }

    /// @brief Brings a number into a range, noting in @p moved if it moved.
    static void clampInto(float& value, float low, float high, bool& moved) {
        const float used = std::clamp(value, low, high);
        if (used != value) {
            value = used;
            moved = true;
        }
    }

    /// @brief Reads a brush's geometry and strokes, applying the reading rules of ADR 044.
    BrushMask brush() {
        const FieldValue* rasteriser = find(fields_.geometry, "rasteriser");
        if (rasteriser == nullptr || rasteriser->structured || !rasteriser->number ||
            !std::isfinite(*rasteriser->number) ||
            *rasteriser->number != std::trunc(*rasteriser->number)) {
            throw Drop{"its rasteriser is missing or not a whole number"};
        }
        if (*rasteriser->number != static_cast<double>(brushRasteriserVersion)) {
            throw Drop{std::format("unknown rasteriser {}", *rasteriser->number)};
        }
        if (find(fields_.geometry, "strokes") != nullptr) {
            throw Drop{"'strokes' is not a sequence of strokes"};
        }
        ignoreOtherGeometry(brushKeys);
        if (!fields_.strokesProblem.empty()) {
            throw Drop{fields_.strokesProblem};
        }
        BrushMask mask;
        if (!fields_.strokes) {
            return mask;
        }
        StrokeTally tally;
        std::vector<Stroke> kept;
        StrokeBudget total;
        bool stopped = false;
        std::set<std::string> unknown;
        for (std::size_t i = 0; i < fields_.strokes->size(); ++i) {
            const StrokeFields& stored = (*fields_.strokes)[i];
            if (i >= maximumStrokesPerMask) {
                ++tally.pastCap;
                continue;
            }
            unknown.insert(stored.unknownKeys.begin(), stored.unknownKeys.end());
            if (stopped) {
                ++tally.pastBudget;
                continue;
            }
            if (!stored.decoded) {
                ++tally.unreadable;
                continue;
            }
            Stroke stroke = stored.decoded->stroke;
            const bool finite = std::isfinite(stroke.radius) && std::isfinite(stroke.hardness) &&
                                std::isfinite(stroke.flow) &&
                                std::ranges::all_of(stroke.points, [](const SensorPoint& point) {
                                    return std::isfinite(point.u) && std::isfinite(point.v);
                                });
            if (!finite || stroke.points.empty()) {
                ++tally.unreadable;
                continue;
            }
            bool movedRadius = false;
            bool movedHardness = false;
            bool movedFlow = false;
            bool movedPosition = false;
            clampInto(stroke.radius, minimumBrushRadius, maximumBrushRadius, movedRadius);
            clampInto(stroke.hardness, 0.0F, 1.0F, movedHardness);
            clampInto(stroke.flow, 0.0F, 1.0F, movedFlow);
            for (SensorPoint& point : stroke.points) {
                clampInto(point.u, minimumMaskPosition, maximumMaskPosition, movedPosition);
                clampInto(point.v, minimumMaskPosition, maximumMaskPosition, movedPosition);
            }
            const StrokeBudget own = budgetOf(stroke);
            StrokeBudget next = total;
            next += own;
            bool alone = true;
            try {
                stroke = normalised(std::move(stroke));
            } catch (const std::invalid_argument&) {
                alone = false;
            }
            if (!alone || !next.withinMaskLimits()) {
                stopped = true;
                tally.budgetFrom = i + 1;
                tally.budgetName = budgetOverrun(alone ? next : own);
                ++tally.pastBudget;
                continue;
            }
            // Counted only now, so a stroke that is dropped is not reported as trimmed too.
            tally.pointsCut += stored.decoded->pointsDropped > 0 ? 1 : 0;
            tally.radius += movedRadius ? 1 : 0;
            tally.hardness += movedHardness ? 1 : 0;
            tally.flow += movedFlow ? 1 : 0;
            tally.positions += movedPosition ? 1 : 0;
            total = next;
            kept.push_back(std::move(stroke));
        }
        trimmed(tally.unreadable, strokesWord(tally.unreadable) + " could not be read");
        trimmed(tally.pointsCut, std::format("points past {} cut from {}", maximumStrokePoints,
                                             strokesWord(tally.pointsCut)));
        trimmed(tally.pastCap, std::format("strokes from {} on dropped: past the {} a mask holds",
                                           maximumStrokesPerMask + 1, maximumStrokesPerMask));
        trimmed(tally.pastBudget, std::format("strokes from {} on dropped: past the {} budget",
                                              tally.budgetFrom, tally.budgetName));
        trimmed(tally.radius, std::format("radius clamped in {}", strokesWord(tally.radius)));
        trimmed(tally.hardness, std::format("hardness clamped in {}", strokesWord(tally.hardness)));
        trimmed(tally.flow, std::format("flow clamped in {}", strokesWord(tally.flow)));
        trimmed(tally.positions,
                std::format("positions clamped in {}", strokesWord(tally.positions)));
        for (const std::string& key : unknown) {
            ignored("strokes[]." + key);
        }
        mask.strokes = std::make_shared<const StrokeList>(std::move(kept), brushRasteriserVersion);
        return mask;
    }

    void deltas(LocalDeltas& into) {
        for (const auto& [key, value] : fields_.deltas) {
            const LocalDescriptor* descriptor = findLocalDescriptor(key);
            if (descriptor == nullptr) {
                ignored(key);
                continue;
            }
            into.*descriptor->member = clamped(number(&value, key), descriptor->range.minimum,
                                               descriptor->range.maximum, key);
        }
    }

    std::size_t position_;
    const MaskFields& fields_;
    DiagnosticLog& log_;
    const std::optional<std::filesystem::path>& subject_;
    std::string idText_;
    std::string typeText_;
};

} // namespace

FieldPart arraw::partOfKey(std::string_view key) noexcept {
    if (std::ranges::find(topKeys, key) != topKeys.end()) {
        return FieldPart::Top;
    }
    if (std::ranges::find(brushKeys, key) != brushKeys.end() || key == "strokes" ||
        std::ranges::find(linearKeys, key) != linearKeys.end() ||
        std::ranges::find(radialKeys, key) != radialKeys.end()) {
        return FieldPart::Geometry;
    }
    return FieldPart::Delta;
}

ReadLocalAdjustments
arraw::readLocalAdjustments(std::span<const MaskFields> entries, std::optional<double> storedNext,
                            DiagnosticLog& log,
                            const std::optional<std::filesystem::path>& subject) {
    ReadLocalAdjustments result;
    std::set<std::uint32_t> seen;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t position = i + 1;
        const auto drop = [&](std::string reason) {
            log.record({.notice = Notice::LocalAdjustmentDropped,
                        .severity = Severity::Warning,
                        .subject = subject,
                        .values = {static_cast<double>(position), MaskReader::idText(entries[i]),
                                   MaskReader::typeText(entries[i]), std::move(reason)}});
        };
        if (i >= maximumLocalAdjustments) {
            drop(std::format("a photograph holds at most {} masks", maximumLocalAdjustments));
            continue;
        }
        auto adjustment = MaskReader(position, entries[i], log, subject).read();
        if (!adjustment) {
            continue;
        }
        if (!seen.insert(adjustment->id.value).second) {
            drop(std::format("the id {} is used by an earlier mask", adjustment->id.value));
            continue;
        }
        result.adjustments.push_back(std::move(*adjustment));
    }
    std::uint32_t above = 1;
    for (const LocalAdjustment& adjustment : result.adjustments) {
        above = std::max(above, adjustment.id.value + 1);
    }
    if (storedNext) {
        if (!std::isfinite(*storedNext) || *storedNext != std::trunc(*storedNext) ||
            *storedNext < 1.0) {
            log.record({.notice = Notice::SettingMalformed,
                        .severity = Severity::Warning,
                        .subject = subject,
                        .values = {std::string("nextLocalAdjustmentId"),
                                   std::string("a whole number of at least 1")}});
        } else {
            above = std::max(
                above,
                static_cast<std::uint32_t>(std::min(
                    *storedNext, static_cast<double>(std::numeric_limits<std::uint32_t>::max()))));
        }
    }
    result.next = LocalAdjustmentId{above};
    return result;
}

WrittenMask arraw::writtenForm(const LocalAdjustment& adjustment) {
    const auto num = [](float value) { return WrittenValue{shortestDouble(value)}; };
    WrittenMask written;
    written.top = {{"id", WrittenValue{static_cast<double>(adjustment.id.value)}},
                   {"type", WrittenValue{std::string(maskTypeName(adjustment.shape))}},
                   {"name", WrittenValue{adjustment.name}},
                   {"enabled", WrittenValue{adjustment.enabled}},
                   {"opacity", num(adjustment.opacity)},
                   {"invert", WrittenValue{adjustment.invert}}};
    if (const auto* linear = std::get_if<LinearMask>(&adjustment.shape)) {
        written.geometry = {{"fromX", num(linear->from.u)},
                            {"fromY", num(linear->from.v)},
                            {"toX", num(linear->to.u)},
                            {"toY", num(linear->to.v)}};
    } else if (const auto* brush = std::get_if<BrushMask>(&adjustment.shape)) {
        written.strokes = brush->strokes ? brush->strokes : emptyStrokeList();
        written.geometry = {
            {"rasteriser", WrittenValue{static_cast<double>(written.strokes->rasteriser())}}};
    } else {
        const auto& radial = std::get<RadialMask>(adjustment.shape);
        written.geometry = {{"centreX", num(radial.centre.u)}, {"centreY", num(radial.centre.v)},
                            {"radiusX", num(radial.radiusX)},  {"radiusY", num(radial.radiusY)},
                            {"angle", num(radial.angle)},      {"feather", num(radial.feather)}};
    }
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        const float delta = adjustment.deltas.*descriptor.member;
        if (delta != 0.0F) {
            written.deltas.emplace_back(std::string(descriptor.key), num(delta));
        }
    }
    return written;
}
