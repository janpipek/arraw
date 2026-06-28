#pragma once
#include "develop/UserMetadata.h"
#include <QSet>

// The film strip's viewing filter over the two cull dimensions (ADR 0042).
// A headless value type: the single source of truth for "does this shot match",
// testable with no model or view.
//
// The two dimensions match by deliberately different rules, because they are
// different kinds of thing:
//   * rating is ordered, so it is a threshold ("≥ N stars");
//   * colour is unordered, so it is a set matched by OR ("any of these");
//   * rejects (rating -1) can never satisfy ≥ N, so they get their own slot;
//   * the two dimensions combine with AND (each narrows the set).
struct FilmStripFilter {
    int minRating = 0;         // 0 = no star constraint; 1..5 = "rating >= N"
    bool rejectsOnly = false;  // match only rejects; mutually exclusive with minRating
    QSet<ColourLabel> colours; // empty = any colour; else OR over members

    bool isActive() const { return rejectsOnly || minRating > 0 || !colours.isEmpty(); }

    bool matchesRating(int rating) const {
        if (rejectsOnly)
            return rating == -1;
        if (minRating <= 0)
            return true;
        return rating >= minRating; // excludes rejects (-1) and unrated (0)
    }

    bool matchesColour(ColourLabel label) const {
        return colours.isEmpty() || colours.contains(label);
    }

    bool matches(const UserMetadata& m) const {
        return matchesRating(m.rating) && matchesColour(m.label);
    }

    bool operator==(const FilmStripFilter&) const = default;
};
