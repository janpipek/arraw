# Shader owns adjustments; color encode is a separate CPU stage (lcms2)

The original invariant was "the GLSL shader is the single source of truth" — it ended
with a pow(1/2.2) encode and export read back gamma-encoded pixels. With a Rec.2020
working space and multiple output profiles, we split the pipeline: the shader remains
the single source of truth for all *adjustments* (always in linear working space), and
the color *encode* is a separate stage — in the shader for display (hardcoded
Rec.2020→sRGB matrix + true sRGB curve, monitor assumed sRGB until soft-proofing
lands), and on the CPU via lcms2 for export (linear float readback → output profile,
ICC embedded). The alternative — per-output-profile shader permutations — was
rejected, and Qt's QColorTransform was rejected because soft-proofing needs lcms2's
rendering intents and arbitrary printer profiles anyway.

WYSIWYG is preserved because display and export apply the *same* adjustments to the
same linear pixels; only the final encode differs per destination.
