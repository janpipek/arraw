Arraw is a multi-platform RAW processing engine and app.

Goals:

- support on Linux (primary), Windows (secondary), MacOS (tertiary)
- a library to support the operations, CLI app to
- (eventually) Python or Lua scripting based on the library (and perhaps to drive the GUI app?)

Setting:

- This branch is a major rework of the "main" branch. You are allowed to look at the files there but you should not
  follow blindly, "clean slate" is an asset (see important files)

Important files (all in /docs):

- desired-features.md - a description of the features we want to support from the photographers perspective
- ideas/reimplementation-plan.md - some guidance on how we rebuild the app, differing from main

Code style:

- Idiomatic C++20
- .h, .cpp files semantically organised
- `clang-format` owns mechanical C++ formatting. Run `just format` only on files
  changed for the current task; use `just format-check` to verify formatting without changes.
