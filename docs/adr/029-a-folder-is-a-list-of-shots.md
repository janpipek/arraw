# A folder is a list of shots

A photographer culls a shoot, not a file. A camera writing RAW+JPEG makes two
files of one capture, and the film strip, `arraw-cli` and Python all need the
same answer to "what are the frames in this folder, in what order, and which
of them do I want". This is step 2 of the film strip plan
(`docs/ideas/filmstrip-plan.md`); the rules are main's, kept.

## Decision

**`arraw::Shot` is in the library** (`include/Shot.h`): a `primary` path, the
file that is shown and developed, and `companions`. Folder discovery and
pairing live beside the decoder rather than in a widget, so the command line,
Python and the strip cannot disagree, and the rules are tested with no Qt
application.

**Pairing, as main.** Files pair when they have the same parent folder and the
same stem, the stem compared case-insensitively. The RAW is the primary and
the standard images (JPEG, PNG, TIFF) of that stem are its companions. A file
with no RAW partner stands alone, and standard images with one stem do not pair
with each other. Where main was ambiguous in its prose and clear in its code, we
follow the code: when *two* RAWs share a stem (`IMG_1.CR2` and `IMG_1.NEF`), every
file of that stem stands alone, the JPEG included, because nothing says which
RAW it belongs to. Unsupported files and sidecars are dropped.

**What counts as an image** is one list, `supportedImageExtensions()`: every
RAW extension LibRaw opens (`rawimport::openedRawExtensions`, 21 of them), then
`jpg`, `jpeg`, `png`, `tif` and `tiff`. The same RAW list decides which file of a
shot is its primary, and when a RAW and a JPEG must not share a sidecar (ADR
019), so what a folder shows, how it pairs and how it names sidecars cannot
disagree. The decoder's own fast path claims fewer by name
(`rawimport::rawExtensions`) for the reason ADR 005 gives, and still decodes the
rest by content. `isSupportedImage` goes by the extension alone, never the
content, so listing a folder opens no file. The open dialog's filter is built
from the same list.

**`groupShots` is pure**: it takes paths and looks at no filesystem, so it can
be tested with invented ones. `listShots(folder)` feeds it the regular files of
the folder, not recursively and not hidden ones, and throws `std::runtime_error`
for a folder it cannot read. `formatLabel` is `ARW`, `JPEG` or `ARW+JPEG`: the
primary's canonical format, then each companion's, each once.

**Natural order.** Shots are ordered by the primary's file name, compared
case-insensitively with runs of digits as numbers, so `IMG_2` comes before
`IMG_10`. Ties (`img_2` and `IMG_02`) are broken by folder and then by the exact
bytes of the name, so the order never depends on the order of the input or of
the directory iteration. Companions are ordered the same way. The comparison is
ASCII-only; other scripts compare by byte.

**One filter, `MarksFilter`** (`include/MarksFilter.h`), ported from main's
film strip filter. A rating is ordered, so it is a threshold, `minRating` N
meaning "at least N stars", which excludes rejects and unrated photographs. A
reject can never satisfy that, so `rejectsOnly` is its own switch, and asking
for both is `std::invalid_argument`. Colours are unordered, so `labels` is a set
matched by OR, where empty means any (and a photograph with no label matches
no non-empty set). The two dimensions combine by AND. It is a value with
`isActive()` and `matches(PhotoMarks)`; a window's filter proxy will hold one.

**The command line** (see ADR 006) accepts folders. A folder stands for the
primary files of its shots; companions are neither exported nor listed apart,
`info` shows them under their shot with the format label, and files given
directly behave as before. `export` and `info` take `--min-rating N`,
`--rejected` and `--label NAME` (repeatable, case-insensitive), which test the
marks in each photograph's sidecar, and the defaults without one or under
`--no-sidecar`. A photograph the filter leaves out is counted in one summary
line, never an error and not a reason for a non-zero exit. An empty folder says
"no photographs" and succeeds. A sidecar that cannot be read fails its
photograph under a filter, since its marks are unknown. The flags are shared by
both commands through one helper (`ShotInputs`).

**Python** binds `Shot` (`primary`, `companions`, `format_label`),
`group_shots`, `list_shots`, `MarksFilter`, `is_supported_image` and
`SUPPORTED_EXTENSIONS`, in ADR 018's style.

## Consequences

- The film strip's model is `listShots` plus a `MarksFilter` in its proxy; it
  adds the folder watcher, which re-lists and diffs, and the thumbnails.
- A folder is read once and all at once: `listShots` is a snapshot, not a
  stream. A very large folder costs a directory scan and a sort, no file opens.
- The shot rules are not the sidecar rules: ADR 019 names a sidecar by stem
  for a RAW and by the full name for a JPEG that shares its stem, which is why a
  shot's companions never share the primary's sidecar.
- Filtering by marks reads a sidecar twice for each photograph that passes (once
  to test, once to open); sidecars are small, and the code stays in one place.
