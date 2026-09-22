#include "ProcessingPlan.h"

using namespace arraw;

ProcessingPlan arraw::planFor(const ColorEncoding& encoding, const DevelopSettings& settings) {
    ProcessingPlan plan = tonePlanFor(settings.tone);
    plan.toWorking = colorMatrixFor(encoding, settings.color);
    return plan;
}

ProcessingPlan arraw::planFor(const Photo& photo) {
    auto plan = planFor(photo.metadata().encoding, photo.settings());
    plan.geometry = geometryPlanFor(photo.metadata().size, photo.metadata().orientation,
                                    photo.settings().geometry);
    return plan;
}

ProcessingPlan arraw::planFor(const ImageBuffer& source, const DevelopSettings& settings) {
    auto plan = planFor(source.encoding(), settings);
    plan.geometry = geometryPlanFor(source.size(), source.orientation(), settings.geometry);
    return plan;
}
