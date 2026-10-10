#include "StrokeCodec.h"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace arraw {

namespace {

[[noreturn]] void malformed(const char* reason) {
    throw std::invalid_argument(std::string("malformed stroke: ") + reason);
}

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

void appendNumber(std::string& out, float value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

/// Reads text token by token.
class TextReader {
public:
    explicit TextReader(std::string_view text) : text_(text) {}

    void skipSpace() noexcept {
        while (at_ < text_.size() && isSpace(text_[at_])) {
            ++at_;
        }
    }

    [[nodiscard]] bool done() const noexcept {
        return at_ >= text_.size();
    }

    [[nodiscard]] char peek() const noexcept {
        return done() ? '\0' : text_[at_];
    }

    void expect(char c, const char* reason) {
        if (peek() != c || done()) {
            malformed(reason);
        }
        ++at_;
    }

    /// Reads a finite float that ends at a space, a delimiter or the end.
    float number(char delimiter) {
        float value = 0.0F;
        const char* first = text_.data() + at_;
        const char* last = text_.data() + text_.size();
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc{} || !std::isfinite(value)) {
            malformed("not a finite number");
        }
        at_ += static_cast<std::size_t>(result.ptr - first);
        if (!done() && !isSpace(peek()) && peek() != delimiter) {
            malformed("junk after a number");
        }
        return value;
    }

private:
    std::string_view text_;
    std::size_t at_ = 0;
};

constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t left = bytes.size() - i;
        const std::uint32_t chunk = (std::uint32_t{bytes[i]} << 16) |
                                    (left > 1 ? std::uint32_t{bytes[i + 1]} << 8 : 0U) |
                                    (left > 2 ? std::uint32_t{bytes[i + 2]} : 0U);
        out.push_back(alphabet[(chunk >> 18) & 63U]);
        out.push_back(alphabet[(chunk >> 12) & 63U]);
        out.push_back(left > 1 ? alphabet[(chunk >> 6) & 63U] : '=');
        out.push_back(left > 2 ? alphabet[chunk & 63U] : '=');
    }
    return out;
}

/// Maps a character to its six bits, 64 for padding and 255 for anything else.
constexpr std::array<std::uint8_t, 256> decodingTable = [] {
    std::array<std::uint8_t, 256> table{};
    table.fill(255);
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        table[static_cast<unsigned char>(alphabet[i])] = static_cast<std::uint8_t>(i);
    }
    table[static_cast<unsigned char>('=')] = 64;
    return table;
}();

std::vector<std::uint8_t> base64Decode(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 4 * 3 + 3);
    std::array<std::uint8_t, 4> group{};
    std::size_t filled = 0;
    int padding = 0;
    bool closed = false;
    for (const char c : text) {
        if (isSpace(c)) {
            continue;
        }
        const std::uint8_t value = decodingTable[static_cast<unsigned char>(c)];
        if (value == 255) {
            malformed("bad base64 character");
        }
        // Padding closes the text: nothing may follow the last group.
        if (closed) {
            malformed("bad base64 padding");
        }
        if (value == 64) {
            if (filled < 2) {
                malformed("bad base64 padding");
            }
            ++padding;
        } else if (padding > 0) {
            malformed("bad base64 padding");
        }
        group[filled++] = value == 64 ? 0 : value;
        if (filled < 4) {
            continue;
        }
        const std::uint32_t chunk = (std::uint32_t{group[0]} << 18) |
                                    (std::uint32_t{group[1]} << 12) |
                                    (std::uint32_t{group[2]} << 6) | std::uint32_t{group[3]};
        // The bits left over by padding must be zero, so one text has one reading.
        if ((padding == 1 && (chunk & 0xFFU) != 0) || (padding == 2 && (chunk & 0xFFFFU) != 0)) {
            malformed("bad base64 padding bits");
        }
        bytes.push_back(static_cast<std::uint8_t>(chunk >> 16));
        if (padding < 2) {
            bytes.push_back(static_cast<std::uint8_t>(chunk >> 8));
        }
        if (padding < 1) {
            bytes.push_back(static_cast<std::uint8_t>(chunk));
        }
        closed = padding > 0;
        filled = 0;
        padding = 0;
    }
    if (filled != 0 || bytes.empty()) {
        malformed("bad base64 length");
    }
    return bytes;
}

void putVarint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80U));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void putFloat(std::vector<std::uint8_t>& out, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    }
}

/// Maps a float's bits to an unsigned integer that rises with the float.
std::uint32_t ordered(float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return (bits & 0x80000000U) != 0 ? ~bits : bits | 0x80000000U;
}

float fromOrdered(std::uint32_t order) noexcept {
    return std::bit_cast<float>((order & 0x80000000U) != 0 ? order & 0x7FFFFFFFU : ~order);
}

std::uint64_t zigzag(std::int64_t value) noexcept {
    return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
}

std::int64_t unzigzag(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1U);
}

/// Reads bytes in order.
class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - at_;
    }

    std::uint8_t byte() {
        if (at_ >= bytes_.size()) {
            malformed("truncated data");
        }
        return bytes_[at_++];
    }

    float real() {
        std::uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) {
            bits |= std::uint32_t{byte()} << (8 * i);
        }
        const auto value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) {
            malformed("not a finite number");
        }
        return value;
    }

    /// Reads a LEB128 value of at most five bytes, in its shortest form.
    std::uint64_t varint() {
        std::uint64_t value = 0;
        for (int i = 0; i < 5; ++i) {
            const std::uint8_t b = byte();
            value |= std::uint64_t{b & 0x7FU} << (7 * i);
            if ((b & 0x80U) == 0) {
                // A last byte of zero after others is a longer spelling of a shorter number.
                if (i > 0 && b == 0) {
                    malformed("non-canonical varint");
                }
                return value;
            }
        }
        malformed("varint over five bytes");
    }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t at_ = 0;
};

} // namespace

std::string strokeText(const Stroke& stroke) {
    std::string out;
    out.reserve(32 + stroke.points.size() * 20);
    appendNumber(out, stroke.radius);
    out.push_back(' ');
    appendNumber(out, stroke.hardness);
    out.push_back(' ');
    appendNumber(out, stroke.flow);
    out += stroke.erase ? " 1;" : " 0;";
    bool first = true;
    for (const SensorPoint& point : stroke.points) {
        if (!first) {
            out.push_back(' ');
        }
        first = false;
        appendNumber(out, point.u);
        out.push_back(',');
        appendNumber(out, point.v);
    }
    return out;
}

DecodedStroke strokeFromText(std::string_view text, std::size_t pointLimit) {
    TextReader in(text);
    DecodedStroke decoded;
    Stroke& stroke = decoded.stroke;
    in.skipSpace();
    stroke.radius = in.number(' ');
    in.skipSpace();
    stroke.hardness = in.number(' ');
    in.skipSpace();
    stroke.flow = in.number(' ');
    in.skipSpace();
    const char erase = in.peek();
    if (erase != '0' && erase != '1') {
        malformed("erase must be 0 or 1");
    }
    stroke.erase = erase == '1';
    in.expect(erase, "erase");
    in.skipSpace();
    in.expect(';', "missing ';'");
    in.skipSpace();
    if (in.done()) {
        malformed("no points");
    }
    while (!in.done()) {
        SensorPoint point;
        point.u = in.number(',');
        in.expect(',', "a point needs two numbers");
        point.v = in.number(' ');
        if (in.peek() == ',') {
            malformed("a point has three components");
        }
        if (stroke.points.size() < pointLimit) {
            stroke.points.push_back(point);
        } else {
            ++decoded.pointsDropped;
        }
        in.skipSpace();
    }
    return decoded;
}

std::string strokeBase64(const Stroke& stroke) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(16 + stroke.points.size() * 6);
    bytes.push_back(1);
    bytes.push_back(stroke.erase ? 1 : 0);
    putFloat(bytes, stroke.radius);
    putFloat(bytes, stroke.hardness);
    putFloat(bytes, stroke.flow);
    putVarint(bytes, stroke.points.size());
    std::int64_t previousU = ordered(0.0F);
    std::int64_t previousV = previousU;
    for (const SensorPoint& point : stroke.points) {
        const std::int64_t u = ordered(point.u);
        const std::int64_t v = ordered(point.v);
        putVarint(bytes, zigzag(u - previousU));
        putVarint(bytes, zigzag(v - previousV));
        previousU = u;
        previousV = v;
    }
    return base64Encode(bytes);
}

DecodedStroke strokeFromBase64(std::string_view text, std::size_t pointLimit) {
    const std::vector<std::uint8_t> bytes = base64Decode(text);
    ByteReader in(bytes);
    if (in.byte() != 1) {
        malformed("unknown format");
    }
    const std::uint8_t flags = in.byte();
    if ((flags & ~1U) != 0) {
        malformed("reserved flag set");
    }
    DecodedStroke decoded;
    Stroke& stroke = decoded.stroke;
    stroke.erase = (flags & 1U) != 0;
    stroke.radius = in.real();
    stroke.hardness = in.real();
    stroke.flow = in.real();
    const std::uint64_t count = in.varint();
    if (count == 0 || count > 0xFFFFFFFFULL) {
        malformed("bad point count");
    }
    if (count > in.remaining() / 2) {
        malformed("point count does not match the data");
    }
    stroke.points.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, pointLimit)));
    std::int64_t u = ordered(0.0F);
    std::int64_t v = u;
    for (std::uint64_t i = 0; i < count; ++i) {
        u += unzigzag(in.varint());
        v += unzigzag(in.varint());
        if (u < 0 || u > 0xFFFFFFFFLL || v < 0 || v > 0xFFFFFFFFLL) {
            malformed("value outside 32 bits");
        }
        const SensorPoint point{fromOrdered(static_cast<std::uint32_t>(u)),
                                fromOrdered(static_cast<std::uint32_t>(v))};
        if (!std::isfinite(point.u) || !std::isfinite(point.v)) {
            malformed("not a finite number");
        }
        if (stroke.points.size() < pointLimit) {
            stroke.points.push_back(point);
        } else {
            ++decoded.pointsDropped;
        }
    }
    if (in.remaining() != 0) {
        malformed("trailing bytes");
    }
    return decoded;
}

} // namespace arraw
