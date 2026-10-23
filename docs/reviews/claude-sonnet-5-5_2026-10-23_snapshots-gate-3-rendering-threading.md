# Review: snapshots Gate 3, rendering and threading lens

Reviewer: claude-sonnet-5-5, 2026-10-23. Subject: the uncommitted Gate 3 working tree
(`include/SnapshotThumbnails.h`, `src/core/SnapshotThumbnails.cpp`, `include/EditSession.h`,
`src/core/EditSession.cpp`, `src/app/ThumbnailCache.*`, `src/app/ThumbnailWorker.*`,
`tests/test_SnapshotThumbnails.cpp`), read against ADR 056 (decisions 6 and 9), the architecture
proposal ("Rendering, thumbnails, and other surfaces" and the watcher paragraph) and the Gate 3
entry of `docs/todo.md`. Lens: rendering and threading (render key, scheduling, cancellation, races
and lifetimes, generation rejection, JPEG bounds, completion versus active state, sidecar lock and
watcher). Static review: nothing was built or run, no production code was changed. Claims about
behaviour come from reading code and tests; the one standard-library claim I make (finding 3) comes
from the C++20 specification of `condition_variable_any::wait` with a stop token and is marked as such.

## Verdict

The mechanics are carefully done. The generation check in `requestSnapshotThumbnail` is under the
queue lock, the two generations are atomics, jobs and results own their data, nothing escapes the
worker thread, and `applySnapshotThumbnail` is a disciplined write: it validates, recomputes the key
from the session's own recipe, goes through the lock-and-patch writer, and only then updates one
node's thumbnail. I found no way for a completion to change the active recipe, history, bases, dirty
state or structural revision.

But the render key as designed is too weak, and I think it is a real defect rather than a conservative
reading (finding 1): it drops both the path and the modification time that the existing developed key
has, so two different photographs can share a key through the global cache and the wrong picture is
then written into a sidecar as "current". The proposal's requirement that the writer identify its own
thumbnail writes for the watcher is not met (finding 2). The worker drains its whole queue on shutdown
(finding 3, mostly pre-existing, amplified), and the apply path as it stands cannot be driven from the
UI thread for a photograph with many stale snapshots (finding 4). The rest is minor.

## Findings, most serious first

### 1. Render key is name plus size: different photographs collide in the shared cache (major)

`src/core/SnapshotThumbnails.cpp:35-72`, consumed at `src/app/ThumbnailWorker.cpp:341`. The key hashes
revision, **file name**, **file size** and the recipe. `ThumbnailCache::developedKey` hashes the
absolute path, size and modification time as well (`src/app/ThumbnailCache.cpp:28-60`). The path and
time were dropped on purpose to survive a sync, but the name and size alone do not identify a
photograph, and the lookup is in a cache shared by every folder on the machine.

Failing scenario: a camera that writes fixed-size files (uncompressed DNG, many ARW/RAF/NEF modes; the
size is constant per camera model and mode) is used on two cards, and both have `DSC_0001.ARW`, which
is what a freshly formatted card produces. The photographer makes a snapshot "Original" of each,
neither edited, so the states are equal (the neutral default, or a shared preset). Both snapshots get
the same key. The thumbnail of the first photograph is cached; opening the second, the worker
`loadSnapshot`s it and `applySnapshotThumbnail` accepts it (key matches by construction) and writes the
**first photograph's picture into the second one's sidecar**, where it stays, is synced, and is never
regenerated because it is "current". The same holds for a file replaced in place by another of equal
size (open question 1), but the cross-folder case needs no replacement at all.

Nothing in the tests would notice: `does not change with the folder or the modification time` copies
identical bytes, which is the one case where the shared key is right.

Options:
- **A.** Add a cheap content identity that travels with the file: a SHA-256 of the first and last 64 KiB
  (a few hundred microseconds; the header carries EXIF capture time and serial), or the capture time
  with sub-second and the camera serial from `ImageMetadata`. Keeps the sync property.
- **B.** Keep the portable key for the sidecar's stored thumbnail (it is next to its file, so name and
  size are enough there), but make the *cache* entry name include the absolute path and modification
  time as `developedKey` does, so cross-folder collisions cannot happen. Smallest change; in-place
  replacement is still undetected.
- **C.** Both.

I would take A, as it also closes the in-place case. Add a test: two different files of the same name
and size in different folders give different keys and different cached thumbnails.

### 2. The writer does not identify its own writes; every landed thumbnail looks like an external edit (major)

`src/core/EditSession.cpp:255-273` (no stamp, nothing for the caller to note), `src/app/FolderWatcher.cpp:135-153`,
`src/app/ui/FilmStrip.cpp:326-330`, `src/app/ui/MainWindow.cpp:1976-2020`. The proposal says: "The writer
must identify its own thumbnail writes so the watcher does not report them as external edits." The
mechanism that exists is `FolderWatcher::noteOwnWrite`, called after the session's own writes
(`MainWindow.cpp:2455`, `FilmStrip.cpp:1028`). Gate 3 neither calls it nor documents that the caller
must. The comparison with thumbnails stripped inside `sidecarChangedOnDisk` is only the second line of
defence, and it runs after the harmful part.

Failing scenario (Gate 4 as the API invites it): a result lands and is applied; the sidecar's stamp
changes and was not noted. `FolderWatcher::onSidecarRefreshed` emits `sidecarChangedExternally`. Then
(a) `FilmStrip` calls `thumbnails_.invalidate(primary)`, an urgent developed job for that photograph
that jumps ahead of every other queued job; (b) `sidecarChangedOnDisk` runs `leaveCropMode(true)`,
`maskOverlay().cancelGesture()` and `developPanel_->finishPendingEdit()` **before** it looks at
anything, so a photographer who is cropping or brushing is thrown out of it by a background job;
(c) with unsaved edits it shows "your unsaved edits are kept" in the status line. With 32 stale
snapshots that is up to 32 repetitions within a few seconds. Open question 5 names (b) but treats it
as a polish item; combined with the missing own-write note it is the common case, not a corner.

Options:
- **A.** Document and test the contract: the caller calls `FilmStrip::noteOwnWrite(photo)` immediately
  after a successful `applySnapshotThumbnail` (on the same thread, so no other write interleaves), and
  add the call to the Gate 4 checklist in `todo.md`.
- **B.** Have `applySnapshotThumbnail` return the sidecar stamp it wrote (or take a callback), so the
  session layer, not the GUI, decides what is noted.
- **C.** Also move the "disk equals session apart from thumbnails" test in `sidecarChangedOnDisk` ahead
  of `leaveCropMode`/`cancelGesture`/`finishPendingEdit`, as open question 5 suggests.

A and C are small; do them before Gate 4 and tick the Gate 3 box only with A in the notes.

### 3. The worker drains every queued job after stop is requested (major; pre-existing mechanism, new cost)

`src/app/ThumbnailWorker.cpp:163-166,279-283,303-308`. `~ThumbnailWorker` only calls `request_stop()`.
`next()` waits with `wake_.wait(lock, stop, predicate)`. Per the C++20 specification that overload
returns `predicate()` when the stop request is seen, so with a non-empty queue it returns true, the job
is run, and `run()` loops (`while (const auto job = next(...))`) until the queues are empty. Nothing in
`run`, `execute` or `executeSnapshot` looks at `stop`. (I did not run this; the reading is from the
standard's wording, and a quick test with a gated job and a queued second one would confirm it.)

The snapshot path adds two things on top of the same behaviour. A renderer job is a full RAW half-size
decode, so a queue of up to 32 snapshots is tens of seconds at quit. And the result callback runs during
the drain, i.e. while the owner of the worker is being destroyed, which is the lifetime hazard the
header's "must not throw / owns its data" paragraph does not cover: the callback captures whatever
`MainWindow` captures.

Fix: check `stop.stop_requested()` at the top of the `run` loop body (and pass the token to
`executeSnapshot` so `obsolete` also returns true when stopping). Add a test: a gated job, a second job
queued, destroy the worker, the second must not run.

### 4. Applying results on the owner thread cannot scale: whole-sidecar rewrite per thumbnail, 2 s lock wait each (major as a design gap, estimate unmeasured)

`src/core/EditSession.cpp:266`, `src/core/Sidecar.cpp:2061-2086`. Each `applySnapshotThumbnail` takes the
lock, parses the XMP, runs `readSnapshotsIn` once to find the node and again (`checkSubtreeSize`) after
patching, and commits the whole file. Those reads validate every node's thumbnail header. A photograph
with 32 snapshots carries up to 32 x 64 KiB of base64 (about 2.7 MB), so 32 landings are 32 parses, 64
snapshot reads and 32 full rewrites, quadratic in the number of thumbnails, all on the thread that owns
the session, which is the UI thread (open question 4). I did not measure it; the proposal itself says
to "measure sidecar size and latency with several brush-heavy snapshots".

The failure side is worse: `SidecarLockTimeout` is thrown after up to 2 s. If another process holds the
lock (a sync tool, the CLI, a stuck lock file below the 5 s stale limit), each result blocks the UI for
2 s and then throws, and since results arrive in a burst, 32 results are about a minute of freeze. The
worker does not retry; `snapshotsNeedingThumbnails()` would list the snapshot again only if somebody
calls it. (Good property: the render is in the cache, so a retry costs a cache read, not a render.)
Also, the exception must not leave a Qt slot, which is a Gate 4 note: nothing here says so.

Options:
- **A.** Batch: `applySnapshotThumbnails(std::vector<std::pair<SnapshotId, SnapshotThumbnail>>)` and a
  matching `setSnapshotThumbnails` that patch several nodes under one lock and one commit; the GUI
  collects results for a short timer interval and applies them together.
- **B.** Apply on a timer with a back-off after a timeout, one failure disabling further tries for that
  photograph until it is reopened.
- **C.** Accept the cost for now and measure at Gate 4. Then write the numbers into the ADR's
  "to measure" list.

A removes the quadratic term and the repeated waits; I would do A plus a timeout back-off.

### 5. Delivery is "not delivered after the generation changed" only up to the callback; docs promise more (minor)

`src/app/ThumbnailWorker.cpp:355-357`, `src/app/ThumbnailWorker.h` (constructor doc, class doc). The worker
checks `obsolete()` and then calls the callback. `beginSnapshots()` on the UI thread can run between those
two statements, and a queued event posted by the callback before `beginSnapshots()` is still in the event
queue afterwards. So a result of the old generation can reach the UI thread after `beginSnapshots()`
returned. The constructor doc says "only while its snapshot generation is current", and the class doc says
a result of an older one "is never delivered". Neither is strictly true.

Failing scenario: photograph A open, result for A's snapshot 2 is posted; the user opens B (generation
bump); the slot runs for B's session with `primary = A`, `id = 2`. `applySnapshotThumbnail` is saved by
the key check (A's key does not match B's recipe for 2 unless name, size and recipe are equal, see finding
1), but only because the key check is strong enough.

Fix: say in the docs that the consumer must compare `result.generation` with `snapshotGeneration()` and
`result.primary` with the session's path on the owning thread, before applying; do it in the Gate 4 slot.
The test `Results of an obsolete snapshot generation are not delivered` covers queued jobs only; add one
where `beginSnapshots()` is called from inside the callback.

### 6. Scheduling: strict FIFO behind all embedded jobs, ahead of all developed ones, and a decode per snapshot (minor)

`src/app/ThumbnailWorker.cpp:292-306`, `src/core/SnapshotThumbnails.cpp:132`. Observations in order of weight:
- Each snapshot job decodes the RAW again (half-size). Thirty-two snapshots of one photograph is 32
  decodes of the same file, each fixed cost, while only the develop step differs. Open question 7 only
  mentions coalescing across photographs; same-photograph sharing is the bigger win. Option: one job per
  photograph that decodes once and develops each recipe in turn (the cancellation callback still applies
  between recipes).
- Snapshot jobs sit behind the *embedded* queue, which holds one entry per shot in the folder. On first
  visit to a big folder the open photograph's placeholders last until every embedded preview is extracted,
  which is minutes for thousands of RAWs. In the other direction they sit ahead of all *developed*
  jobs, including the visible ones, so 32 stale thumbnails (for instance after a revision bump) delay
  the visible filmstrip by 32 renders. Both are defensible readings of "low priority", but it is a
  choice made by queue position, with no test (open question 6). An explicit rank (urgent, embedded
  visible, snapshot, developed visible, rest) would say what is meant.
- The worker is one thread, so a running snapshot render also delays an `urgent_` job (a just-saved
  photograph's filmstrip update) by one decode. Bounded, but it adds to the film-strip invalidations of finding 2.

### 7. Cancellation is coarse and ignores shutdown (minor)

`src/core/SnapshotThumbnails.cpp:125-148`, `src/app/ThumbnailWorker.cpp:333-349`. The callback is checked
before the decode, after the decode and after `develop`, never inside them. The lens branch
(`openPhoto`, `lensDatabase_()`, whose first load "may take a second", per the constructor doc) runs
before the first check inside `renderSnapshotThumbnail` and is not guarded at all. Net: an obsolete job
costs up to one decode plus a lens-database load before it notices. `obsolete` also does not consult
the stop token (finding 3). The core cancellation test only proves the callback is called more than once
and that a render with an always-true callback returns nothing, not that it stops at the right place.

### 8. The lens branch reads the sidecar and can persist a lens-less picture as current (minor)

`src/app/ThumbnailWorker.cpp:343-348`. (a) `openPhoto(job.primary).with(job.recipe.negative)` parses the
sidecar with all its snapshots and thumbnails only to get the file's metadata, so the claim of the header
("reads no sidecar") holds for `renderSnapshotThumbnail` but not for the worker path that calls it.
It costs a full parse per lens-enabled snapshot and couples the render to the sidecar's state (the
recipe's own lens settings are put back by `.with`, so the result is right, only slower). Use the metadata
reader that `savedLensCorrection`'s sibling code could share, not a document open.
(b) If `lensDatabase_()` yields an empty or not-loaded database, `lensCorrectionFor` returns an empty
correction, the render proceeds without lens correction, `storeSnapshot` caches it under a key that
excludes the lens, and `applySnapshotThumbnail` writes it as current. ADR 056 decision 9 accepts that
another machine's database does not invalidate a usable image; a machine with *no* database producing the
image is a different case, because nothing will ever replace it. Option: do not store or deliver a render
whose recipe has `lens.any()` while the database is unavailable. No test covers any lens-enabled
snapshot in the worker.

### 9. Cache and apply validate the JPEG header only (minor)

`src/app/ThumbnailCache.cpp:183-209`, `src/core/Snapshots.cpp:39-73`. `validate(SnapshotThumbnail)` reads
the header ("a crafted file that declares huge dimensions is refused unseen"), so a cache entry that is
truncated in the body, or has a good header and garbage after it, passes `loadSnapshot`, is delivered,
and is written into the sidecar as current. The header comment "a damaged or oversized entry is deleted
on read" is true for damage the header shows. The test uses the text `not a jpeg`. A 256x256 JPEG decode
is about a millisecond: decode fully in `loadSnapshot` (not in the sidecar reader, where the header-only
rule is deliberate), and test with a truncated JPEG.

### 10. Key hygiene nits (minor)

`src/core/SnapshotThumbnails.cpp:43-72`, `include/SnapshotThumbnails.h:22`.
- `snapshotThumbnailRevision` is not tied to anything. A develop-engine change that does not bump it
  leaves stale thumbnails current forever. A golden test that renders the fixture at a fixed recipe and
  compares a digest of the decoded pixels (tolerance not needed on one CPU path) would force the bump.
- The negative's demosaic is in the key but the render is a half-size decode, which ignores it
  (`NegativeSettings::demosaic`: "full size only"). Harmless over-invalidation: two snapshots differing
  only in demosaic get identical images and two cache entries.
- The file name is hashed as the filesystem gave it. A synced folder whose non-ASCII names change
  normalisation form (NFC and NFD between macOS and the others) or case (Windows) gets new keys and
  regenerates. Normalise (NFC, and decide on case) if the sync property is to hold.
- A half-size decode of a tightly cropped photograph can leave fewer than 256 pixels (FitInside never
  enlarges), so heavy crops give small thumbnails that the UI will upscale. Fine for 256x256 bounds;
  mention it so Gate 4 does not assume 256 on the long side.
- The error message of `encodeSnapshotThumbnail` names the maximum even when a test's lower limit
  failed (`SnapshotThumbnails.cpp:112-115`).

## Checked and found sound

- Generation rejection: `requestSnapshotThumbnail` compares under `mutex_`; `beginSnapshots` clears and
  increments under the same lock; the worker reads the atomic between steps. The worker never touches
  `snapshotGeneration_` while holding the queue lock in a way that can deadlock, and `pendingJobs()`
  cannot report 0 while a job runs (`running_` is set in the same critical section as the pop).
- A failing job is silent and the loop goes on (everything in `executeSnapshot` is inside the `try`).
  The one thing it hides is that the consumer cannot tell "pending" from "failed", so a Gate 4
  placeholder cannot say "could not render"; keep that in mind.
- Completion versus active state: `applySnapshotThumbnail` changes only `found->thumbnail` after the
  writer returned true; the writer rereads the sidecar under the lock and patches only that node, does
  not bump the structural revision, and leaves pending edits unsaved. A gesture open in the session is
  irrelevant to it. The refusal paths (unknown ID, key mismatch, snapshot deleted elsewhere, invalid
  thumbnail, timeout) leave both the file and the session copy unchanged.
- The key is computed by the same function on the worker thread, in `hasCurrentThumbnail`, and in
  `applySnapshotThumbnail`; the apply path recomputes from the session's own recipe and the current file
  size, so a file that changed size between render and apply is refused.
- Byte and dimension bounds: the ladder stops at the first fit, `validate` runs before return, the
  render's fit makes 256 a ceiling, and the cache reads at most limit + 1 bytes.

## Test gaps

- No test for two different files with equal name and size in different folders (finding 1).
- No test that a running job is dropped when `beginSnapshots()` is called, or from inside the callback
  (finding 5); the existing test only covers queued jobs, and relies on the fixture having an embedded
  preview to enter the gate (a missing preview would show up as a 20 s timeout, not a clear failure).
- No test of snapshot versus embedded versus developed ordering (open question 6; finding 6).
- No test of shutdown with a non-empty queue (finding 3).
- No test of a lens-enabled recipe in the worker, or of the lens database being unavailable (finding 8).
- No test of a truncated-body cache entry (finding 9).
- No test of `applySnapshotThumbnail` followed by `FolderWatcher::noteOwnWrite` (finding 2); the test
  that a re-read sidecar equals the session covers the comparison, not the event.

## On the implementer's open questions

1. Name plus size: no, see finding 1; the conservative reading is the one that adds identity, not the one
   that removes it.
2. Lens database outside the key: agreed for the *other machine* case; see finding 8(b) for the no-database
   case.
3. Priority against developed jobs: see finding 6; it is also starved from the other side.
4. Lock wait on the UI thread: see finding 4; batching is the fix that also helps the quadratic cost.
5. Watcher: see finding 2; it is more than polish.
6. Missing priority and in-flight cancellation tests: agreed, and findings 3, 5 and 7 are what such tests
   would have found.
7. No coalescing across photographs: true but small; coalescing within one photograph is the real win
   (finding 6).
