#include "FilmStripRules.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The rules of the film strip that need no widget (src/app/FilmStripRules.h).

TEST_CASE("A cell is a square that follows the strip's height", "[filmstrip]") {
    // The content is the height less the padding on both sides, and the cell adds it back.
    CHECK(cellSide(132) == 132);
    CHECK(cellSide(200) == 200);
    CHECK(cellSide(minimumCellContent + 2 * cellPadding) == minimumCellContent + 2 * cellPadding);
}

TEST_CASE("A cell never gets smaller than its minimum content", "[filmstrip]") {
    const int smallest = minimumCellContent + 2 * cellPadding;
    CHECK(cellSide(0) == smallest);
    CHECK(cellSide(-50) == smallest);
    CHECK(cellSide(smallest - 1) == smallest);
}

TEST_CASE("Stepping moves to the neighbour among the shots shown", "[filmstrip]") {
    CHECK(adjacentRow(5, 2, 1) == 3);
    CHECK(adjacentRow(5, 2, -1) == 1);
    CHECK(adjacentRow(5, 0, 1) == 1);
    CHECK(adjacentRow(5, 4, -1) == 3);
}

TEST_CASE("Stepping stops at either end of the shots shown", "[filmstrip]") {
    CHECK_FALSE(adjacentRow(5, 4, 1).has_value());
    CHECK_FALSE(adjacentRow(5, 0, -1).has_value());
    CHECK_FALSE(adjacentRow(1, 0, 1).has_value());
    CHECK_FALSE(adjacentRow(1, 0, -1).has_value());
}

TEST_CASE("Stepping from no active shot goes to the first or the last", "[filmstrip]") {
    CHECK(adjacentRow(5, -1, 1) == 0);
    CHECK(adjacentRow(5, -1, -1) == 4);
    // An active shot the filter hides is no position either.
    CHECK(adjacentRow(5, 9, 1) == 0);
}

TEST_CASE("Stepping with no shots shown goes nowhere", "[filmstrip]") {
    CHECK_FALSE(adjacentRow(0, -1, 1).has_value());
    CHECK_FALSE(adjacentRow(0, 0, -1).has_value());
}

TEST_CASE("A label key sets the label unless every shot has it", "[filmstrip]") {
    const PhotoMarks none{};
    const PhotoMarks red{.rating = 3, .label = ColorLabel::Red};
    const PhotoMarks blue{.rating = 0, .label = ColorLabel::Blue};

    // One shot: a toggle.
    CHECK(toggledLabel({none}, ColorLabel::Red) == ColorLabel::Red);
    CHECK_FALSE(toggledLabel({red}, ColorLabel::Red).has_value());
    CHECK(toggledLabel({blue}, ColorLabel::Red) == ColorLabel::Red);

    // Several: removed only when all have it.
    CHECK_FALSE(toggledLabel({red, red}, ColorLabel::Red).has_value());
    CHECK(toggledLabel({red, none}, ColorLabel::Red) == ColorLabel::Red);
    CHECK(toggledLabel({red, blue}, ColorLabel::Red) == ColorLabel::Red);
}

TEST_CASE("A label key on nothing gives the label", "[filmstrip]") {
    CHECK(toggledLabel({}, ColorLabel::Green) == ColorLabel::Green);
}

TEST_CASE("The filter controls make the filter they stand for", "[filmstrip]") {
    CHECK_FALSE(filterOf(0, {}).isActive());
    CHECK(filterOf(3, {}).minRating == 3);
    CHECK_FALSE(filterOf(3, {}).rejectsOnly);
    CHECK(filterOf(rejectsOnlyChoice, {}).rejectsOnly);
    CHECK(filterOf(rejectsOnlyChoice, {}).minRating == 0);

    const std::set<ColorLabel> labels{ColorLabel::Red, ColorLabel::Blue};
    const MarksFilter filter = filterOf(2, labels);
    CHECK(filter.minRating == 2);
    CHECK(filter.labels == labels);
    CHECK_NOTHROW(filter.validate());
    CHECK_NOTHROW(filterOf(rejectsOnlyChoice, labels).validate());
}

TEST_CASE("Choices outside the filter's range filter nothing by rating", "[filmstrip]") {
    CHECK_FALSE(filterOf(-1, {}).isActive());
    CHECK_FALSE(filterOf(7, {}).isActive());
}

TEST_CASE("Every label has its own colour and name", "[filmstrip]") {
    std::set<QRgb> colours;
    std::set<std::string> names;
    for (const auto& [label, name] : colorLabelNames) {
        CHECK(labelColour(label).isValid());
        colours.insert(labelColour(label).rgb());
        names.insert(labelName(label));
    }
    CHECK(colours.size() == colorLabelNames.size());
    CHECK(names.size() == colorLabelNames.size());
}
