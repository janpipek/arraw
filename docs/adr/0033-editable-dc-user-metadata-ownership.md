# Editable descriptive User Metadata is owned in five `dc:` properties

arraw makes Title, Caption, Keywords, Creator, and Copyright editable in the
[[Info Panel]] and writes them to the develop sidecar as Dublin Core: `dc:title`,
`dc:description`, `dc:subject`, `dc:creator`, `dc:rights`. This **amends
[ADR 0026](0026-shared-xmp-property-ownership.md)**, which previously listed only
the `arraw:` namespace, the modeled `crs:` properties, and `xmp:Rating`/`xmp:Label`
as owned. arraw now also owns exactly these five `dc:` properties; every other
`dc:` property (and the whole rest of the packet) is still preserved semantically,
unchanged, on save.

We chose to own *specific named* `dc:` properties rather than the whole `dc:`
namespace so that a foreign tool's other Dublin Core metadata — and in particular a
keyword hierarchy in `lr:hierarchicalSubject` — survives an arraw save untouched.
arraw writes a flat `dc:subject` bag only; it never authors a hierarchy
(cataloguing is out of scope — "Not a DAM, not a catalogue").

## Read precedence

A field's displayed value is resolved **sidecar `dc:` → embedded XMP in the file
(`LibRaw imgdata.idata.xmpdata`) → camera EXIF**. The sidecar is authoritative
because it is what arraw and other XMP editors write; the embedded packet and EXIF
are fallbacks that pre-fill a field only until the user authors its `dc:` value.
Two read sources, one extraction routine: the same `dc:`-parsing code runs over the
sidecar document and over the embedded blob (logic exists once).

## Persistence

Descriptive fields follow the existing User Metadata contract (Rating / Colour
Label), not the develop contract: each edit saves to the sidecar **immediately** on
field commit (focus-out / Enter) and is **not** placed on the develop undo stack
(which is per-image and cleared on image switch). Pending edits are flushed before
switching images.

## Considered Options

- **Own the whole `dc:` namespace.** Simpler merge, but would erase any `dc:`
  property arraw does not model — including `lr:`-adjacent keyword hierarchies and
  fields written by digiKam/Lightroom. Rejected for the same reason ADR 0026
  rejected owning whole `crs:`/`xmp:`.
- **Own nothing in `dc:` (display-only).** Keeps the merge trivial but makes the
  feature impossible — there would be nowhere standards-compliant to write
  user-authored keywords/title. Rejected.
- **Embedded-XMP-first precedence.** A stale value baked into the file would
  outrank a fresh sidecar edit. Rejected; the sidecar is the edit surface.
- **Author a keyword hierarchy (`lr:hierarchicalSubject`).** Powerful, but it is a
  DAM/catalogue feature the project explicitly disclaims. Rejected; an existing
  hierarchy is preserved but never authored.

## Consequences

- `XmpSidecar` gains namespace-aware read/write of the five `dc:` properties with
  their correct RDF containers: `dc:title`/`dc:description`/`dc:rights` as
  `rdf:Alt` (`x-default`), `dc:subject` as `rdf:Bag`, `dc:creator` as `rdf:Seq`.
- A `dc:`-only save replaces just these five properties and preserves the rest of
  the packet, exactly as ADR 0026 already requires for owned properties.
- Embedding metadata into **exported JPEGs**, and editing metadata on **standalone
  non-RAW files** (whose metadata is embedded, not in a sidecar), are deliberately
  **deferred** — both need a metadata *writer* for in-file segments, which Qt does
  not provide and which would likely mean a new dependency (e.g. exiv2). Tracked in
  the plan, not built here.
