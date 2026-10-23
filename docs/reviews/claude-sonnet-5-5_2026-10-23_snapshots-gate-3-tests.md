# Review: snapshots Gate 3 (thumbnails), tests lens (uncommitted working tree)

Reviewer: Claude Sonnet 5.5, 2026-10-23. Scope: the uncommitted changes implementing Gate 3 of
[ADR 056](../adr/056-snapshots-are-per-photo-looks-with-independent-history-and-sidecar-storage.md)
(decision 9, and the Verification list), read against the Gate 3 entry of [`todo.md`](../todo.md)
("Verify stale results, size bounds, and that completion does not alter active edits or dirty
state"). Lens: do the tests prove what the ADR says they must; are any vacuous or timing-flaky; is
failure injection missing; was an existing test weakened. Files read:
`tests/test_SnapshotThumbnails.cpp` (699 lines, new), the diffs to `tests/CMakeLists.txt`,
`CMakeLists.txt`, `src/app/ThumbnailWorker.{h,cpp}`, `src/app/ThumbnailCache.{h,cpp}`,
`include/EditSession.h`, `src/core/EditSession.cpp`, and the new `src/core/SnapshotThumbnails.cpp`.
I did not build or run anything, so every "would still pass" below is by reading, not by mutation.

**Counts:** major 4, medium 6, minor 5.

Summary: the session half is good. "Applying a thumbnail patches the matching node and nothing
else" observes history, position, editing, dirty state, both base IDs, the structural revision and
the on-disk active recipe around an open, unsaved gesture, and "a thumbnail that is not the one
wanted" checks the sidecar bytes, the collection and the session for six refusal causes including
the lock timeout and its retry. Those two tests discriminate. The key-sensitivity and cache-damage
tests are also sound. The weak half is the worker. The stale-result test cannot tell the
generation checks apart (queue clearing hides the in-flight checks), "a new folder leaves snapshot
jobs alone" is not tested although its title says so, the cache hit is never proven, and the one
byte-limit test that names the 64 KiB clamp cannot fail. The key tests skip local adjustments, the
one part of the key with bespoke masking code. Nothing in the existing suite was weakened.

Existing-test edits: none. `tests/CMakeLists.txt` gains one line and the `ThumbnailWorker`
constructor takes the new callback as a defaulted last parameter, so every old call site compiles
and means what it did. The report's pass counts (1856 and 320 cases) are consistent with that.

---

## Major

### T1. Obsolete-generation test cannot tell the in-flight checks from the queue clearing

`tests/test_SnapshotThumbnails.cpp:607-647`; `src/app/ThumbnailWorker.cpp:333-358, 235-240`.

The test holds the worker inside an embedded callback, queues jobs 1 and 2, bumps the generation,
and shows that only job 4 is delivered. But `beginSnapshots()` already empties the queue, so jobs 1
and 2 never reach `executeSnapshot`. The three checks that the ADR's "stale results" claim
depends on, all inside `executeSnapshot` (`obsolete()` before the key at line 334, the same predicate as the
cancellation callback between render steps at line 349, and `!obsolete()` before delivery at line
355), are never reached with a stale job. Delete all three and the test still passes; delete the
`snapshots_.clear()` instead and the first check saves it. The case that matters in the GUI is a
job already decoding when the user moves to the next photograph, and that is exactly the case with
no test. The implementer lists it as open question 6 for the cancellation callback only.

Failing scenario: someone later changes `obsolete()` to compare against the wrong atomic, or drops
the delivery check as redundant; a thumbnail of photograph A is delivered after photograph B was
opened, and `applySnapshotThumbnail` refuses it only because the key differs, if the IDs collide
(`SnapshotId{1}` in both) and the recipe happens to match it is applied to the wrong photograph's
session. (Only the file name and size in the key stand between two photographs of one name.)

Fix: the worker's `LensDatabaseSource` is called on the worker thread after the first check and
before the render, so a recipe with `negative.lens.distortion = true` and a database lambda that
calls `worker.beginSnapshots()` bumps the generation mid-job deterministically. Assert no delivery
and that the next job (new generation) is delivered. A second variant with a gate in the lens
lambda and a bump after release exercises the cancellation callback.

### T2. "A new folder leaves snapshot jobs alone" is not tested, and the failing-job half only covers the quiet path

`tests/test_SnapshotThumbnails.cpp:649-668`; `src/app/ThumbnailWorker.cpp:168-186, 329-361`.

The test queues a job for a missing file, calls `setShots({})`, then queues job 2, and expects job
2 alone. Job 1 is almost surely finished before `setShots` runs (a missing file returns at
`snapshotThumbnailKey`, a stat call), and job 2 is queued after `setShots` in any case. So whether
`setShots` clears `snapshots_` makes no difference to the outcome. Adding `snapshots_.clear()` to
`setShots` passes this test. The header documents "A new folder leaves them alone"; nothing pins
it.

The failure half is also thinner than its name. A missing file makes the key `nullopt` and the job
returns before any render, so the `catch (const std::exception&)` is never entered. A file that
exists but is not decodable (a text file named `bad.dng`) would reach `loadImage`, throw, and
prove the catch and that the worker thread survives, which is the path the comment "Nothing may
escape the thread, or the process terminates" protects.

Fix: gate the worker in an embedded callback as the other two tests do, queue a snapshot job, call
`setShots` with another folder, release, and expect the snapshot delivered. For the failure, use a
nonempty garbage `.dng` as job 1 and a good one as job 2.

### T3. The render key's local-adjustment handling has no test

`tests/test_SnapshotThumbnails.cpp:204-253`; `src/core/SnapshotThumbnails.cpp:44-56`.

The only code in the key function that is not a plain call is the local-adjustment branch: it
rewrites `nextLocalAdjustmentId` to the smallest valid value (so the id counter does not change the
key) and spells brush strokes as `StrokeDetail::Digest`, a count and a content hash. None of
`localAdjustments` is touched by any key test: the sections change exposure, `demosaic`,
`lens.distortion`, the file's size and its name. Two properties are therefore unproven:

- A different mask, or the same brush with one stroke point moved or a stroke erased, gives a
  different key. If `contentHash` ignored a field, two different looks share a key and the
  thumbnail of one is shown, cached and accepted by `applySnapshotThumbnail` for the other.
- A state differing only in `nextLocalAdjustmentId` (an adjustment added and removed) gives the same
  key, which the code claims and which keeps the cache from missing for no reason.

Fix: two sections with a linear/radial mask and a brush. Also one section with state that differs
only by a tiny float (exposure 1.0 against 1.0001) to pin that `settingsToJson` is exact enough.

### T4. The cache hit is never proven, and neither is job priority

`tests/test_SnapshotThumbnails.cpp:562-605`; `src/app/ThumbnailWorker.cpp:341-356`.

The test is named "the cache serves the second run" but it never makes the second run observe a
cache hit. The section reads the entry directly and compares it with the first result, then
corrupts it and shows regeneration. If `cache_.loadSnapshot(*key)` were deleted from
`executeSnapshot`, every assertion still holds: the render is deterministic and the second run
re-renders and rewrites an identical entry. The performance claim (a gate-4 grid of 32 snapshots
must not re-render every open) has no test.

Fix: plant a distinguishable valid thumbnail (a solid red JPEG) under the computed key with
`storeSnapshot`, request the recipe, and expect that exact image delivered.

The same applies to priority ("run after the embedded jobs and before the developed ones",
reported by the implementer as open question 6): no test fixes the order, so a refactor of `next()`
that swaps the branches passes. The Gate pattern in the same file supports it: hold the worker,
queue an embedded-and-developed shot and a snapshot job, and record the callback order.

---

## Medium

### T5. "The limit never exceeds 64 KiB" cannot fail

`tests/test_SnapshotThumbnails.cpp:301-305`; `src/core/SnapshotThumbnails.cpp:104`.

The test passes `10 * maximumSnapshotThumbnailBytes` and noise at 256x256, which the neighbouring
section's own comment says is about 50 KB at quality 85. The result is under 64 KiB with or without
the `std::min(byteLimit, maximumSnapshotThumbnailBytes)` clamp. If the clamp were dropped, the
result would be the same. The encoder's `validate(thumbnail)` would catch an oversized result with a
throw, so the real safety net exists, but the test does not show it and its title says it does.

Fix: either delete the section or make it meaningful with a seam that really forces the clamp (a
test-only `qualities` override, or a one-bit-per-pixel pattern if some image beats noise).

### T6. At render level, the 256 px bound and "never enlarges" are not exercised

`tests/test_SnapshotThumbnails.cpp:313-344, 271-284`.

The render test uses the 32x24 DNG fixture throughout. `sizeOf(dark).width() <= 256` holds for any
output, and nothing asserts the exact size (32x24, or 16x12 for the half-size decode), so an
enlarging `FitInside` (a regression the header promises against) would still pass. The pyramid loop
`for (i < level) source = halved(source)` always runs zero times on a 32x24 source. Scaling down
is covered only synthetically through `encodeSnapshotThumbnail` with a 1000x500 image. Nothing
covers orientation (`linear-32x24-rotated.dng` is available), or a state with a crop, either of
which changes the output size.

Fix: assert the exact output size for the fixture, add the rotated fixture and a crop state, and add
one render of a large synthetic source (a generated DNG, or `loadImage` seam) that shows a long side
of exactly 256.

### T7. The worker's lens branch is untested end to end

`src/app/ThumbnailWorker.cpp:343-348`; `tests/test_SnapshotThumbnails.cpp` (no lens section in the
worker tests).

`renderSnapshotThumbnail` is called with a default `LensCorrection` in every test, and the worker's
`negative.lens.any()` branch (open the photo, apply the recipe's own negative, look up the
database) is never entered. Two claims have no support: the comment "The recipe's own lens settings,
not the sidecar's", and the ADR's "a different machine's lens database alone does not invalidate a
usable embedded image". `lens-canon-32x24.dng` and `test::fixtureDatabase()` exist and
`test_ThumbnailWorker.cpp:365-407` shows the pattern, including `ARRAW_REQUIRE_LENSFUN_OR_SKIP`.

Fix: a lensfun-tagged test with a recipe that turns lenses on, a sidecar that does not, the fixture
database against an empty one: the pictures differ, the render keys are equal, and the
corrected one matches the recipe's. This would also pin open question 2 (an uncorrected render made
while the database failed to load is cached forever under a key that cannot tell).

### T8. The "matching node only" assertions are looser than the claim

`tests/test_SnapshotThumbnails.cpp:458-467, 471-502`.

`OnDisk` records the active state, negative, saved base, revision and the snapshot names, so a
change to another snapshot's recipe, parent or time would not show. The final
`disk.snapshots == session.snapshots()` is true if the implementation damages both the same way
(the session copy is written from the same object). The check on `b` covers only its thumbnail.
Stronger and cheaper: take the whole collection before, replace only `a`'s thumbnail in the copy,
and compare it with the disk and the session after.

Related gaps from the ADR Verification list ("concurrent structural writes and thumbnail
patches", decision 6 "does not overwrite newer collection changes"):

- Nobody else changes the collection between the session's last read and the apply except deletion.
  Missing: another writer creates snapshot C or renames A, then the session applies A's
  thumbnail; the file must keep C and the new name, and the session's copy must show A's thumbnail
  only (and its own stale revision, so the watcher refreshes).
- The dirty case only: a clean session (`hasUnsavedChanges()` false) is not tried, nor a session
  whose saved active state differs from its working one, which is where "never saves pending
  develop edits" could be broken by writing the wrong state.
- Applying a then b in sequence is not tried, although the worker will deliver them that way.

### T9. Failure injection beyond the lock is thin

`tests/test_SnapshotThumbnails.cpp:504-560, 346-380, 562-605`.

Covered: lock timeout (with retry), stale key, unknown ID, deleted elsewhere, invalid thumbnail.
Not covered:

- A damaged sidecar (unparseable XMP, or damaged snapshot data that `setSnapshotThumbnail` must
  refuse rather than repair, per ADR decision 7) during an apply.
- An I/O failure on the commit (read-only directory) leaving the file and the session copy as they
  were. The code assigns `found->thumbnail` only after `setSnapshotThumbnail` returns true, which is
  right, but a throwing commit is not exercised.
- `storeSnapshot` failing (cache directory not writable): the worker still delivers, since the
  result of `storeSnapshot` is ignored; the test file has no check that the delivery does not
  depend on the cache.
- The worker with a callback that throws (the header says "must not throw"; the `catch` would
  swallow it, which is silent loss).

### T10. Priority and cancellation inside `develop()` are untested (disclosed)

`src/app/ThumbnailWorker.cpp:277-299`; open question 6 of the report. Mentioned in T1 and T4 for
the checks that can be tested without new seams; the remaining part, cancelling during `develop()`,
needs a seam and is rightly deferred, but the unit test for the callback
(`tests/test_SnapshotThumbnails.cpp:331-339`) only shows that it is polled more than once and
returns nothing, not at which steps.

---

## Minor

### T11. Key sensitivity is shown for three fields only

`tests/test_SnapshotThumbnails.cpp:204-253`. Exposure, `demosaic` and `lens.distortion` stand for
the whole of `settings` and `negative`. A key that dropped, say, white balance, crop or curves
would pass. A loop over one change per settings group (or comparing with the sidecar key function
`settingsToJson`'s own coverage test, if there is one) is cheap. The revision constant is part of
the key but no test can show it, because it is `constexpr`; a `snapshotThumbnailKey` overload that
takes the revision would make it testable.

### T12. Cache path safety and oversized store are untested

`tests/test_SnapshotThumbnails.cpp:346-380`; `src/app/ThumbnailCache.cpp:183-236`. `pathOf` builds
`root/key[0..2]/key.jpg`, and the render-key validation (`storableMaskName`) is a UTF-8/control
check, so a key such as `../x` would pass `validate`. The worker only passes hex digests, so this is
defensive, but nothing pins that `loadSnapshot`/`storeSnapshot` refuse such a key. Also
`storeSnapshot` is shown to refuse a 10-byte junk JPEG but not a valid JPEG over 64 KiB or over 256
px.

### T13. Own-write watcher claim has only the equality half

`tests/test_SnapshotThumbnails.cpp:498-499`. The equality `disk.snapshots == session.snapshots()`
is the necessary condition and is checked. Whether `MainWindow::sidecarChangedOnDisk` really
treats the write as no change (and does not call `leaveCropMode`/`cancelGesture`, open question
5) is not tested in `arraw-widget-tests`; the implementer defers it to gate 4. Record it there as a
required test.

### T14. Test hygiene

- `Gate` has no RAII release: a throwing assertion or exception between `gate.entered()` and
  `gate.release()` (tests at `:616-640` and `:678-694`) leaves the worker thread blocked in
  `gate.wait()`, and the worker destructor's join then hangs the whole suite instead of failing.
  A `struct Release { Gate& g; ~Release() { g.release(); } }` guard fixes it.
- `noise()` uses the global `QRandomGenerator`, so byte counts vary run to run. The margins
  (best ~50 KB against 30 KiB) are wide, so this is not flaky in practice, but a seeded generator
  is free.
- `waitUntil(... pendingJobs() == 0)` is correct because `pendingJobs` counts the running job; no
  sleeps are used for assertions. I found no timing-dependent assertion; the 20 s timeout is a
  hang guard only. The "a thumbnail the reader drops" section replaces the first `/9j/` in the
  sidecar, which is the thumbnail's only because these snapshots carry no brushes (base64 of
  brush data could contain it); it `REQUIRE`s the outcome, so a wrong hit fails rather than passes.

### T15. Small coverage nits

- `hasCurrentThumbnail` is tested for state and a hand-set key; the file-size and negative cases
  are covered only through `snapshotsNeedingThumbnails`.
- "does not change with the folder or the modification time" sets the time of the copy; a file
  copied with its time preserved is the common case but the same code path.
- `renderOf` always uses `SnapshotId{1}`; no test shows that the delivered `id` is the recipe's
  rather than the first one queued (the worker test uses 7, which does show it for one job).

---

## What the tests do prove

- Matching-node-only patch, active-edit/history/base/dirty/revision immutability, including an open
  gesture, and the unchanged on-disk active recipe: `:471-502`. Discriminating.
- Stale results at the session boundary (other recipe, other file size, unknown ID, deleted
  elsewhere, invalid, lock timeout with retry), each with a byte-for-byte sidecar comparison:
  `:504-560`. Discriminating.
- Size bounds of the encoder (256 px scaling, no enlargement, quality ladder with a lowered limit,
  hard failure when nothing fits) and the cache's deletion of damaged, oversized and over-dimension
  entries: `:271-311` (except T5) and `:346-380`.
- Key independence from folder and modification time, and dependence on name, size, state and
  negative: `:204-253` (except T3, T11).
- Duplicate requests replaced, worker renders the given recipe and not the sidecar: `:562-605`
  (except T4), `:670-699`.
