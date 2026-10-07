// The bounds of the Presence context's luminance, shared by
// presence_filter.frag and develop.frag. Included, never compiled on its own.

// presenceLuminanceFloor and presenceLuminanceCeiling in Presence.h: 2^-14 and 2^16.
const float presenceLuminanceFloor = 6.103515625e-05;
const float presenceLuminanceCeiling = 65536.0;

// boundedLuminance(), Presence.h: NaN and below the floor to the floor, above
// the ceiling to the ceiling.
float boundedLuminance(float luminance) {
    if (!(luminance > presenceLuminanceFloor)) {
        return presenceLuminanceFloor;
    }
    return luminance < presenceLuminanceCeiling ? luminance : presenceLuminanceCeiling;
}
