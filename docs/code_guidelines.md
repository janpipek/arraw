# C++ Code Guidelines

Active for all source files in the project.

> **Domain vocabulary is not repeated here.** [CONTEXT.md](../CONTEXT.md) is the
> single source for every preferred term and the words to avoid. Use it for
> identifiers, comments, UI strings, and commit messages alike.

## Naming

* **Strictly avoid Hungarian notation**: no type prefixes, no `m_` prefixes.
* When editing a file, **strip any existing `m_` prefixes** from names in it and do
  not introduce new ones. The legacy code still carries some; each edit shrinks it.
* Class members, local variables, and parameters use clean, plain names — `zoom`,
  `params`, `viewport`, `curveLutTex`.

## Language

* Modern C++20.
* Prefer `const` by default.
* Use `auto` where the type is obvious from context.
* Declare one pointer or reference variable per statement, to avoid ambiguous
  mixed declarators.

## Formatting

`clang-format` (18+ preferred) owns mechanical formatting; `just format` applies it
and `just format-check` verifies. What the config enforces, recorded here so the
intent survives a config change:

* Same-line opening braces for function, class, and struct definitions.
* A single empty line between function definitions.
* `*` and `&` attach to the type: `QWidget* parent`, `const ImageBuffer& buffer`.
* Wrapped constructor initializer lists get one initializer per line, with
  trailing commas.

In headers, keep consecutive plain function declarations compact. Add a single
empty line before a documented declaration or between inline definitions;
`clang-format` preserves one intentional empty line and enforces definition-block
separation, while docstring grouping remains a human style rule.

## Scope discipline

Never reformat, re-indent, or reorder includes in files unrelated to your task.
Limit `just format` to the files you actually edited. A cleanup-only reformat
belongs in its own commit, not bundled into a feature change.

## Layering

Includes are layer-qualified from the `src/` root (`#include
"develop/GlobalAdjustment.h"`), so a file's include block is its dependency
manifest and a downward violation is greppable. The layer table is in
[DESIGN.md § Source Layout](../DESIGN.md#source-layout)
([ADR 0041](adr/0041-layered-source-directories.md)).
