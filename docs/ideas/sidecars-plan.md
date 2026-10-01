# Sidecars and settings as JSON — execution plan

Status: implemented on branch `sidecars`. The format and its guarantees are
recorded in [ADR 019](../adr/019-develop-settings-live-in-an-xmp-sidecar-and-in-json.md).

## Goal

Develop settings and culling marks are stored beside the photograph in an
`.xmp` sidecar, and the same settings can be written to and read from JSON.
`openPhoto` picks up the sidecar, so `arraw-cli export` renders each file
"through its own sidecar", as `desired-features.md` promises. Python gets the
same behaviour through the bindings.

Not in scope:
- `crs:` reading and writing (Lightroom interop). This follows ADR 008: `crs:`
  is neither read nor written yet, and existing `crs:` content is preserved
  untouched.
- Presets, history, and the GUI's culling and saving interface.
- JPEG-embedded XMP.

## Decisions

| Question | Answer |
|---|---|
| Namespace | `arraw:` (`http://ns.arraw.org/develop/1.0/`, final URI still to confirm: it becomes permanent with the first sidecar written, and needs a domain the project controls), with keys taken from the descriptor table. `crs:` becomes a second key column later |
| Standard XMP | `xmp:Rating` (-1 reject, 0–5) and `xmp:Label` read and written in their own namespaces from the start (ADR 008) |
| Sidecar name | `IMG_1.xmp` (Lightroom) for a RAW that no other RAW shares a stem with, and for a file no other image shares a stem with. Anything else uses its whole name, `IMG_1.JPG.xmp`, so two files never share one sidecar |
| Marks on write | A mark is written only when it differs from what reading the file gave, so a rating or label arraw cannot represent (a rating of `9`, a label `Rot`) stays as the other tool wrote it until someone changes it |
| XML | Qt Xml (already found by CMake), DOM-based, so unknown elements, attributes and namespaces survive a write |
| JSON | Qt's `QJsonDocument` (Qt Core), so no new dependency |
| Reading | The sidecar is applied onto defaults. Out-of-range values are clamped with a warning, and unknown keys or wrong types are skipped with a warning (ADR 008) |
| Writing | Explicit only. Opening never writes. The file is replaced atomically (`QSaveFile`) |

## One codec, two formats

The table is walked once per format. Each leaf type encodes to a small neutral
value, and each format only maps that value to its own syntax. A per-type
switch therefore exists once, not once per format.

```cpp
// src/core/SettingCodec.h (private)
using Encoded = std::variant<std::monostate /*null*/, bool, double, std::string,
                             std::map<std::string, double>>;
Encoded encode(const FieldDescriptor&, const DevelopSettings&);
void decode(const FieldDescriptor&, const Encoded&, DevelopSettings&, DiagnosticLog&);
```

| Leaf type | Encoded | JSON | XMP attribute |
|---|---|---|---|
| float, double | number | number | shortest round-trip text |
| optional<float> | number or null | number or `null` | absent when unset |
| bool | bool | bool | `True` or `False` (XMP convention) |
| WhiteBalanceMode, QuarterTurn | string | `"asShot"`, `"clockwise90"` | same string |
| optional<UprightCropRect> | map {left, top, right, bottom} or null | object or `null` | `"l,t,r,b"`, absent when unset |
| CropAspect | `"free"`, `"original"` or map {ratio} | `"free"` or `{"ratio": 1.5}` | `"free"`, `"original"` or `"1.5"` |

The enumeration names live next to the enumerations as `constexpr` tables, so
the CLI can later parse `--white-balance` from the same names.

**Partial application is the primitive.** Reading a document means applying the
keys it contains onto a base: defaults for a sidecar, or the current settings
for a future preset. A key that is absent leaves the base alone. In JSON,
`null` sets an optional field to "unset". Writing always emits every key.

## Public API

```cpp
// include/SettingsJson.h
std::string settingsToJson(const DevelopSettings&);   // {"arraw": 1, "settings": {...}}
DevelopSettings applySettingsJson(std::string_view json, DevelopSettings base,
                                  DiagnosticLog& = discardedDiagnostics());

// include/PhotoMarks.h
enum class ColorLabel { Red, Yellow, Green, Blue, Purple };
struct PhotoMarks { int rating = 0; /* -1 reject, 0..5 */ std::optional<ColorLabel> label; };

// include/Sidecar.h
std::filesystem::path sidecarPath(const std::filesystem::path& photo);
struct SidecarContents { DevelopSettings settings; PhotoMarks marks; };
std::optional<SidecarContents> readSidecar(const std::filesystem::path& photo,
                                           DiagnosticLog& = discardedDiagnostics());
void writeSidecar(const Photo&);   // preserves everything it does not own
```

- **`Photo`** gains `marks()` and `with(PhotoMarks)`.
- **`openPhoto`** reads the sidecar when there is one. A photograph without a
  sidecar opens exactly as it does today.
- **Versioning:** the JSON document carries `"arraw": 1`, and the XMP carries
  `arraw:version="1"`. A newer version is still read, with a warning.

## Steps

1. **Codec and JSON.** This step adds:
   - enumeration name tables;
   - `SettingCodec`;
   - `settingsToJson` and `applySettingsJson`;
   - new `Notice`s (`SettingClamped`, `SettingUnknown`, `SettingMalformed`,
     `NewerSettingsVersion`).

   Tests cover:
   - a round trip for every row, driven from the table;
   - partial application;
   - clamping, unknown keys, wrong types and `null`, each with its warning.
2. **Marks and sidecars.** This step adds `PhotoMarks`, `sidecarPath`,
   `readSidecar`, `writeSidecar`, and `openPhoto` reading the sidecar. Tests
   cover:
   - a round trip for settings and marks;
   - preservation: a fixture sidecar with `crs:` settings, a foreign namespace
     and an unknown `arraw:` attribute survives a write in meaning, apart from
     what we own. Not byte for byte: Qt re-sorts attributes, drops the
     packet's padding and trailing newline, quotes the XML declaration with
     single quotes and rewrites its encoding as UTF-8. ADR 019 states this;
   - the pair-naming rule;
   - an atomic write: a failed write leaves the old file byte-identical;
   - a foreign label or rating (localised, out of range, `3.0`) surviving a
     write, a newer-version sidecar and a non-UTF-8 sidecar being handled
     without damage;
   - clamping on read.
3. **CLI.** `export` applies only the flags that were given, on top of each
   file's sidecar. Today it replaces the settings wholesale with defaults plus
   flags (`ExportCommand.cpp`, `openPhoto(input, log).with(request.settings)`).
   `--no-sidecar` ignores sidecars. The CLI never writes a sidecar in this
   phase.

   *Done.* The flags are a list of (descriptor, encoded value) applied with
   `decode` over `openPhoto`'s settings; `--no-sidecar` opens with
   `Photo(path, readImageMetadata(path, log))`, so no API changed. See ADR 006.
4. **Python.** This step adds:
   - `DevelopSettings.to_json()` and `DevelopSettings.from_json(text, base=None)`;
   - `Photo.marks` and `with_(rating=…, label=…)`;
   - `arraw.write_sidecar(photo)` and `arraw.sidecar_path(path)`;
   - `open` reading the sidecar;
   - regenerated type stubs.
5. **ADR 019** records the format, the naming rule and the preservation
   guarantee. `desired-features.md` is updated where it promises `crs:`.

Execution follows the Python plan: small workflows with Sonnet implementing and
Opus reviewing, and a review between steps.
