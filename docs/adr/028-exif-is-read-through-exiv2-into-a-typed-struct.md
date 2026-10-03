# EXIF is read through exiv2 into a typed struct

The film strip's tooltips, `arraw-cli info` and, later, the metadata an export
carries all need what a photograph records about its capture: camera, lens,
exposure, time, place. This is the metadata reader ADR 005 deferred until it
had more than one caller.

## Decision

**exiv2 reads it, and exiv2 is required.** LibRaw parses part of the EXIF of a
RAW but not of a JPEG, and Qt's codecs expose none. exiv2 reads RAW (CR3 and
other ISO-BMFF included, which 0.28 enables by default), JPEG, TIFF and PNG
with one API, and it is the library that will later *write* metadata on
export, so one dependency serves both. It is required for the reason LibRaw
is (ADR 005): a build that shows a lens name where another shows none is a
bug nobody can reproduce. It is GPL-2.0-or-later, which a GPL-3.0-or-later
work may use. CMake uses pkg-config on Linux and macOS and exiv2's own CMake
package (`exiv2lib`) on Windows, where vcpkg ships that and there is no
pkg-config; the Windows branch has not been built. It is linked privately to
`arraw`; no exiv2 type is in `include/`. A Python wheel would have to bundle
it with Qt and LibRaw (ADR 018's open conditions).

**A typed struct named after the tags.** `ExifInfo` (in `include/ExifInfo.h`)
has one `std::optional` field per EXIF tag arraw uses, in camelCase:
`make`, `model`, `lensModel`, `dateTimeOriginal`, `offsetTimeOriginal`,
`exposureTime`, `fNumber`, `photographicSensitivity`, `focalLength`,
`focalLengthIn35mmFilm`, `exposureBiasValue`, `flash`, `gps`, `artist` and
`copyright`. A photographer who knows the tag knows the field, and Python and
the JSON of `info` follow with snake_case and camelCase respectively. Not a
key-value map: a map would push the typing, units and sign handling onto
every caller. Not one flat field list with pre-formatted strings either:
`exposureTime` and `fNumber` are `URational`, `exposureBiasValue` an
`SRational`, so 1/250 s is exactly that, and each reader formats it for
itself. The date stays EXIF's `"YYYY:MM:DD HH:MM:SS"` text and the offset
`"+HH:MM"`: converting them needs a time zone the file may not give, and
nothing yet needs a time point. The GPS tags become one `GpsPosition` in
signed decimal degrees and metres; a latitude or longitude without its
hemisphere reference, or beyond 90 or 180 degrees, is no position at all
rather than a guessed one.

**Separate from `ImageMetadata` and `Photo`.** `ImageMetadata` is what a plan
is resolved from (ADR 012) and `Photo` is a document; both are needed to
render and are cheap. EXIF is only shown or copied, and reading it opens the
file again through a second library. `readExif(path, log)` is its own call,
so a render never pays for it and a failure of it never fails an open.

**Empty plus a notice, never an error.** A file exiv2 cannot read, or that has
no EXIF, gives an `ExifInfo` with every field absent and a
`Notice::ExifUnreadable`: `Info` severity when the file simply records
nothing (a PNG, a RAW with only the tags needed to decode it) and `Warning`
when exiv2 failed on it. A film strip full of screenshots must not be a
wall of errors, and `info` drops the `Info` kind because an absent block says
it already. A path that does not exist throws `std::runtime_error`, as
`readImageMetadata` does. A field whose tag is present but malformed (wrong
type, a zero denominator in a coordinate, out of range) is absent, not
reported.

**Paths and logging.** exiv2 0.28 takes a narrow path. On POSIX that is the
path's own bytes, so any filename works; on Windows it is UTF-8, which
exiv2 widens (the library's documented 0.28 behaviour; not run on Windows
yet). exiv2's own log is muted once: it writes warnings to stderr by default,
and a library must not. Its warnings are not routed to the `DiagnosticLog`,
because they are global rather than per call and a real camera's maker note
triggers them often.

**Thread safety.** `readExif` may be called from several threads: exiv2's
tables are constant, an image object belongs to one call, and the one shared
mutable thing, XMP's namespace registry, is given a mutex by initialising the
XMP parser once with a lock function (a function-local static, which C++
initialises thread-safely). The thumbnail worker and the strip's tooltips can
therefore both read. A test reads from eight threads at once.

**What export metadata will reuse.** Writing descriptive metadata into an
exported file goes through the same exiv2 dependency, the same path handling
and the same initialisation. The struct is a read model; what is written is a
separate decision (see the export metadata notes) and may reuse the field
names so that "copy the camera's EXIF" is a field-by-field copy.

## Consequences

- `arraw-cli info` shows `camera`, `lens`, `exposure`, `exposure bias`,
  `flash`, `taken`, `GPS`, `artist` and `copyright` lines for the fields a
  file records, and `--json` an `exif` object with the camelCase keys of the
  fields present, rationals as `{"numerator", "denominator"}`.
- Python has `read_exif(path)`, `ExifInfo`, `URational`, `SRational` and
  `GpsPosition`, frozen like the settings classes.
- The fixtures built by `make_raw_fixtures.py` were synthetic and carried no
  capture EXIF, so `exif-32x24.dng` was added: a RAW with Make, Model and
  EXIF and GPS IFDs. Its values are what the tests pin. The southern and
  western signs are tested on a JPEG written in the test with exiv2.
- Lens correction (ADR 005) can build on `make`, `model`, `lensModel`,
  `focalLength`, `fNumber` when it arrives.
