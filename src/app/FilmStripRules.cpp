#include "FilmStripRules.h"

#include <algorithm>

namespace arraw::app {

int cellSide(int availableHeight) {
    return std::max(minimumCellContent, availableHeight - 2 * cellPadding) + 2 * cellPadding;
}

std::optional<int> adjacentRow(int count, int current, int delta) {
    if (count <= 0) {
        return std::nullopt;
    }
    if (current < 0 || current >= count) {
        return delta >= 0 ? 0 : count - 1;
    }
    const long next = static_cast<long>(current) + delta;
    if (next < 0 || next >= count) {
        return std::nullopt;
    }
    return static_cast<int>(next);
}

std::optional<ColorLabel> toggledLabel(const std::vector<PhotoMarks>& marks, ColorLabel label) {
    const bool allHaveIt =
        !marks.empty() &&
        std::ranges::all_of(marks, [&](const PhotoMarks& each) { return each.label == label; });
    if (allHaveIt) {
        return std::nullopt;
    }
    return label;
}

MarksFilter filterOf(int starChoice, const std::set<ColorLabel>& labels) {
    MarksFilter filter;
    if (starChoice == rejectsOnlyChoice) {
        filter.rejectsOnly = true;
    } else if (starChoice >= 1 && starChoice <= highestRating) {
        filter.minRating = starChoice;
    }
    filter.labels = labels;
    return filter;
}

QColor labelColour(ColorLabel label) {
    switch (label) {
    case ColorLabel::Red:
        return {0xD6, 0x45, 0x41};
    case ColorLabel::Yellow:
        return {0xF5, 0xD8, 0x20};
    case ColorLabel::Green:
        return {0x4C, 0xAF, 0x50};
    case ColorLabel::Blue:
        return {0x3B, 0x82, 0xF6};
    case ColorLabel::Purple:
        return {0x9B, 0x59, 0xB6};
    }
    return {};
}

const char* labelName(ColorLabel label) {
    for (const auto& [value, name] : colorLabelNames) {
        if (value == label) {
            return name.data();
        }
    }
    return "";
}

} // namespace arraw::app
