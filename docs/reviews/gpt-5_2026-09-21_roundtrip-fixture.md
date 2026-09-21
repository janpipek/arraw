# Round-trip test and fixture review

Scope: `tests/test_RoundTrip.cpp`, `tests/fixtures/`, and their directly
supporting test helpers and documentation.

The separation is sound: a committed, dependency-free fixture generator keeps
the input independent of the code under test, while the test itself remains a
short black-box round trip. I would not merge the generator's patch definition
into the C++ test: keeping their expectations independently stated is what lets
the fixture-sanity case detect a stale or malformed committed PNG.

## Findings

### High — the lossless test name promises exact equality that the assertions reject

`tests/test_RoundTrip.cpp:79` is named “A lossless round trip preserves every
pixel”, but lines 106–108 expressly allow a one-code error and require only
80% of pixels to be exact. The detailed comment correctly explains this as the
colour transform not being self-inverse, so the mismatch is only in the test's
headline — but headlines are what a failing CTest run presents first.

Rename it to state the actual contract, for example “Lossless formats stay
within the measured colour-conversion bound”. This also makes clear that
“lossless” describes the codec, not the entire colour-managed pipeline.

### Medium — the JPEG rationale overstates the fixture's contents

`tests/test_RoundTrip.cpp:141` says the test card “is all hard patch edges”.
The fixture also contains a hue sweep and grey/red/green/blue ramps
(`tests/fixtures/make_fixtures.py:169–194`). The intended point — that the
card *contains* hard patch edges that expose DCT ringing — is valid, but the
word “all” makes the measured JPEG bound look less representative than it is.

Change this to “contains hard patch edges” (or name the bottom patch band).

## Verification

- `just test` — passed: 29/29 tests.

