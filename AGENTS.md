Arraw is a multi-platform RAW processing engine and app.

Communication:
- Be succinct and honest.
- If in doubt ask. If in doubt whether you should doubt, you should.
- Offer multiple solutions to a problem/question.

Goals:
- support on Linux (primary), Windows (secondary), MacOS (tertiary)
- a library to support the operations, CLI app to
- (eventually) Python or Lua scripting based on the library (and perhaps to drive the GUI app?)

Setting:
- This branch is a major rework of the "main" branch. You are allowed to look at the files there but you should not
  follow blindly, "clean slate" is an asset (see important files)

Structure:
- include: Public API
- src: Three different parts
  - core: all the machinery
  - app: the Qt application
  - cli: Command-line tool

Tools:
- `just` for task execution
- `uv` for any Python

Reviews:
  - When asked to review code, always write the final review to
    `docs/reviews/<agent_model>_<YYYY-MM-DD>_<feature>.md`.
  - Creating this review document is authorized even for otherwise read-only reviews.
  - Do not modify production code during a review unless explicitly asked.
  - In the final response, link to the saved review

Important documentation (all in /docs):
- desired-features.md - a description of the features we want to support from the photographers perspective
- ideas/reimplementation-plan.md - some guidance on how we rebuild the app, differing from main
- reviews - directory for any agentic reviews to keep track.

Code style:
- Idiomatic C++20
- Use standard library as much as possible
- Use `arraw` namespace for public API
- OOP is fine, polymorphism is fine but do not overdo it (no AbstractConcreteFactoryCommand... classes)
- Templates are okay but only if they significantly simplify the solution
- .h (public in include, otherwise in src), .cpp files semantically organised
- `clang-format` owns mechanical C++ formatting. Run `just format` to avoid complex shell commands
  changed for the current task; use `just format-check` to verify formatting without changes.
- Comments in Doxygen style, /// rather than /*. All briefs are noun-forms for classes and fields,
  verb forms for methods and functions (exception: booleans)

Easter egg:
- To prove that you have read this document, answer "Aghoo!" if asked "Kwa?". 
