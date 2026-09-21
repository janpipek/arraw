# Develop settings are plain values with a descriptor table beside them

This ADR is written ahead of the code it describes. `DevelopSettings` is an
empty struct today and nothing consumes it.

Five callers need to do more with a develop setting than read it: the command
line parses one by name, Python takes it as a keyword argument, the sidecar
reads and writes it, presets carry a *partial* bundle of them, and the GUI
draws a control for it. Each needs the setting's key, type, range, default,
group, and whether it applies to this photograph at all. C++20 has no
reflection, so that information has to be written somewhere.

The previous implementation put part of it in `FieldSpec`, which defines
numerical behaviour partly in integer slider ticks — the reimplementation plan
names that as a thing to separate: photographic values are the model, and
slider ranges, display precision and localised text are presentation derived
from it.

## Decision

**Settings are plain values in an aggregate**, and a `constexpr` table beside
it describes them:

```cpp
struct DevelopSettings {
    float exposure = 0.0F; ///< In EV.
    float temperature = 0.0F;
    // ...
};

struct FieldDescriptor {
    std::string_view key;
    Member member;      ///< Variant over `float DevelopSettings::*` and friends.
    Range range;
    Group group;
    Applicability applies;
    Stage affects;
};

inline constexpr std::array descriptors{
    FieldDescriptor{"Exposure2012", &DevelopSettings::exposure, {-5, 5},
                    Group::Tone, Applicability::Always, Stage::Pixel},
    // ...
};
```

Member pointers keep the table type-checked by the compiler, and the fields
stay ordinary floats — addressable, trivially copyable, and laid out the way a
GPU uniform block will want them.

**The alternatives, and why not.** Self-describing fields
(`Setting<float, "Exposure2012", Range{-5, 5}>`, using C++20 class-type
template parameters) make drift impossible but stop `settings.exposure` being a
float, and still need something to enumerate the members. Generating the struct
and the table from one list removes the duplication entirely and is the likely
answer once GPU uniform layouts must be verified mechanically — it is worth
revisiting then, not now. `Q_GADGET` would give runtime names and generic
access for free, since Qt is already linked into the core, but it puts Qt macros
in the public headers, is stringly typed, and still needs a side table for
ranges and groups.

**C++26 reflection is used as a drift guard, not as the mechanism.** Measured on
this machine, GCC 16.2 with `-std=c++26 -freflection` enumerates an aggregate's
members, names and all. MSVC and AppleClang do not, and Windows and macOS are
supported targets, so the table stays. Behind a feature test, reflection
supplies `static_assert(fieldCount<DevelopSettings>() == descriptors.size())`,
which fails the Linux build when a field is added without a row. The table is
deleted outright when the other compilers catch up.

**The first slice is Exposure and White Balance.** Exposure proves the plumbing
end to end; white balance forces the question of where a setting acts, which
ADR 007 answers.

**Temperature is anchored to the camera, and non-RAW images get a different
setting.** For a RAW, `temperature` is the illuminant's correlated colour
temperature in Kelvin, converted to camera multipliers through
`CameraNative::toWorking`, and `asShotNeutral` inverts to the value shown when
the file opens — so the slider reads the light the camera saw, not a dial
centred on an arbitrary 5500 K. A JPEG or TIFF has no sensor to anchor to, so
it carries `incrementalTemperature` and `incrementalTint` on a -100 to 100
scale instead.

These are two settings, not one setting with two meanings. Each carries its own
range, default and applicability, the panel shows whichever applies, and the
mapping is one-to-one onto `crs:Temperature` / `crs:Tint` and
`crs:IncrementalTemperature` / `crs:IncrementalTint` — Lightroom's own model, so
the sidecar needs no cleverness and a stored value means something without the
photograph beside it.

**White balance therefore does not cross the RAW/non-RAW boundary.** Copy and
paste, and presets, carry the setting that applies; the checklist reports white
balance as inapplicable rather than inventing an equivalent.

**A value outside its range is rejected, clamped, and clamped again.**
Setting one through the library, command line, Python or the GUI is an error:
nothing invalid enters a session, and the person who typed it is present to be
told. A value read from a sidecar or a preset is clamped to the modelled range
and warned about, on the same load-warning channel as the substituted as-shot
illuminant above. The processing contract clamps as well, redundantly, so that
no pixel maths depends on the reader having done it.

That redundancy is the point. Clamping on read is only tenable while arraw
writes no sidecar that Lightroom reads: the moment it does, clamping a foreign
30000 K down to 12000 and writing it back destroys someone's edit, and the
policy must become retain-on-read with the clamp left to the renderer. Because
the renderer already clamps, that change is one deleted call in the read path
and no change to any maths.

**Develop settings are written in `arraw:`, and `crs:` is neither read nor
written yet.** Reading Adobe's develop properties without writing them back is
worse than ignoring them: the file then holds two namespaces that disagree about
the same photograph, with Lightroom still showing its own. One truth per file is
the rule, and the `crs:`-aligned semantics above mean that enabling interop
later is a mapping layer rather than a re-tuning of every setting.

Two carve-outs. Genuinely standard XMP is not ours to rename — `xmp:Rating`,
`xmp:Label` and `dc:` metadata are read and written in their own namespaces from
the start, being culling marks and metadata rather than develop settings. And
XMP we do not understand is never deleted: a foreign `crs:` block survives a
write untouched, which is what keeps this decision reversible instead of
quietly erasing somebody's Lightroom edits in the meantime.

## Consequences

- **Adding a setting is two edits** — the field and its row — and the missing
  row fails the build on Linux.
- **An absolute Kelvin transfers correctly across bodies.** Pasting 5500 K
  across a shoot shot on two cameras lands on the same colour, which a gain
  measured in the working space cannot do.
- **A file that declares no as-shot neutral will show a confident Kelvin for an
  illuminant it never recorded.** LibRaw substitutes daylight multipliers in
  that case, and `RawImport.cpp` already notes that this "becomes a warning on
  the import path as soon as there is somewhere to put one". Under this
  decision the substitution is visible on a slider, so that warning channel is
  required rather than optional.
- **The adjustment panel changes units per photograph**, showing Kelvin and
  Tint for a RAW and two -100 to 100 sliders otherwise. Lightroom behaves the
  same way; it still has to be built deliberately.
- **Presentation is derived, not stored.** Slider tick counts, decimal places
  and localised names belong to the GUI, which reads the descriptor.

Two conditions require this decision to be revisited, and neither is a defect
when it arrives:

- **arraw begins writing sidecars Lightroom reads.** Clamping on read becomes
  destructive and must become retain-on-read.
- **arraw widens a range in a later version.** A file written by the newer one
  is clamped by the older, without Lightroom being involved at all. That is the
  processing-compatibility policy the reimplementation plan lists as open, and
  clamp-with-a-warning is a defensible default for it rather than an answer.

