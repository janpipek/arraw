# A render resolves from a photo and a request, and the plan starts at the file

ADR 011 decides what the stages are and how they fuse, and says a plan "holds
everything the settings imply". It does not say what a plan is resolved *from*,
and a review of it showed what that costs: checkpoint validity is decided by
comparing plans, so anything that changes what a stage computes without
appearing in the plan makes two different renders compare equal. Two such
things already exist in these ADRs. ADR 007 permits an early downsample for a
preview where an export reads full resolution, and ADR 005 bakes demosaic,
white balance and highlight handling into import, where "changing demosaic or
highlight handling means re-decoding" — the same photograph, the same settings,
different pixels.

Today `develop(source, settings)` takes pixels somebody else loaded. That is
why the question has not come up: loading is outside the contract, so nothing
in the contract can describe it.

## Decision

**A render resolves from a photo snapshot and a render request.**
`planFor(photo, request)` replaces `planFor(encoding, settings)` — which is the
same function with its inputs narrowed to what one stage needed, so today's
code is an instance of this rather than a contradiction of it.

**The photo carries metadata and develop state; the pixels are the plan's job.**
That is ADR 001 unchanged: `Photo` exists when metadata, sidecar binding and
editable develop state form a coherent document, and "decoded pixels do not
belong to `Photo`". What it must carry for a plan to be resolvable without
touching a pixel is the camera's colour description — `CameraNative`: the
composed matrix, the daylight scale, the as-shot and applied multipliers —
which today is attached to the decoded buffer's `ColorEncoding` by
`RawImport.cpp`.

That is cheap, and it was checked rather than assumed: LibRaw's `open_file`
populates `imgdata.color` and `imgdata.sizes` identically to after `unpack`, so
the colour description does not need the mosaic, let alone the demosaic. (The
committed fixtures are synthetic, with an identity `rgb_cam` and unit `pre_mul`,
so the matrix path deserves one confirmation against a real camera file before
anything depends on it.) For a photograph that is not a RAW, `QImageReader`
gives size and format without decoding, and the encoding is sRGB.

**Decoding is the plan's first stage, not something that happened earlier.**
Its block names the file, the parameters the decode runs with — demosaic
algorithm, highlight handling, decode resolution — and a stamp of the file's
size and modification time as observed when the metadata was read. Everything
that decides which pixels come out is therefore compared by value, along with
everything else, by the fold ADR 011 already describes.

**A source has no issued identity, and no generation counter.** Both were
considered and neither is needed once decoding is described by the plan: what a
counter would have tracked is precisely what the decode block now states. What
remains is the file changing underneath an open document, and the stamp catches
that as far as a size and an mtime can — which is a filter, not a proof. A
persistent cache, whose checkpoints outlive the process that made them, needs
to say more about the file than this; that is a decision for whoever builds one.

**Different settings mean a different snapshot, never an override channel into
the renderer.** A snapshot is metadata and develop state with no pixels, so
another one is cheap, and ADR 001 already has export holding one while the GUI
edits on. The command line's `--exposure` renders the sidecar's snapshot with
exposure replaced; a preset, a before-and-after and a soft proof are the same
move. A second settings input to the render would need a merge rule, living
where it could drift from the one the GUI applies.

**A caller holding pixels supplies a checkpoint, and a supplied checkpoint is an
input, never a cache entry.** Tests build buffers, and one day something will
arrive from somewhere that is not a file. "Here are pixels, start after decode"
is what `resumeFrom` means, so this needs no second door into the pipeline —
but such a checkpoint has no provenance anyone can verify, so nothing may be
cached from it or reused against it.

## Consequences

- **ADR 005's baked-in decode becomes a migration with a finish line.** The
  rule is that a source revision covers exactly what the plan does not yet
  describe. Today that is all of LibRaw's processing; as demosaic and highlight
  handling become stages with their parameters in the decode block, the covered
  part shrinks until only the file stamp is left.
- **ADR 007's cache argument becomes structural.** With the as-shot gains in the
  decode block and the balanced matrix in the pointwise one, a temperature
  change cannot reach the decode prefix — not by discipline, but because the
  fold cannot see it.
- **ADR 007's free function is answered rather than replaced.** It says
  `develop` "becomes a processor object at the first cache"; this says what that
  object is resolved from. `develop(source, settings)` stays what it is until
  then.
- **`Photo` has to become a document before any of this executes.** It is an
  empty struct today. The first step is splitting the metadata read from the
  pixel decode in `RawImport`, so that a photo can be resolved without
  demosaicing — real work with real tests, deliberately not done here.
- **A snapshot stays cheap only while develop state stays light.** When brush
  rasters and long spot lists live in it, a snapshot shares them rather than
  copying, and the plan compares them by revision, as ADR 011 already says for
  large payloads.
