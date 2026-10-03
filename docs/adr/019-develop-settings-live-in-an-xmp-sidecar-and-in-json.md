# Develop settings live in an XMP sidecar, and in JSON

`desired-features.md` promises that edits are never baked into the file: they
live in an `.xmp` sidecar beside the photograph. [ADR 008](008-develop-settings-and-their-descriptors.md)
already decided the rules that sidecar must follow:
- settings are written in an `arraw:` namespace;
- `crs:` is neither read nor written yet;
- values out of range are clamped on read, with a warning;
- XMP arraw does not understand is never deleted.

The [sidecar plan](../ideas/sidecars-plan.md) built it, together with a JSON
form of the same settings. This ADR records the file format and the guarantees
around it. The Python side follows [ADR 018](018-python-binds-the-public-api-and-nothing-else.md).

## Decision

**One codec, two spellings.** Every develop setting is a row of the
descriptor table. `SettingCodec` (private, `src/core`) encodes each leaf type
to a small neutral value: null, bool, number, string, or an ordered list of
named numbers. It decodes that value back, clamping and warning as ADR 008
asks. JSON and XMP only spell that value. So a new setting needs no change to
either format, and the formats cannot disagree about a value.

| Leaf | JSON | XMP attribute |
|---|---|---|
| float, double | number | shortest round-trip text |
| optional float | number, or `null` for unset | absent when unset |
| bool | `true`, `false` | `True`, `False` |
| enumeration | its name: `"asShot"`, `"clockwise90"` | the same name |
| crop rectangle | `{"left", "top", "right", "bottom"}` or `null` | `"l,t,r,b"`, absent when unset |
| crop aspect | `"free"`, `"original"` or `{"ratio": r}` | `"free"`, `"original"` or `"r"` |

Floats round-trip exactly: every float32 value comes back with the same bits.

**Reading applies onto a base.** A document sets the keys it contains and
leaves the rest of the base alone. A sidecar applies onto defaults; a future
preset applies onto the current settings; the command line's flags apply onto
each file's sidecar. `null` in JSON unsets an optional. A problem with one
setting is a warning and leaves that setting as the base had it:
- an unknown key (`SettingUnknown`);
- a value of the wrong shape (`SettingMalformed`);
- a number out of range, which is clamped (`SettingClamped`).

A problem with the whole document is an error: unreadable JSON, a document
without its structure, or XML that does not parse.

**JSON is `{"arraw": 1, "settings": {…}}`,** with every key in table order.
`settingsToJson` and `applySettingsJson` are public. A newer version is read
with `NewerSettingsVersion`.

**The sidecar holds `arraw:` attributes and standard marks.** The settings are
attributes in `http://ns.arraw.org/develop/1.0/`, one per descriptor key, plus
`arraw:version="1"`. That URI is a placeholder, and it becomes permanent with
the first sidecar written outside this repository. Culling marks are
`xmp:Rating` (-1 reject, 0 to 5) and `xmp:Label` (Red, Yellow, Green, Blue,
Purple), in their own namespace as ADR 008 requires. `PhotoMarks` carries
them on `Photo`, beside its settings.

Reading accepts the settings as attributes or as simple child elements, on any
`rdf:Description`. It accepts a rating written as a real (`3.0`) or with a sign
(`+2`). Reading also reports, without interpreting it, who else wrote in the
file: `xmp:CreatorTool`, and each namespace other than arraw's and XMP's own
that holds a property on an `rdf:Description`, with its prefix and its count of
top-level properties (`SidecarContents::creatorTool` and `others`; a table maps
the well-known URIs, `dc:` among them, to a tool or vocabulary name). Writing
puts every key as an attribute on the description that already holds `arraw:`
content, or on the first one. It removes any child-element form, so the file
says each thing once.

**The sidecar is named as Lightroom names it, except where two files would
share one.** A RAW keeps `IMG_1.xmp` unless another RAW shares its stem. A
file that shares a stem with no other image also keeps `IMG_1.xmp`. Anything
else uses its whole name, `IMG_1.JPG.xmp`, so a RAW+JPEG pair, two RAWs or a
HEIC+JPEG pair never share a sidecar. The RAW extensions are the ones LibRaw
opens. Siblings are found by probing candidate names in both cases, so the
cost does not grow with the directory. An existing `IMG_1.XMP` is used when
`IMG_1.xmp` is absent.

**Opening reads the sidecar; only an explicit call writes one.** `openPhoto`
applies the sidecar when there is one. A photograph without one opens exactly
as before. `writeSidecar(photo)` is the only writer. Opening never writes, and
neither does the command line, whose `export` and `info` only read.

**A write keeps everything arraw does not own, by meaning.** That includes
`crs:` settings, other namespaces, nested RDF, unknown elements, and unknown
`arraw:` attributes a newer arraw may have written.

A mark is written only when it differs from what reading the file gives. A
rating or label that arraw cannot represent (a rating of `9`, a label `Rot` or
`Select`) reads as no mark, and stays in the file as the other tool wrote it
until someone changes the mark.

What is kept is the document's meaning, not its bytes. Qt's DOM:
- reorders attributes;
- drops the packet's padding and trailing newline;
- quotes the XML declaration with single quotes.

A declaration of another encoding (Latin-1, UTF-16) is rewritten as UTF-8,
which is what the file is then written in.

**A write never damages what it cannot understand.** The file is replaced
atomically (`QSaveFile`), so a failed write leaves the old one byte for byte.
`writeSidecar` refuses, and leaves the file alone, when:
- the sidecar is not XML;
- it is XML but not XMP;
- its `arraw:version` is newer than this arraw. Its values were clamped to
  this version's ranges on the way in, so writing them back would damage it.

**An unreadable sidecar is loud, not fatal to opening.** `openPhoto` records
`SidecarUnreadable` at error severity and opens with defaults. The command
line fails that file, because exporting it bare would silently drop the
photographer's edits, and it goes on with the batch. `--no-sidecar` exports
the file anyway.

**Warnings name the photograph.** Every diagnostic from reading a sidecar has
the photograph as its subject, as other diagnostics about a file do. The
sidecar's own name appears in the message where it matters.

## Alternatives

- **`crs:` from the start.** This would give Lightroom interop at once. But
  ADR 008's "one truth per file" rule means reading `crs:` needs writing it
  too, with Adobe's semantics for each setting. With the table, `crs:` becomes
  a second key column and a mapping, not a new format.
- **Exiv2 or the Adobe XMP toolkit.** Either is a real XMP library, but it is
  a new dependency for the one namespace arraw owns. Qt Xml is already linked.
  A DOM parsed without Qt's namespace processing, with prefixes resolved by
  hand, writes back what it read. Qt's namespace-aware mode duplicated and
  lost prefix declarations.
- **Darktable's naming (`IMG_1.ARW.xmp`) everywhere.** It is unambiguous, but
  Lightroom would not find the sidecar. The rule above keeps Lightroom's name
  wherever it is unambiguous.
- **Keeping unrepresentable marks in `PhotoMarks`.** This was tried with
  `otherLabel` and `otherRating`. It put storage leftovers in the model, and it
  could not clear a foreign label. Comparing against the file at write time
  keeps the model clean.
- **Writing marks always.** This would overwrite another tool's rating of 9
  with 5 on the first save of an unrelated edit.

## Consequences

- **Adding a develop setting is still two edits,** a field and a row. It then
  appears in JSON, the sidecar, the command line's options, `info` and
  Python.
- **Moving a crop with a rotation or flip (ADR 014) is not implemented.** The
  command line resets an explicit crop to automatic framing, with
  `CropReset`, when its flags change the frame the crop was drawn in. The
  geometry editor will carry it properly.
- **Clamping on read is safe only while Lightroom does not read these files.**
  That is ADR 008's revisit condition, and it now applies to `crs:` interop.

Conditions that require revisiting this decision:

- **Interop with `crs:`.** Clamping on read becomes destructive and must become
  keeping values as read (ADR 008), and the mapping must define what Lightroom
  sees.
- **Settling the namespace URI.** It must happen before any sidecar leaves this
  repository.
- **A GUI that saves.** It decides when `writeSidecar` runs (on every edit,
  debounced, on leaving a photograph), and how a sidecar changed on disk by
  another tool meanwhile is noticed.
