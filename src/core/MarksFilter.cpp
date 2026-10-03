#include "MarksFilter.h"

#include <stdexcept>
#include <string>

using namespace arraw;

void MarksFilter::validate() const {
    if (rejectsOnly && minRating > 0) {
        throw std::invalid_argument("a filter cannot want rejects only and a minimum rating too");
    }
    if (minRating < 0 || minRating > highestRating) {
        throw std::invalid_argument("the minimum rating must be 0 to " +
                                    std::to_string(highestRating));
    }
}

bool MarksFilter::matches(const PhotoMarks& marks) const {
    validate();
    // With no threshold even a reject is wanted, which `rating >= 0` would leave out.
    const bool ratingWanted =
        rejectsOnly ? marks.rating == rejectedRating : minRating <= 0 || marks.rating >= minRating;
    const bool labelWanted = labels.empty() || (marks.label && labels.contains(*marks.label));
    return ratingWanted && labelWanted;
}
