// The CPU's std::pow, std::clamp and smoothstep, where the built-ins differ
// from them. Included, never compiled on its own.
//
// Powers. C++ std::pow and GLSL pow differ where GLSL leaves the result
// undefined: for x < 0, and for x == 0 with y <= 0. The callers (develop.frag
// says where its chain can reach them) give x >= 0, so the only case that
// needs care is x == 0, where std::pow gives 0 and a driver's
// exp2(y * log2(x)) gives it only by way of infinities. pow0 spells it out. It
// also returns NaN for x < 0 (as std::pow does for a non-integer exponent),
// although nothing reaches that today, so that a future caller cannot silently
// get an undefined value. NaN and +inf pass through pow as on the CPU.

// std::pow for the arguments the callers give it; see above.
float pow0(float x, float y) {
    if (x > 0.0) {
        return pow(x, y);
    }
    if (x == 0.0) {
        return 0.0;
    }
    // Negative, or NaN: NaN, as std::pow with a non-integer exponent.
    return uintBitsToFloat(0x7fc00000u);
}

// std::clamp(value, low, high), which the built-in clamp is not: it leaves a
// NaN alone where the built-in is undefined.
float clampExact(float value, float low, float high) {
    return value < low ? low : (high < value ? high : value);
}

// smoothstep, TonePlan.h. Not the built-in, which is undefined for
// first >= last and leaves the clamping to the driver.
float smoothStep(float first, float last, float value) {
    const float t = clampExact((value - first) / (last - first), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
