# One engine library, tested through CTest, built by CI

The front ends listed each engine source file themselves, so every consumer
recompiled the same code and nothing prevented the CLI from acquiring a Widgets
dependency. There was no test target and no continuous integration, while the
reimplementation plan rests on a second rendering backend held to the first by
comparison tests that only a machine running them can keep honest.

## Decision

`arraw` is a static library built from `src/core`, with `include/` as its public
interface directory. `arraw-ui`, `arraw-cli`, and the tests link that target and
inherit the headers from it. The library keeps AUTOMOC off: the engine has no Qt
types, and that stops being true loudly rather than silently.

Tests use Catch2 v3, taken from a system installation when one is present and
built from a pinned source revision otherwise. Each `TEST_CASE` is registered
individually with CTest.

GitHub Actions builds every target and runs the suite on Linux for each push and
pull request, installing Qt from a published distribution because the runner
image ships a version below the project's minimum. Formatting is not checked
there: `clang-format` output varies between major versions, so a runner several
versions behind the developer machine would reject correctly formatted code.

## Consequences

- A CPU-only consumer can link `arraw` without Qt Widgets.
- Adding a photographic module means one library, not an edit per executable.
- A contributor without Catch2 installed can still configure and run the suite.
- The formatting gate stays local, in `just format-check`, until the toolchain is
  pinned.
