# Shared XMP sidecars are merged by property ownership

arraw shares XMP sidecars with digiKam, Lightroom, and other XMP editors. Saving
must preserve the semantic content of every property arraw does not own, rather
than regenerating the packet from only the fields arraw understands. XML
formatting, prefix choice, and attribute order may change.

Property ownership is narrower than namespace use. arraw owns the complete
`arraw:` namespace, the specific `crs:` properties represented by
`GlobalAdjustment`, and `xmp:Rating` plus `xmp:Label`. A develop save replaces
the owned `crs:` properties and all `arraw:` content; a User Metadata save
replaces only the two owned `xmp:` properties. Every other attribute and RDF
element survives, including unknown properties in `crs:` or `xmp:`.

Sidecar resolution supports both `IMG_0042.xmp` and `IMG_0042.NEF.xmp`. If
exactly one exists, arraw reads and writes that file. If neither exists, arraw
creates `IMG_0042.xmp` for backward and commercial-editor compatibility. If
both exist, the state is ambiguous and arraw refuses to load or save rather
than silently choosing or combining independent metadata histories. Existing
malformed or unreadable sidecars are likewise never replaced. Successful saves
replace the selected sidecar atomically.

## Considered Options

- Rewriting only modeled fields is simple but destroys keywords, captions,
  regions, provenance, and application-specific metadata. Rejected.
- Owning whole `crs:` or `xmp:` namespaces makes merging easier but would erase
  valid properties written by other editors. Rejected.
- Preserving bytes exactly would retain formatting and ordering but makes safe
  property replacement substantially more complex without improving XMP
  interoperability. Rejected in favor of semantic preservation.
- Merging two simultaneously present naming conventions would require conflict
  rules for every property and could combine unrelated histories. Rejected;
  ambiguity must be resolved by the user or another metadata tool.

## Consequences

- XMP parsing and serialization must be namespace-aware and retain arbitrary RDF
  structures, so streaming only the known fields is insufficient.
- Callers continue to use the `XmpSidecar` interface; ownership, filename
  selection, merging, and atomic replacement remain inside that module.
- ADR 0008's statement that foreign tags are dropped is superseded by this
  decision.
