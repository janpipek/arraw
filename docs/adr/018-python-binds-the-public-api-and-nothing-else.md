# Python binds the public API, and nothing else

The reimplementation plan makes standalone Python scripts, notebooks and batch
processing a first-class use, and names "the Python packaging and lifecycle" as
a decision to record. The
[phase 1 plan](../ideas/python-phase1-plan.md) built the first module. This ADR
records what that work decided, so that phase 2 (sidecars, batch) and phase 3
(GPU) start from the decisions rather than from reading the code.

## Decision

**The module binds `include/` and never `src/`.** `_arraw` links `arraw` and
nothing else: not `arraw-gpu`, not the CLI. Python is therefore an honest test
of the public API. A Python feature that needs a private header means the C++
API is missing something, and the fix belongs there. Photographic behaviour is
never written in the bindings, which is what makes pixel parity with
`arraw-cli export` a test and not a hope (`tests/python/test_cli_parity.py`
requires the pixels to be identical).

**nanobind, not pybind11.** The reimplementation plan named pybind11. nanobind
serves the same purpose with smaller binaries and faster calls. Its
`nb::ndarray` gives NumPy views without a copy, and it has a stable-ABI mode for
when wheels matter. The bindings live in `src/python/` and are split by topic
(`BindImage`, `BindSettings`, `BindPhoto`). The package's Python side,
`src/python/arraw/`, only re-exports.

**Python follows C++ values one for one, with Python spelling.** `Photo`,
`DevelopSettings` and its parts, `ImageBuffer` and the enumerations keep their
C++ names and shapes. Attributes are snake_case and enumeration members are
UPPER_SNAKE. A function takes the same arguments as the C++ call it wraps, and
defaults such as `save`'s are read from the C++ `ExportOptions{}`, not copied.

**Settings are frozen.** Settings objects have keyword-only constructors,
read-only attributes, `replace()`, `==` and hashing. A mutable nested attribute
would let `photo.settings.tone.exposure = 1` edit a copy and change nothing,
which is the worst kind of silent failure in a script. Keyword-only also means a
reordered C++ struct cannot change what existing Python code means.

**Flat keywords come from the descriptor table ([ADR 008](008-develop-settings-and-their-descriptors.md)).**
`photo.with_(exposure=0.7, flip_horizontal=True)` takes the snake_case form of
each descriptor key, converts the value by visiting the row's accessor, and
builds a new `Photo`. Nothing in the bindings lists the keys. A new row is a new
keyword with no Python change, and `setting_descriptors()` exposes the table
for tools that want ranges or groups. Values are type-checked strictly: a bool
is not a number and an int is not an enumeration member. An out-of-range value
is refused by `validate()` in `Photo`'s constructor, and so raises
`ValueError`.

**Pixels are loaded explicitly, with one shortcut.** `photo.load()` decodes and
`develop(buffer, settings)` renders, so a loop that develops one photograph many
ways decodes it once. `develop(photo)` decodes on every call, for one-off
scripts. A `Photo` never caches pixels, because it is a cheap value
([ADR 001](001-photo-state-and-image-buffer-ownership.md)).

**Buffers are shared with NumPy, not copied.** `ImageBuffer.pixels` is a
writable NumPy view with shape `(height, width, channels)`, a dtype that follows
the pixel format, and strides from the row stride. The view keeps the buffer
alive. Phase 1 cannot build an `ImageBuffer` from an array; that belongs with a
use that needs it.

**Heavy calls release the GIL.** `open`, `load`, `develop` and `save` release
it, so Python threads can run a batch in parallel. The diagnostics log is the
only code path that touches Python from a released section. It re-acquires the
GIL, and it swallows its own failures, because core code does not expect a log
to throw.

**Diagnostics go to `logging`, once per file.** Each diagnostic goes to
`logging.getLogger("arraw")` as `describe(diagnostic)`, at the level of its
severity. `warnings` is not used, because a diagnostic is a fact about a file,
not a deprecation-style warning about the caller's code. A `Photo` has already
reported what its file declares when it was opened, so `Photo.load()` and
`develop(photo)` do not report it again, as the command line does not.

**Exceptions use nanobind's defaults.** `std::invalid_argument` becomes
`ValueError` and `std::runtime_error` becomes `RuntimeError`. There is no
`arraw.Error` hierarchy until a caller needs to tell arraw's failures apart from
others.

**No `QCoreApplication`.** On the Qt this was measured with (6.10, Linux),
QImageReader and QImageWriter find the JPEG and TIFF plugins through
QLibraryInfo without an application instance. The module creates none, so it
cannot collide with a PySide host's own. If a platform turns out to need one,
the module creates it lazily, and only when `QCoreApplication::instance()` is
null.

**Qt comes along.** The core links Qt for its codecs, so the module does too. A
Qt-free core, with non-Qt codecs, is not planned. The cost is size and a
possible second Qt in a PySide process. That cost matters for wheels, not for
the local builds this ADR covers.

**One CMake, two ways in.** Both routes use the same CMake:
- `just py-test` builds `_arraw` in its own `py-debug` tree (Python on, apps
  and tests off) against the uv-managed `.venv`, then runs pytest. It also
  builds `arraw-cli`, because the parity tests fail rather than skip without
  it.
- `uv sync` installs arraw editable through scikit-build-core.

Python is uv-managed (`python-preference = "only-managed"`), because a
distribution's Python may lack development headers. `uv.lock` is committed.
`ARRAW_BUILD_APPS` lets a Python-only build skip the applications.

**Stubs are generated and committed.** `nanobind_add_stub` writes
`arraw/_arraw.pyi`, and the package ships `py.typed`. A pattern file keeps
`with_` and `__version__`, which stubgen otherwise treats as private. A test
checks that they are present.

## Alternatives

- **A C API with cffi or ctypes.** This would serve other languages too, but Lua
  was ruled out and nothing else is asking. It would duplicate every value type
  as a C struct and give up NumPy views.
- **pybind11.** It is familiar, but nanobind is better at both things we care
  about: array views and the stable ABI.
- **A hand-written map from keywords to fields.** It is quicker, but it is the
  duplication ADR 008 exists to prevent. The descriptor table was built first
  so that Python, the command line, and later sidecars and the GUI all read
  one list.
- **Mutable settings mirroring the C++ aggregates.** This is what a C++
  programmer expects. In Python, assigning to a nested attribute through a
  property edits a copy, so it looks like it works and doesn't.

## Consequences

- **Adding a develop setting is still two edits** (the field and its row). The
  new setting reaches Python, the command line and `setting_descriptors()`
  with no further change.
- **Settings read back at float32 precision**, for example
  `with_(exposure=0.7)` gives `0.699999988`. The repr prints the shortest text
  that reads back the same. Tests need `pytest.approx`.
- **Every Python behaviour has a C++ owner.** A bug report against Python is a
  bug in the core or in a thin conversion, never in photographic logic written
  twice.
- **`open` is importable but not in `__all__`,** so `from arraw import *` does
  not shadow the builtin.

These conditions require this decision to be revisited, and none of them is a
defect when it arrives:

- **Wheels or PyPI.** Bundling Qt and LibRaw, choosing the stable ABI, and
  possibly making the core Qt-free all become real questions.
- **A GUI with a Python console.** Scripting an open window needs a lifecycle
  story (whose event loop, whose `QCoreApplication`) that this ADR avoids by
  having no application instance.
- **GPU from Python.** The device lives behind a Qt platform. A Python process
  will need the headless platform the command line uses, or an explicit
  choice, and the GIL rules for a device that belongs to one thread.
