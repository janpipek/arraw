# Review: brush-cache-ratio2 (superseded Extended parents)

- Reviewer: claude-opus-5-5
- Date: 2026-10-16
- Scope: `git diff main` on branch `brush-cache-ratio2` (uncommitted): `src/core/BrushCoverageCache.{h,cpp}`,
  `tests/test_BrushCoverageCache.cpp`, `tests/test_MainWindowMasks.cpp`, ADR 044, `docs/todo.md`.
- Method: read the code; ran `[brush][cache]` (debug, 27 cases pass); mutation-tested the four new tests
  by relinking `arraw-tests` against mutated copies of `BrushCoverageCache.cpp` in the scratchpad (no
  production file was touched); ran the release timing session once at `QT_SCALE_FACTOR=2`;
  `just format-check` is clean.

## Verdict

The cache change is correct and matches the Option A design. I found no bug under the mutex, in the byte
accounting, in the "Grown replaces parent" and dedupe rules, or in the GPU serial path. Nothing in the code
has to be fixed before merging. Two things should be fixed first: the ADR note is in the wrong place (it
splits the 2026-10-11 section from its table), and one of the new tests does not test what its name says.
The measurements support "the ratio-2 misses are gone". They support "the rest is render cost" only partly.

My run at ratio 2, release, CPU: fit miss 0, evictions 38, 511.4 MiB, commit median 507 ms and p95
527 ms, undo median 414 ms (20 of 20 Hits), latency p95 104 ms. fit+tint miss 16, evictions 8. 1:1 miss
15, evictions 136. All 197 assertions pass. These match the implementation report.

## Correctness (verified)

- **Locking.** Every `superseded` read or write happens under `mutex_`: in `find`, the first `coverage`
  block, and `insert`, which is called with the lock held. `best` points into `entries_` and is only used
  inside the same lock scope, with no change to the vector in between. `extended` is a `shared_ptr` held
  across the unlocked draw, so `extends` cannot be a reused address (no ABA), just as `parent` cannot.
  - Benign race: another thread can make the base live (an undo Hit) between the two locks, and the
    insert then marks it superseded again. The only effect is eviction priority. Not worth fixing.
- **Byte accounting.** A settled grid is retained tile by tile, on top of `tiles`. So
  `releaseTiles(*settled)` on the base drops only the references the settled grid added, and the tiles it
  shares with `tiles` stay counted. After that `settled` is null, so a later `releaseEntry` releases only
  `tiles`. That is consistent. The test "Extending releases the settled grid" checks the exact byte total,
  and a mutant that skips the release fails four tests.
- **Dedupe and "Grown replaces parent".** `parent` and `extended` never both apply: the call passes
  `parent ? nullptr : extended.get()`. `extends` can never equal `entry.list` (the prefix is strictly
  shorter), so the duplicate erase cannot remove the base. A superseded entry has no settled grid, so
  `growthBase` never picks it, and a Grown never "replaces" a superseded entry. The ContentHit copy
  (`retain`) starts with `superseded = false`, which is right because the entry it was copied from was
  just cleared.
- **Eviction comparator.** `a.superseded != b.superseded ? a.superseded : a.lastUse < b.lastUse` is a
  strict weak order: superseded entries first, then LRU within each group. The new entry, at `end()-1`,
  is still excluded.
- **Losing the settled grid costs no Grown in the app.** The base of an Extended has a finished last
  stroke, because the live stroke's updates are Grown from the previous update and replace it. So nothing
  in the app's flow would ever have grown from the base's settled grid.
- **GPU tile serials.** No tile is mutated. Releasing a settled grid only drops references. An Extended
  draws on `base->tiles`, so tiles it does not touch keep their serials. Nothing changes for the upload
  path.
- **Callers.** `DevelopedFrame.cpp` and `BrushCoverage.cpp` call `find`/`coverage` with the brush's
  current list. A `find` clears the flag only on that list, never on an old base during a stroke.

## Findings

1. **Medium, docs: the ADR note splits a section.**
   `docs/adr/044-...md:1214` puts the 2026-10-16 note between the 2026-10-11 Q6 (b) prose and its
   "Measured as above (...)" table. The table and "as above" now seem to refer to the new note, and its
   numbers (commit 962 ms and so on) contradict it. The design asked for the note under the **Cache.**
   paragraph.
   - Fix: move the note to after "Re-measured after the review fixes" (around line 1245), or at least
     after the **Cache.** paragraph.
   - In the same edit, update the **Cache.** paragraph. Its sentence "The session reports rather than
     asserts the `Miss`es at ratio 2, a known limit" is now false.
   - Also update line 1194, "asserts no `Miss` at fit; it asserts no eviction only at ratio 1", so it
     says the Miss check applies at every ratio.
2. **Medium, test quality: "An extension touches only the base it builds on" does not test that.**
   `tests/test_BrushCoverageCache.cpp:782`. A mutant that puts back the old touch-every-prefix behaviour
   (`entry.lastUse = ++clock_` inside the prefix loop) passes all 27 cache tests.
   - Why: entries sit in insertion order, so touching every improving prefix still leaves
     `lastUse(0) < lastUse(1) < lastUse(2)`. The checks `gone[0] <= gone[1] <= gone[2]` hold either way.
   - The real bug needs a live entry older than the chain. A stray touch made a dead prefix younger than
     that live entry. Once all prefixes are superseded, the flag already hides the touch.
   - Fix: add a case where the stray touch lands on a live entry, so the flag cannot hide it.
     1. Hold P = [x], then Q = [x, y] (an Extended, which marks P superseded).
     2. Hit P, as an undo does, so P is live again.
     3. Insert an unrelated live list L.
     4. Request [x, y, z] with `retain = false`, so nothing is inserted and nothing is marked. The loop
        passes P, then Q, which is the best base.
     5. Apply pressure for one eviction and check which entry goes.

     With the fix, P is the oldest live entry and goes. With the old touch, P is younger than L, so L
     goes. Rename the existing test to what it does check ("superseded prefixes go in LRU order").
3. **Low, test quality: two of the clear sites are untested.** Mutants that drop `superseded = false`
   from `find()` (both branches) or from the ContentHit branch of `coverage()` pass every
   `[brush][cache]` test. Only the Hit branch is covered ("A hit on a superseded entry makes it live
   again" fails without it). Fix: add the same undo-then-pressure scenario through `find()`, and through
   an equal list behind a different pointer (ContentHit).
4. **Low, claim not verified: "the evictions are superseded parents".** This is said in the test comment
   (`tests/test_MainWindowMasks.cpp:1998`), the report and the ADR note, but nothing counts it. The rule
   takes superseded entries first only while there are some. Once they are gone, a live entry goes. The
   fit cache sits at 511.4 of 512 MiB, so the margin is very thin. Zero misses is consistent with the
   claim but does not prove it: an evicted live entry that is never asked for again does not show up as
   a miss.
   - Option A: add an internal counter for live-entry evictions (private or test-only), and assert it is
     0 at fit at ratio 2.
   - Option B: soften the wording to "mostly".
5. **Low, claim wording: "with every lookup a Hit the commit is the cost of rendering the 6 MP level".**
   This is in ADR note line 1225 and in the report. It is not quite true. At fit `extended=120` for 60
   strokes, one per level per stroke, so every commit still draws the new stroke once at 3000x2000 as an
   Extended. Undo is all Hits and costs 414 ms, so render plus pack is about 410 ms of the 507 ms commit.
   About 100 ms is the Extended draw plus the "render in flight" overlap. Fix the wording to "all brushes
   but the painted one are Hits; the painted one is one Extended draw". The conclusion, that the remaining
   time is mostly render cost and outside the cache, still holds.
6. **Low, the baselines disagree.** The **Cache.** paragraph gives the ratio-2 commit before this change
   as median 1.0 s, p95 4.7 s. The new note says "was 534" and "was 2370". Both may be correct, from
   different runs, but a reader cannot tell. State which baseline the "was" numbers come from (for
   example "the implementer's main-branch run on 2026-10-16").
7. **Low, measurements not checked.** Ratio-2 1:1 evictions rose from 83 to about 137. Misses stayed at
   15, which is harmless: it is likely more, smaller superseded entries evicted in place of fewer live
   ones. The 1:1 commit and latency medians at ratio 2 have no baseline from the same session, so the
   "no 1:1 regression" target is not shown. The older ADR numbers (3.7 s commit, 5.0 s latency) suggest
   no regression, but they come from a different run. Fix: one baseline run on `main` at ratio 2.
8. **Info, design gap: redo makes the base live for good.** After an undo (a Hit on the base clears the
   flag) and a redo (a Hit on the longer list, with no Extended), the base stays live without a settled
   grid. That is the same dead-parent pressure as before, in sessions with a lot of undo and redo.
   - Option A: accept it (bounded, and it needs user action).
   - Option B: at eviction, treat any entry whose list is a strict pointer prefix of another held entry
     of the same size as superseded, computed rather than stored. This also fixes stale flags. It is
     O(n^2) over at most `maximumEntries` entries, and only when evicting.
   - Option C: set the flag again when the longer list gets a Hit. This needs a prefix scan on a Hit.
   I recommend A for now and a note in ADR 044.
9. **Info, the timing assertion is fragile.** `CHECK(misses == 0)` at ratio 2 depends on 0.6 MiB of
   headroom. It is fine in a `[.timing]` test, but expect it to flip if brush presets or the tile size
   change. Finding 4 Option A would make a failure easier to diagnose.

## Doc nits

- `docs/todo.md:124-126`: these lines are about 110 columns, against about 85 in the paragraph around
  them. Rewrap them.
- The header brief and the `insert` `@param extends` say what the design asked for. Fine.

## Must fix before merge

Findings 1 and 2. Do 3 to 5 in the same pass if it is cheap. No production code changes are needed.
