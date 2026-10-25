# Review: snapshots Gate 4, writer threading

- Reviewer: claude-sonnet-5-5
- Date: 2026-10-25
- Lens: writer-threading (the writer thread, races with `EditSession` and an ordinary Save, lost updates,
  error and conflict paths, deadlocks with the sidecar lock, lifetime of queued work, the discard operation,
  watcher own-write handling).
- Scope: the uncommitted working tree against `docs/ideas/snapshots-gui-plan.md` and ADR 056.
  Read: `include/SnapshotWrites.h`, `src/core/SnapshotWrites.cpp`, `src/app/SnapshotWriter.{h,cpp}`, the diffs of
  `src/core/Sidecar.cpp`, `src/core/EditSession.cpp`, `src/app/FolderWatcher.*`, `src/app/ui/FilmStrip.*`,
  `src/app/ThumbnailWorker.*`, and the writer-facing parts of `src/app/ui/MainWindow.cpp`
  (`snapshotWriteLanded`, `sidecarChangedOnDisk`, `showPhoto`, the create/rename/delete/discard entry points)
  and `tests/test_SnapshotWriter.cpp`.
- Method: reading only. Nothing was built or run, and no production file was touched.

## Verdict

The thread itself is sound. One FIFO queue and one consumer give result order for free. The core calls run
outside `mutex_`, and delivery runs outside it too, so the writer cannot deadlock with the GUI thread through its
own mutex. The destructor joins before any member the callback touches is gone (`snapshotWriter_` is declared
after the other workers, and the callback only posts to a live `QObject`). The sidecar lock is the only
cross-thread resource. A Save on the GUI thread and the writer serialise on it, and the Save's
`SnapshotBaseMissing` retry covers a Delete that lands first. The discard is correct for every shape of damage I
traced (see "Verified").

The weak spot is the seam between the writer's results and the GUI's session. Results are applied only when
the session serial still matches, but sessions are rebuilt (switching photos, an external reload) while writes
are in flight, and a rebuilt session for the same photo is not told about a write that its read may have
missed. That yields a spurious "changed by another program" and a list that lacks a snapshot the user just
made. Nothing is lost on disk. There is no finding that corrupts the sidecar or loses a snapshot.

None of the findings blocks the commit. The first two I would fix before Gate 5, because the user can reach
them with ordinary use.

## Findings

1. **Medium, correctness: a result for the same photograph but an older session is thrown away.**
   `src/app/ui/MainWindow.cpp:1925` ignores a result when `result.owner != open_->serial`, even if
   `open_->session.photo().path() == result.write.photo`. `showPhoto` (3516-3533) and the reload in
   `sidecarChangedOnDisk` (2715-2719) build a new session from `openDocument()`, which reads the sidecar on the
   GUI thread (2468, 2565, 2664) with no regard to writes still queued.
   - Scenario: Ctrl+N on photo A, the name dialog is accepted, the Create is queued (or waits on the lock).
     The user presses the next-photo key and the previous-photo key within the write's latency. The new session
     for A reads the sidecar before the commit and holds revision R. The Create commits (R+1) and its result is
     dropped by the serial check, so the snapshot never appears in the panel, and the "Created snapshot" message
     is never shown. The new session has `structuralTicket` empty, so the user can submit a Rename or another
     Create with expected revision R. That one comes back as `Conflict`, and the status line says "changed by
     another program", which is false. The list then refreshes. Slow disks and a contended lock (up to
     `sidecarLockTimeoutMilliseconds`, 2 s) widen the window a lot.
   - The reload path in `sidecarChangedOnDisk` has the same hole: it empties `structuralTicket`
     (a new `OpenPhoto`) while a structural write for the old serial may still be queued.
   - Fix options: (a) when the path matches but the serial does not, adopt `outcome.update` (or `fresh`) into
     the current session through `adoptSnapshots()` and skip the anchor, with a revision guard (finding 4).
     (b) On `showPhoto` for a path that has a ticket in flight, keep `structuralTicket` on the new session until
     it lands. (c) Make `openDocument` for the window wait for `pending()==0` for that photo (blocks the GUI,
     worst).

2. **Medium, correctness: the thumbnail sticky-failure flag also stops rendering.**
   `MainWindow.cpp:1970` sets `thumbnailWritesFailed` for any non-`Refused` failure, and
   `requestSnapshotThumbnails` (2014) and `snapshotThumbnailLanded` (2000) both return early on it. Step B
   deviation 2 says this is on purpose, but the flag is never cleared until the photograph is reopened.
   - Scenario: another program (a sync client, Lightroom, exiftool) holds the sidecar lock for more than 2 s
     once. One thumbnail patch times out and the flag is set. Every snapshot created afterwards in this session
     gets no thumbnail request, so its panel row stays blank, although the render needs no sidecar at all.
     Rows that were rendered but not stored are lost after the next open too.
   - Fix options: (a) stop only the writing, and keep requesting and showing renders (the picture goes to
     `snapshotModel_->setRendered`). (b) Clear the flag on the next successful structural write or after a
     timer. (c) Count consecutive failures and stop at 3.

3. **Low, correctness: one bad patch refuses the whole batch of up to 32.**
   `SnapshotWrites.cpp:21` calls `validate(patch.thumbnail)` and line 22 can throw inside
   `snapshotThumbnailKey` for a state it rejects, and `writeSnapshotThumbnailPatches` is called once for the
   run (`SnapshotWriter.cpp` batch branch). The exception becomes `Refused` for every result of the run.
   `setSnapshotThumbnails` also throws `invalid_argument` when the combined result would pass
   `maximumSnapshotSubtreeBytes` (`Sidecar.cpp`, `checkSubtreeSize`).
   - Scenario: a sidecar near the subtree limit gets a batch of patches. The combined result is over the limit,
     so all patches are refused, although each alone would fit. `requestedKeys` already records them, so they
     are never retried that session. A few oversized or odd patches thus drop the thumbnails of others.
     Step A deviation 8 knew the first case; the second is not mentioned.
   - Fix: on `invalid_argument` from a batch of more than one, retry the patches one by one (still in the
     writer, so the order holds), and report each result on its own.

4. **Low, correctness: `finishSnapshotWrite` / `adoptSnapshots` can move the session backwards.**
   `EditSession.cpp:369` (`adopt(update.collection)`) and the `fresh` path in `snapshotWriteLanded` replace the
   collection unconditionally. An external change processed by `sidecarChangedOnDisk` (`adoptDiskSnapshots`)
   or a Save's `refreshSnapshots()` on the GUI thread may already have adopted a newer revision when an older
   write result, or a `fresh` read taken right after a conflict, lands.
   - Scenario: Rename commits at R+1 and its result is queued. Before the GUI runs it, a Save hits
     `SnapshotBaseMissing` for a different reason and refreshes at R+2 (another program added a snapshot). The
     Rename result then adopts the R+1 collection, which lacks the new snapshot, and the session revision goes
     back to R+1. The next structural write conflicts and refreshes again. Narrow window, self-healing.
   - Fix: ignore a collection whose revision is lower than the session's (`adopt` and `adoptSnapshots`), and
     still apply the remap of references.

5. **Low, watcher: the own-write stamp is noted after the event may already have been judged.**
   `SnapshotWriter.cpp:161` reads the stamp on the writer thread after the commit; the stamp reaches
   `FolderWatcher::noteOwnWrite(sidecar, stamp)` only when the posted result runs on the GUI thread
   (`MainWindow.cpp`, `snapshotWriteLanded`). Before this gate every own write noted its stamp synchronously
   on the thread that processes the watcher's events; now the two are ordered only by when each thread posts.
   - Scenario: the writer is descheduled between the commit and `fileStamp`/post while the watcher's recheck
     job (two thread hops) reads the file and posts first. `onSidecarRefreshed` finds no matching stamp and
     emits `sidecarChangedExternally`. In the common case this is benign, because snapshot-only changes go
     through `adoptDiskSnapshots()` and return. It costs a full `openDocument()` (sidecar parse plus image
     metadata) on the GUI thread per false alarm, and a thumbnail burst can produce several.
   - Related: a Save on the GUI thread that commits between the writer's commit and its `fileStamp` leaves the
     writer's own stamp (S1) unknown, and the Save notes S2 for both. A recheck that read S1 is reported as
     external. The four-stamp memory cannot help, because S1 was never recorded. Accepted in the plan; I
     mention it so the failure mode is written down.
   - Fix options: (a) have the writer take the stamp before it releases the lock (the core call could return it
     from `updateSidecar` after the rename), which makes the stamp atomic with the commit. (b) Let
     `sidecarChangedOnDisk` skip the `openDocument()` while `structuralTicket` is set or the writer's
     `pending()` is nonzero, and recheck once when it drops to zero.

6. **Low, ordering: stale thumbnail patches delay structural writes for another photograph.**
   The queue is FIFO across photographs (`SnapshotWriter::take`), and a photograph switch does not cancel
   patches already queued for the old one (the thumbnail generation drops only worker jobs).
   - Scenario: open a photograph with 30 snapshots whose thumbnails are all missing. Each result becomes its own
     one-patch request when the writer is idle, so up to 30 commits happen. Switch to photo B and press
     Ctrl+N. The Create waits behind the remaining patches of A (each a parse and write of a sidecar full of
     JPEGs). The panel shows busy, then the result arrives. It is latency only, but a user can see it.
   - Fix options: (a) drop queued patches whose owner is not the current serial when a new owner submits.
     (b) Let the destructor's rule (patches are derived data) apply to a `cancelPatches(owner)` call from
     `showPhoto`.

7. **Low, robustness: `onResult_` is called outside any `try`.**
   `SnapshotWriter.cpp:177` runs the callback on the writer thread. The contract says it must not throw, and
   MainWindow's callback allocates (`invokeMethod` with a moved result). A `std::bad_alloc` there would
   terminate the process, and `running_` would not be decremented. The risk is tiny; a `try/catch (...)`
   around the call, logging through `qWarning`, costs three lines.

8. **Low, tests: gaps in the writer's cases.**
   - There is no test that a discard goes through the writer (`SnapshotDiscardUnreadable` is only in
     `test_Snapshots.cpp` and the window test), so the `written=false` / no-update result is not pinned
     (`Outcome.written` false, no stamp).
   - There is no test for a result of an older session of the same photograph (finding 1) or for results
     arriving after a photograph switch.
   - The shutdown test uses a 150 ms sleep; the author already flagged it. A deterministic alternative is
     to read `pending()` from a second, long-lived object, or to add a test-only hook in the destructor after
     `stopping_` is set.
   - `detail::sidecarLockHeldHook` is a plain global `std::function`. Every test assigns it while a writer
     thread of the same test is alive, and the "Save waits" test resets it (`Reset` is declared after `writer`,
     so it runs first) while the writer thread may still be about to call it. The accesses are ordered by the
     queue mutex in practice, but a ThreadSanitizer run would report the unsynchronised read. A small
     `std::atomic<void (*)()>` or a mutex around the hook would make it clean.
   - There is no test for the write timing out for a batch (`LockTimeout` on several patches), so the "every
     result of the run gets the failure" branch is untested.

9. **Info, discard semantics worth stating in the dialog.**
   `discardUnreadableSnapshots` re-reads under the lock and acts on what is there, but the confirmation shows the
   count the session had. An older arraw or another program can add unusable entries without changing
   `snapshotRevision`, so the revision check cannot catch it, and the user confirms N but removes more. The
   dialog does say "This cannot be undone" and names the file. A cheap fix is to return the removed count in
   `SnapshotUpdate` and show it in the status line afterwards, so a surprise is at least visible.

## Verified

- **Order and ownership.** One FIFO and one consumer deliver results in submission order. A thumbnail result
  can never overtake the structural write it follows, and a structural result carries the thumbnails that were
  stored before it, so `adopt()` of a structural update never loses a patch that landed earlier. A patch that
  lands after a structural update is applied on top of it (`finishSnapshotWrite` sets the thumbnail on the
  snapshot, which exists in the adopted collection or is skipped).
- **Batching.** Only adjacent patches for the same photograph merge, and a structural request ends a run, so no
  request is reordered past another. A refused or unmatched patch is `written=false` and the rest still land
  (test at `test_SnapshotWriter.cpp:302`). The stamp is attached to all results of a run, including unwritten
  ones; harmless.
- **Revision rules.** A patch ignores the revision by design, and the sidecar writer never changes it for a
  patch. A Create, Rename, Delete or discard uses the revision the session held when it prepared the write.
  With one structural write in flight (`structuralTicket`) the session's revision is the one the file holds,
  except in the cases in findings 1 and 4.
- **Save and the writer.** The Save path rereads under the lock and patches only its own fields, so it keeps
  snapshots the writer added. If a Delete lands first and the Save's base is gone, `SnapshotBaseMissing` leads
  to `refreshSnapshots()` and one retry, and `survivor()` maps the base to the nearest listed ancestor. If the
  Save lands first, the Delete remaps `savedBase` in the file to the parent, matching the session's `adopt`.
  Both orders end with file and session agreeing on the saved base.
- **Deadlocks.** The writer holds `mutex_` only to touch the queue and flags. The GUI thread takes the sidecar
  lock (Save) and waits up to 2 s; the writer takes it and waits up to 2 s; neither waits for the other's
  thread otherwise, since delivery is a queued post. `Qt` treats a live lock owned by the same process as busy,
  so two threads in one process wait for one another as two processes would (the test covers it). A lock held
  over 5 s (`sidecarStaleLockMilliseconds`) is broken by Qt even if its owner is alive, as before.
- **Shutdown.** The destructor sets `delivering_=false` and `stopping_`, drops queued patches under the lock,
  and joins. `take()` returns work until the queue is empty, so queued structural writes are carried out, each
  bounded by the lock timeout. The window may block up to about two lock timeouts while closing (a patch batch
  in flight plus one structural write), only when another program holds the lock. `~MainWindow` runs the member
  destructors with the `QObject` still alive, so a callback that already passed `delivering_` can still post.
- **Queued work after the session changes.** A patch or structural write for a session that is gone is still
  carried out (the sidecar is the owner of the truth) and its stamp is still noted (`snapshotWriteLanded` notes
  it before the serial check). Only the session-side application is skipped, see finding 1.
- **Errors.** `Conflict` carries the collection read right after, and a failed read still reports the conflict.
  `LockTimeout` / `Refused` / `Error` map from the exception type and apply to every result of a batch. An
  exception from the hook is caught. Non-structural failures are logged, not shown. No path leaves
  `structuralTicket` set after its result lands.
- **Own-write memory.** `noteOwnWrite(sidecar, stamp)` runs on the GUI thread only (the map is not shared).
  The four-stamp deque deduplicates and evicts the oldest; the test covers out-of-order and a fifth stamp. The
  old one-argument overload now feeds the same deque, so a Save's stamp does not displace the writer's.
- **Discard.** Checked against `readSnapshotsIn`: damaged nodes, repeated ids and entries past the limit are
  unusable and removed from a usable list; a list that is not one sequence lists nothing, so removing every
  occurrence keeps no readable snapshot; the repaired parents are written back (including the cut cycle edge);
  a saved base that names no listed snapshot is dropped; the counter is lifted above every raw id of every
  occurrence, with the overflow case guarded; the revision is raised; a final reread with a refusal if
  anything unusable remains happens inside the lock, so a failure writes nothing. The revision is checked
  before the nothing-to-do return, so a stale caller still gets `Conflict`.
- **Thumbnail key before the lock.** The key (two 64 KiB reads) is computed outside the lock, so the lock is
  held only for the parse and the write, which keeps the 5 s stale threshold far away.
- **Priority change and `storedKey`.** The developed jobs of visible shots go ahead of snapshot jobs, and a job
  whose stored key is current is dropped without a render or a result. Nothing there touches the writer; the
  GUI's `requestedKeys` stops a re-request loop.

## Questions for the author

- Is it acceptable that a photograph switch within the write's latency shows a false "changed by another
  program" (finding 1)? If yes, a one-line note in the plan would help the next reader.
- Should the thumbnail failure flag (finding 2) ever clear?
- For the open question on the error text: the runtime message for a damaged list is thrown by the core and
  seen by the CLI, Python and the GUI. I would keep it neutral. The GUI already adds its own dialog title
  ("Cannot Remove Unreadable Snapshot Entries") and the panel notice offers the action.
