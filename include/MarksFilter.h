#pragma once

#include <PhotoMarks.h>

#include <set>

namespace arraw {

/// @brief Which culling marks a shot must have to be wanted.
///
/// The one definition of "does this shot match", for the command line, Python
/// and the film strip (ADR 029). The two dimensions follow different rules
/// because they are different kinds of thing: a rating is ordered, so it is a
/// threshold; a colour is not, so the labels are a set matched by OR; a reject
/// can never satisfy "at least N stars", so it has its own switch; and the two
/// dimensions combine by AND, each narrowing the set.
struct MarksFilter {
    /// @brief Fewest stars wanted: 0 for no constraint, 1 to ::arraw::highestRating for "at least
    /// N".
    ///
    /// Excludes rejects and unrated photographs.
    int minRating = 0;

    /// @brief Whether only rejected photographs are wanted; excludes ::MarksFilter::minRating.
    bool rejectsOnly = false;

    /// @brief Colour labels wanted, any one of them; empty for any label, or none.
    std::set<ColorLabel> labels;

    /// @brief Tells whether the filter narrows anything.
    /// @return `true` unless every mark matches.
    [[nodiscard]] bool isActive() const {
        return rejectsOnly || minRating > 0 || !labels.empty();
    }

    /// @brief Checks that the filter says something coherent.
    /// @throws std::invalid_argument if both ::MarksFilter::rejectsOnly and a
    /// ::MarksFilter::minRating are set, or the rating is outside 0 to ::arraw::highestRating.
    void validate() const;

    /// @brief Tells whether a photograph's marks pass the filter.
    /// @param marks Marks to test.
    /// @return `true` if the rating and the label are both wanted.
    /// @throws std::invalid_argument as ::MarksFilter::validate does.
    [[nodiscard]] bool matches(const PhotoMarks& marks) const;

    friend bool operator==(const MarksFilter&, const MarksFilter&) = default;
};

} // namespace arraw
