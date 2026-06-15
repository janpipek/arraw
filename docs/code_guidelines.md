# C++ Development & Terminology Guidelines

This rule is active for all source files in the project.

## Code Style Rules
* Use Modern C++20.
* **Strictly avoid Hungarian notation**: No type prefixes or `m_` prefixes.
* When editing/modifying a file, **strip any existing `m_` prefixes** from variables in that file and do not introduce new ones.
* Class members, local variables, and parameters should use clean, plain names (e.g., `zoom`, `params`, `viewport`).
* Prefer `const` by default and use `auto` when the type is obvious from context.
* `clang-format` enforces same-line opening braces for function, class, and struct definitions.
* `clang-format` should keep function definitions separated by a single empty line.
* `clang-format` attaches `*` and `&` to the type (`QWidget* parent`, `const ImageBuffer& buffer`).
* Declare one pointer or reference variable per statement to avoid ambiguous mixed declarators.

## Domain Vocabulary Constraints
Avoid legacy or incorrect domain terms and prefer the following official project terms:

* **Culling**: Avoid "cataloguing", "library", "DAM".
* **Rating**: Avoid "stars", "pick/reject flag". Reject is rating `-1`.
* **Colour Label**: Avoid "tag", "keyword", "free-text label". Use canonical English names (Red, Yellow, Green, Blue, Purple) or none.
* **User Metadata**: Avoid "image metadata" (which refers to read-only EXIF data) or "catalog data".
* **Working color space**: Avoid "internal color space", "pipeline color space". (Linear-light Rec.2020).
* **Display transform**: Avoid "gamma step", "sRGB step".
* **Output transform**: Avoid "export color conversion".
* **Soft-proofing**: Avoid "print preview", "proof mode".
* **Clipping**: Avoid "extremes", "blown/crushed".
* **Clipping Overlay**: Avoid "show extremes", "zebra", "clipping mask".
* **Gamut Warning**: Avoid "clipping", "out-of-range".
* **Tone Curve**: Avoid "curves".
* **Luma Curve**: Avoid "master curve", "RGB curve".
* **Local Adjustment**: Avoid "filter", "layer", "adjustment brush".
* **Mask**: Avoid "selection", "channel", "cut-out".
* **Develop Preset**: Avoid "profile", "template", "style".
* **Straighten**: Avoid "deskew", "level", "auto-rotate".
