#include "ProcessingPlan.h"

using namespace arraw;

ProcessingPlan arraw::planFor(const ColorEncoding& encoding, const DevelopSettings& settings) {
    ProcessingPlan plan = tonePlanFor(settings.tone);
    plan.toWorking = colorMatrixFor(encoding, settings.color);
    return plan;
}

ProcessingPlan arraw::planFor(const Photo& photo) {
    return planFor(photo.metadata().encoding, photo.settings());
}
