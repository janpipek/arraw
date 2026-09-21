# Integration tests over committed fixtures

Every test so far checks one function against buffers the suite itself
constructed. `exportImage` is covered well that way; `loadImage` is not covered
at all, and nothing crosses the seam between them. That is a real gap rather
than a tidy one: the working space is an *internal* choice, so no unit test can
observe whether carrying an image through linear Rec.2020 and back costs
anything. Only a file that goes in one end and comes out the other can.

The alternative is to keep testing each stage against its own expectations and
trust composition. That is what a synthetic-buffer suite does, and it would
never notice a working space that crushes shadows, an import that narrows
precision, or an export that undoes a conversion import never applied.

## Decision

The engine is tested end-to-end through file-to-file round trips, alongside the
unit tests, not instead of them. `tests/test_RoundTrip.cpp` loads a real image,
exports it, and compares the two files. The tests are tagged `[integration]` so
they can be selected or skipped as a group.

Such a test needs an input its own codecs did not produce, and an oracle that is
not the code under test. Both follow from one rule: **integration fixtures are
committed binaries produced by a committed generator.**
`tests/fixtures/make_fixtures.py`, run by hand through `just fixtures`, writes
the PNGs with `zlib` and `struct` alone — no image library, least of all Qt —
and the PNGs are committed and read directly by the tests. The build does not
run the generator, so neither the build nor CI needs Python. Comparison decodes
both files with `QImage`, never through `loadImage`.

Had the tests written their own input with Qt, a Qt codec fault would cancel
itself out on both sides and the round trip would pass straight through it.

## Consequences

- Fixture and generator can drift, because nothing checks that the committed
  PNGs still match the script. A byte-level check was considered and rejected:
  PNG output is not reproducible across zlib versions, so it would have needed a
  PNG decoder in the generator to defend against one careless commit.
  `tests/fixtures/README.md` states the rule instead.
- Tolerances are measured, then asserted, per format — never guessed. The
  round trip through a lossless *format* is bounded at **1 code of 255** rather
  than being exact, which is Qt's colour
  transform not being exactly self-inverse rather than anything the working
  space costs; sRGB fits inside Rec.2020 and linear U16 resolves a shadow code
  into roughly twenty steps. This bounds what any future claim of "export is
  lossless" may say, and it should be re-measured if the working space of
  ADR 003 changes.
- A single shared tolerance is not viable. JPEG at quality 100 measures 3 codes,
  but at the `ExportOptions` default of 90 the same image measures **194** — a
  saturated green edge decoded as `(66, 184, 194)`, which is libjpeg subsampling
  chroma. One global bound would have to be 194 wide and would assert nothing.
  The JPEG case therefore states a different claim from the lossless ones:
  bounded degradation, not preservation.
- **`loadImage` is still not tested.** This decision does not cover it, and
  "we have integration tests" must not be read as though it did. The round trip
  exercises exactly one path through import — 8-bit, RGB, sRGB-tagged. Its
  `chooseLayout` float branch, its "an untagged file is sRGB" fallback, and its
  honouring of a non-sRGB embedded profile remain unexercised. Fixtures for all
  of them are generated and committed, unused, waiting for that work.
