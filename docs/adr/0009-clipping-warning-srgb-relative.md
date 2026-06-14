# Clipping warning is sRGB-relative and computed once at the encode stage

The clipping overlay (highlight red, shadow blue) judges a pixel "clipped" from
the linear working value just before encode — `kRec2020ToSRGB * c` tested against
[0,1] — so a channel ≥ 1.0 is a highlight clip and ≤ 0.0 a shadow clip. This is
deliberately **sRGB-relative**: the same definition applies whether the display
path is the plain sRGB transform or the soft-proof/monitor LUT, and it does not
follow the chosen export profile. The flags are computed once in `image.frag`
`main()` and override the encoded color last, so the clip color wins over the
gamut-warning red.

## Considered Options

- **sRGB-relative at the display transform (chosen).** One definition, one place;
  matches what the user sees on the (assumed sRGB) monitor; sits naturally next
  to the existing `gamutWarn` overlay. Cost: while soft-proofing a wide-gamut
  print profile, the clip warning still reflects sRGB, not the proof output.
- **Export-profile-relative.** Clipping would match the exported file exactly,
  but the preview display transform is always sRGB, so the overlay and the
  on-screen image would disagree about what's clipped — confusing during develop.
- **Proof-profile-relative when soft-proofing.** Clipping would track the active
  encode, but then "clipped" silently changes meaning as you toggle proofing,
  and it conflates the tonal warning with the chroma-reproduction one the gamut
  warning already covers.

## Consequences

- The Ubuf gains a packed `clipWarn` int (bit 0 highlight, bit 1 shadow),
  mirrored across `image.vert`, `image.frag`, `RendererCore.h`, and `fillUbuf()`.
- `clipWarn` must be forced to 0 for the export readback **and** the histogram
  readback (both run `displayEncode` on); the overlay exists only in the
  on-screen widget paint. This is the per-caller-uniform-leak class from
  docs/adr/0006 / the RHI migration.
- Tonal clipping (this) and the gamut warning (docs/adr/0001, soft-proof only)
  stay distinct concepts; see CONTEXT.md.
