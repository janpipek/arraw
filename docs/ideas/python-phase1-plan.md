# Python bindings, phase 1 — execution plan

Status: implemented on branch `gpu-develop` (89f19e9, 6079101, 3cb47c6 and
aed395f). Where the result differs from the plan, see
[Outcome](#outcome) at the end. The decisions that last are recorded in
[ADR 018](../adr/018-python-binds-the-public-api-and-nothing-else.md).

## Goal

`import arraw` in a local build can open a photograph, decode it, develop it on
the CPU, hand the pixels to NumPy without a copy, and save the result. That is
exactly what `arraw-cli export` does, so the bindings add no photographic
behaviour of their own.

Not in scope: GPU, wheels / PyPI, XMP sidecars, `RenderRequest::targetSize`
(not implemented in C++ yet), `EditSession`, building an `ImageBuffer` from
NumPy, scripting the running GUI, Lua (not wanted).

## Decisions

| Question | Answer |
|---|---|
| Binding library | nanobind (not the reimplementation plan's pybind11): smaller, `nb::ndarray` built in, stable ABI available later |
| Build | One CMake. `ARRAW_BUILD_PYTHON` in the normal tree for `just py-test`, and `pyproject.toml` with scikit-build-core so `uv pip install -e .` works too |
| Settings | Nested mirror of the C++ structs, plus flat keywords on `Photo.with_()` driven by the ADR 008 descriptor table, built first (below) |
| Pixels | Explicit `photo.load()` + `develop(buffer, settings)`, and a `develop(photo)` shortcut that decodes on each call |
| Qt | The module links Qt as the core does. Making the core Qt-free is not planned |
| Scope | Local build only, no wheels. No Lua, so no C API |
| Python | uv-managed CPython (`python-preference = "only-managed"`): the system 3.14 has no development headers |

## Step 0: the descriptor table (ADR 008)

The Python keywords come from the table, so the table comes first. The command
line moves onto it in the same change, which removes its hard-coded ranges.

ADR 008 sketched a flat `DevelopSettings` reached through `float
DevelopSettings::*`. The settings have since become nested (`tone`, `color`,
`geometry.crop`), so a row reaches its field through a captureless lambda
turned into a function pointer instead:

```cpp
// include/SettingDescriptors.h
using SettingAccessor = std::variant<
    float& (*)(DevelopSettings&), std::optional<float>& (*)(DevelopSettings&),
    double& (*)(DevelopSettings&), bool& (*)(DevelopSettings&),
    WhiteBalanceMode& (*)(DevelopSettings&), QuarterTurn& (*)(DevelopSettings&),
    std::optional<UprightCropRect>& (*)(DevelopSettings&), CropAspect& (*)(DevelopSettings&)>;

struct FieldDescriptor {
    std::string_view key;               ///< camelCase leaf name, unique: "exposure", "cropRectangle"
    SettingAccessor member;
    std::optional<SettingRange> range;  ///< absent for bool, enum and compound rows
    SettingGroup group;                 ///< Color, Tone, Geometry
    Applicability applies;              ///< Always, or RawOnly (temperature, tint)
    Stage affects;                      ///< the existing RenderCheckpoint Stage: Pointwise or Geometry
};
inline constexpr std::array developSettingDescriptors{ /* one row per leaf */ };
```

- **Rows** cover every leaf: the 7 tone settings; `whiteBalance`,
  `temperature` and `tint`; `rotation`, `flipHorizontal`, `flipVertical`,
  `straighten`, `cropRectangle` and `cropAspect`. The crop rows have no range,
  because their constraints stay in GeometryPlan.
- **Ranges** come from the existing `darkestExposure` … `maximumStraighten`
  constants. The table replaces the scattered constants where nothing else
  uses them.
- **Reading through a const `DevelopSettings`** goes through one documented
  helper, which is the only place a `const_cast` is allowed.
- **`validate(const DevelopSettings&)`** throws `std::invalid_argument` for a
  non-finite value or one out of range, naming the key, the value and the
  range. `Photo`'s constructor calls it, so an invalid photograph cannot exist.
  The renderer's own clamping stays (ADR 008).
- **Drift guard:** there is no reflection on GCC 15, MSVC or AppleClang. A
  C++20 aggregate field-count trick in `tests/support` asserts the number of
  fields in each settings struct, so adding a field without a row fails the
  tests. A test also writes a sentinel through each row and checks that
  exactly one field changed.
- **CLI:** option names are the key in kebab-case, and ranges come from the
  table in both parsing and `--help`. The human-readable help text stays in
  the CLI. The compound options (`--rotate`, which sets rotation and
  straighten, `--crop` and `--crop-aspect`) keep their own parsing but take
  their limits from the table. Existing CLI tests pass unchanged.
- **ADR 008** gets a dated note describing the nested shape and the accessor
  pointers.

## API

```python
import arraw

photo = arraw.open("DSC_0042.NEF")          # Photo, reads no pixels
photo.path, photo.metadata.size, photo.metadata.orientation
photo.settings.tone.exposure                  # 0.0

brighter = photo.with_(exposure=0.7, white_balance=arraw.WhiteBalanceMode.CUSTOM,
                       temperature=5200, tint=8)
brighter = photo.with_(settings=some_develop_settings)   # whole replacement

source = photo.load()                         # ImageBuffer, decoded once
out    = arraw.develop(source, brighter.settings)
out    = arraw.develop(brighter)              # shortcut: load + develop

arr = out.pixels                              # numpy view, (h, w, c), dtype by format; no copy
arraw.save(out, "out.jpg", quality=92)        # ExportOptions as keywords
```

- **Settings objects are frozen.** They have keyword constructors, read-only
  attributes, `replace(**kw)`, `==` and `repr`. A mutable nested attribute
  would silently edit a copy (`photo.settings.tone.exposure = 1` would do
  nothing to `photo`), which is the trap freezing avoids.
- **Flat keys** are the descriptor keys in snake_case (`filmic_highlights`,
  `crop_rectangle`, …), converted mechanically, with nothing written out by
  hand. An unknown key raises `TypeError`. An out-of-range value raises
  `ValueError` from `validate`. `arraw.setting_descriptors()` exposes the
  table's key, range, group, applicability and stage.
- **Enums** are `WhiteBalanceMode`, `QuarterTurn`, `PixelFormat`,
  `ImageOrientation`, `NamedEncoding`, `ImageFileFormat` and `Severity`, with
  UPPER_SNAKE members.
- **The crop aspect** variant is three classes: `FreeCropAspect`,
  `OriginalCropAspect` and `CropRatio(width_over_height)`.
- **`ImageBuffer`** is read-only apart from its pixel view. It exposes `size`,
  `format`, `encoding`, `orientation` and `pixels`, and the view keeps the
  buffer alive.
- **Encoding** is either a `NamedEncoding` or an opaque `CameraNative` with a
  `repr`. Phase 1 does not expose its matrices.
- **Other functions:** `arraw.load(path)` → ImageBuffer and
  `arraw.read_metadata(path)`, the free-function forms of the C++ API.
- **Errors** use nanobind's defaults: `invalid_argument` → `ValueError`,
  `runtime_error` → `RuntimeError`.
- **Diagnostics** go to `logging.getLogger("arraw")` as
  `describe(diagnostic)`: Info → INFO, Warning → WARNING, Error → ERROR. The
  C++ log re-acquires the GIL before calling into Python.
- **The GIL** is released in `load`, `develop` and `save`.

## Layout

```
src/python/Module.cpp         NB_MODULE(_arraw): the bindings, split by topic if it grows
src/python/arraw/__init__.py  re-exports from ._arraw; logging bridge
src/python/arraw/py.typed
tests/python/test_*.py        pytest over tests/fixtures
pyproject.toml                scikit-build-core, dependency group dev = pytest numpy nanobind
```

## Build

- **CMake:** `option(ARRAW_BUILD_PYTHON OFF)` and `option(ARRAW_BUILD_APPS ON)`.
  - `find_package(Python 3.12 COMPONENTS Interpreter Development.Module)`
  - nanobind from `python -m nanobind --cmake_dir`
  - `nanobind_add_module(_arraw NB_STATIC ...)` linking `arraw` only (not
    `arraw-gpu`)
  - `POSITION_INDEPENDENT_CODE ON` on `arraw`
  - `install(TARGETS _arraw DESTINATION arraw)`
- **In the dev tree,** the module and the package's `.py` files land in
  `build/<prefix>debug/python/arraw/`.
- **scikit-build-core** sets `ARRAW_BUILD_PYTHON=ON`, `ARRAW_BUILD_APPS=OFF` and
  `ARRAW_BUILD_TESTS=OFF`. Its build dir stays under `build/`, and the version
  comes from `project(VERSION)`.
- **`just py-test`** runs `uv sync --no-install-project`, configures the debug
  preset with `-DARRAW_BUILD_PYTHON=ON -DPython_EXECUTABLE=.venv/bin/python`,
  builds `_arraw`, then runs `uv run --no-sync pytest tests/python` with
  `PYTHONPATH` set to the build's `python/`.
- **Qt:** the codecs may need a `QCoreApplication`. Check first whether
  QImageReader finds the JPEG/TIFF plugins without one (plugin paths come from
  QLibraryInfo). Only if it doesn't, create one lazily, on the first call that
  needs it and only if `QCoreApplication::instance()` is null, so a PySide host
  keeps its own instance.

## Tests (pytest)

- `open` on a DNG and a PNG fixture gives the size, orientation and encoding
  kind that the C++ tests expect.
- `with_`: flat keys land in the right nested field. An unknown key raises
  `TypeError`. The original `Photo` is unchanged, and `==` holds.
- `develop(photo)` equals `develop(photo.load(), photo.settings)` element for
  element.
- `pixels`: shape `(h, w, 4)`, `float32` for a developed buffer. The view
  survives `del` of the buffer.
- `save` writes a readable JPEG/PNG/TIFF and rejects `bit_depth=16` for JPEG
  with `ValueError`.
- A fixture that substitutes white balance logs a WARNING on `arraw`.
- Pixel values match the C++ CLI's output: develop in Python, export, and
  compare with `arraw-cli export` on the same fixture and settings.

## Execution

Small workflows, with Sonnet implementing and Opus reviewing, run in order with
a review after each:

1. Descriptor table, `validate`, drift-guard tests, ADR note.
2. CLI onto the table.
3. Python build plumbing plus a module that imports.
4. Bindings and the Python package.
5. pytest suite and `just py-test`.

## Outcome

These notes record where the implementation differs from the plan above, or
settles something the plan left open.

- **Step order:** the CLI moved onto the table together with the table
  itself, so steps 1 and 2 landed as one commit. The table's types are
  `SettingAccessor` and `SettingRange`, not `Member` and `Range`, which were
  too generic for the public namespace.
- **No `QCoreApplication`:** none is needed, so the module creates none. Qt
  finds its JPEG and TIFF plugins through QLibraryInfo, and loading and
  saving both work without an application instance.
- **Build trees:** `just py-test` builds in its own `py-debug` tree (Python on,
  apps and tests off), so the everyday debug tree never needs `.venv`. It also
  builds `arraw-cli` in the debug tree, because the parity tests compare
  against it and fail rather than skip if it is missing (`ARRAW_REQUIRE_CLI`).
  Running `uv sync` without `--no-install-project` installs arraw editable
  through scikit-build-core instead.
- **Settings:**
  - Constructors are keyword-only, except `CropRatio` and `UprightCropRect`.
  - `DevelopSettings.with_` exists alongside `Photo.with_`. Only `Photo`
    validates, as in C++.
  - Values are stored as float32, so they read back as, for example,
    `0.699999988`.
- **Types are strict:** a bool is refused for a number and an int for an
  enumeration. An int is accepted for a number.
- **Diagnostics:**
  - `open`, `read_metadata` and `load(path)` report to the `arraw` logger.
  - `Photo.load()` and `develop(photo)` do not report again, since `open`
    already said what the file declares. The CLI does the same.
- **Missing RAW file:** a RAW that does not exist is now reported as "No such
  file or directory" instead of LibRaw's "Image too big for processing". This
  is a core fix in `RawImport.cpp`.
- **Stubs:** `_arraw.pyi` is generated by nanobind's stubgen (target
  `arraw-stubs`) and committed. A pattern file keeps `with_` and
  `__version__`, which stubgen otherwise treats as private.
- **`open`** is importable but left out of `__all__`, so `from arraw import *`
  does not shadow the builtin.
