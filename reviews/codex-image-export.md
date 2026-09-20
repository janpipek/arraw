# Image export review

I’d fix these before relying on the exporter:

1. **[P1] Failed export destroys an existing file** — [ImageExport.cpp:95](../src/core/ImageExport.cpp#L95). Opening with `WriteOnly` truncates the destination before validation or encoding. I reproduced an existing file becoming empty when exporting `RgbU16`. Use [`QSaveFile`](https://doc.qt.io/qt-6/qsavefile.html) and commit only after successful encoding, or write to a temporary file and rename.

2. **[P1] File-open failure terminates the application** — [ImageExport.cpp:96](../src/core/ImageExport.cpp#L96). A missing parent directory calls `exit(-127)`, killing the library’s host and bypassing stack unwinding. Throw an exception containing the path and `outputFile.errorString()`.

3. **[P1] Encoding failures silently succeed** — [ImageExport.cpp:100](../src/core/ImageExport.cpp#L100). [`QImageWriter::write()`](https://doc.qt.io/qt-6/qimagewriter.html#write) returns failure, which is discarded. The mappings at lines 77–79 already trigger this for valid `RgbU16` and `RgbF32` buffers: both produced empty files while returning normally. Convert these layouts or explicitly reject them before opening the destination; check every write and report `writer.errorString()`.

4. **[P2] Most export options have no effect** — [ImageExport.cpp:93](../src/core/ImageExport.cpp#L93). `quality`, `bitDepth`, `encoding`, and `embedProfile` are ignored, as is the source colour encoding. Probes confirmed identical JPEG bytes at qualities 0 and 100, 16-bit PNG output despite the default `bitDepth = 8`, and no embedded profile despite `embedProfile = true`. Implement these options or narrow the API until supported.

5. **[P2] Tests verify signatures, not image contents** — [test_ImageExport.cpp:45](../tests/test_ImageExport.cpp#L45). Incorrect pixels, dimensions, colour profiles, and bit depth all escape detection. Add an odd-width PNG round-trip with pixel comparison, coverage for TIFF and supported sample layouts, and failure cases checking exception reporting and preservation of an existing destination.

For nicer C++, I’d keep the current small functions and make a few changes:

- Put all implementation helpers in an unnamed namespace; three currently have external linkage.
- Remove the unused `using namespace std;` and include `<stdexcept>` directly.
- Use `std::array` for fixed signatures and `std::span<const std::uint8_t>` for the comparison helper, avoiding the extra signature vector.
- Document `exportImage`’s supported inputs and exception contract.

The unsigned-character handling in `tolower`, exhaustive switches, and borrowed-buffer lifetime are sound.

**Validation:** all 12 tests pass; `just format-check` fails in [ImageExport.h](../include/ImageExport.h). No source changes made during the review.
