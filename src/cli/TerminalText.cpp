#include "TerminalText.h"

#include <QString>

namespace arraw::cli {

std::string terminalText(std::string_view text) {
    const QString input = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    QString output;
    output.reserve(input.size());
    for (qsizetype index = 0; index < input.size();) {
        char16_t code = input[index++].unicode();
        bool escaped = false;
        if (code == 0x1b) {
            if (index == input.size()) {
                break;
            }
            code = input[index++].unicode();
            escaped = true;
        }

        const bool csi = (escaped && code == '[') || (!escaped && code == 0x9b);
        if (csi) {
            /// CSI parameters and intermediates precede one final byte.
            while (index < input.size()) {
                const auto next = input[index].unicode();
                if (next >= 0x40 && next <= 0x7e) {
                    ++index;
                    break;
                }
                if (next < 0x20 || next > 0x3f) {
                    break;
                }
                ++index;
            }
            continue;
        }

        const bool osc = (escaped && code == ']') || (!escaped && code == 0x9d);
        const bool controlString =
            osc || (escaped && (code == 'P' || code == 'X' || code == '^' || code == '_')) ||
            (!escaped && (code == 0x90 || code == 0x98 || code == 0x9e || code == 0x9f));
        if (controlString) {
            /// Strip the payload too; unterminated strings consume the rest of the field.
            while (index < input.size()) {
                const auto next = input[index++].unicode();
                if (next == 0x9c || (osc && next == 0x07)) {
                    break;
                }
                if (next == 0x1b && index < input.size() && input[index] == u'\\') {
                    ++index;
                    break;
                }
            }
            continue;
        }

        if (escaped) {
            /// Other ESC sequences may contain intermediates before their final byte.
            while (code >= 0x20 && code <= 0x2f && index < input.size()) {
                code = input[index++].unicode();
            }
            continue;
        }
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f)) {
            continue;
        }
        output += QChar(code);
    }
    return output.toStdString();
}

} // namespace arraw::cli
