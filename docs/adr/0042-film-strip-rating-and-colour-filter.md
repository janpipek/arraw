# Filtering the film strip by rating and colour label

The film strip already lets you cull — rate (0–5, X to reject) and colour-label
(R/Y/G/B/P) every shot in a directory, written straight through to the XMP
sidecar (ADR 0008). The missing half is *acting on those marks while you browse*:
narrowing the strip to the keepers, or to one colour group, so the next pass
works on a smaller set. This adds an in-strip filter over the two cull
dimensions, with deliberately **different** matching rules for each because the
two dimensions are different kinds of thing.

## The filter

A small headless value type is the single source of truth for "does this shot
match", testable with no Qt view:

```cpp
struct FilmStripFilter {
    int minRating = 0;          // 0 = off; 1..5 = "≥ N stars"
    bool rejectsOnly = false;   // the ✗ slot; mutually exclusive with minRating
    QSet<ColourLabel> colours;  // empty = any colour; else OR over members
    bool isActive() const;
    bool matches(const UserMetadata&) const;
};
```

The matching rules differ per dimension, and that asymmetry is the point:

- **Rating is a threshold, not an equality.** Ratings are ordered, and the cull
  question is "show me the keepers", so picking 3★ matches 3, 4, and 5 (`rating
  >= minRating`). `minRating == 0` imposes no star constraint. Rejects (rating
  −1) are excluded by any `minRating >= 1`.
- **Colour is an unordered set, matched by OR.** Labels have no order, so there
  is no threshold to apply; instead you toggle any subset of the five colours and
  a shot matches if it carries *any* of them. An empty set means "any colour".
- **Rejects get a dedicated slot.** Because `≥ N` can never select rejects, the
  star control has a separate ✗ state (`rejectsOnly`) that matches *only* rejects
  — the "review what I tossed before deleting" view. It is mutually exclusive with
  a star threshold.
- **The two dimensions combine with AND.** `≥3★ AND (Red OR Green)` narrows to a
  4★ Green shot and hides a 4★ Blue or a 2★ Red one. AND (not OR) because each
  dimension is meant to *narrow* the set, matching Lightroom and the cull intent.

## Architecture: a filter proxy under the view

A `QSortFilterProxyModel` subclass sits between `FilmStripModel` (unchanged,
still the source of paths/marks/thumbnails) and the `QListView`. Its
`filterAcceptsRow` reads `RatingRole`/`LabelRole` from the source and defers to
`FilmStripFilter::matches`. `setFilter()` stores the filter and calls
`invalidateFilter()`. Mark writes already emit `dataChanged`, so re-rating a shot
re-evaluates its row automatically — no extra plumbing.

The cost this imposes, and the reason it is recorded here, is **index-space
discipline in `FilmStrip`**. The widget today freely mixes source rows and view
indexes (`indexForPath`, `navigateBy`, `setCurrentFile`, `selectFirst`,
`requestVisibleThumbnails`). With a proxy interposed, every crossing of the view
boundary must `mapToSource` / `mapFromSource`, and navigation must run in *proxy*
coordinates so ±1 lands on the next **visible** match and `indexForPath` returns
invalid for a filtered-out path. This is the bulk of the implementation work and
the main regression risk.

## Behaviour when the active image stops matching

Non-matching shots are **hidden** (the strip collapses to the matches), the
standard filmstrip behaviour. The hard case is the *active* image — the one in
the viewport — ceasing to match, which happens two ways, both resolved by one
"select nearest still-visible row" helper:

- **You change the filter** and the open image no longer qualifies → jump to the
  nearest remaining match (prefer forward) and load it. The strip and viewport
  stay in sync rather than leaving an orphaned, unhighlighted image.
- **You re-rate the active image out of range** (press `2` while filtering ≥3★,
  or `X` to reject) → auto-advance forward to the next match. This deliberately
  turns rate-down/reject into a culling accelerator: mark, advance, mark, advance.

If nothing matches, the selection clears and the viewport keeps its last image;
the list paints a centred "no shots match" hint.

We considered keeping the active image pinned in the strip until you navigate
away (avoids any mid-edit vanish) and rejected it: it complicates the proxy with
a per-row exemption, and the auto-advance cull is worth more than the occasional
surprise jump.

## UI and lifetime

The controls live **inline in the existing dock title bar**, after the path
label: a star segment `✗ | 1 | 2 | 3 | 4 | 5`, five checkable colour swatches
tinted from the existing `labelColour()` palette (lifted out of `FilmStrip.cpp`'s
anonymous namespace so the swatches and the cell delegate share one definition —
colours defined once, per the single-source rule), and a Clear button enabled
only while a filter is active. The path label elides so the controls always fit.

The star buttons are **toggles with single-selection**: at most one is active,
clicking another moves the selection, and clicking the active one again clears
the rating filter. This makes a separate "off"/"All" button unnecessary — the
absence of a checked star *is* "any rating". (An earlier draft had an explicit
`off` slot; removing it is fewer controls for the same states.)

The filter is **session-sticky but not persisted**: it survives switching
folders (it is a viewing mode, not a property of one directory) but each launch
starts unfiltered, so the app never opens to a mysteriously empty strip. It is
therefore *not* written to `QSettings`/window state and *not* cleared in
`setDirectory`.

## Deferred

- **No "N of M shown" hidden-count badge** in the title bar — trivial to add
  later once the layout is settled.
- **No keyboard shortcut** to toggle the filter on/off yet.
- **No "unrated only" or "no label" selector** — `off` means "any rating" and an
  empty colour set means "any colour"; finding the *un-marked* shots is a
  separate inverse query we are not adding now.

## Testing

The predicate and the proxy are headless-testable and get unit coverage: the
`FilmStripFilter::matches` truth table (thresholds, rejects-only, colour OR sets,
star×colour AND), and the proxy over a seeded model (visible-row counts, and that
a mark write re-filters). Navigation is asserted to skip hidden rows. The
jump/auto-advance feel and the title-bar widget are GUI-level — verified by
running the app — consistent with "headless-test the math, run the app for feel".
