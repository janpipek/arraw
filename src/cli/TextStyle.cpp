#include "cli/TextStyle.h"
#include <cstdio>
#include <QByteArray>
#include <QtGlobal> // Q_OS_WIN, before the platform split below

#if defined(Q_OS_WIN)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cli {

namespace {

const char* escapeFor(Ink ink) {
    switch (ink) {
    case Ink::Bold:
        return "\033[1m";
    case Ink::Dim:
        return "\033[2m";
    case Ink::Red:
        return "\033[31m";
    case Ink::Green:
        return "\033[32m";
    case Ink::Yellow:
        return "\033[33m";
    case Ink::Blue:
        return "\033[34m";
    case Ink::Magenta:
        return "\033[35m";
    case Ink::Cyan:
        return "\033[36m";
    }
    return "";
}

// Is the *stdout we actually write to* a console that will render escapes?
//
// Deliberately narrow, because every way of getting this wrong prints raw
// escape bytes into someone's file:
//   - asks about `stdout`'s own descriptor, the one QTextStream(stdout) writes
//     to, not descriptor 1 by number — those differ once stdout is reopened;
//   - treats any non-zero isatty() as true: POSIX specifies 1, but the
//     contract is "non-zero", and errno is left alone either way;
//   - on Windows, _isatty() is true for *any* character device — NUL and
//     serial ports included — so a real console must also answer
//     GetConsoleMode, and must accept virtual-terminal processing before we
//     believe it can render anything. A pre-VT console refuses, and would
//     otherwise print the escapes literally.
#if defined(Q_OS_WIN)
bool stdoutIsTerminal() {
    const int fd = _fileno(stdout);
    if (fd < 0 || _isatty(fd) == 0)
        return false;

    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (console == nullptr || console == INVALID_HANDLE_VALUE || !GetConsoleMode(console, &mode))
        return false;
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
        return true;
    // Enabling VT processing mutates the console for the rest of the process;
    // that is the point, and it is why this runs once, from main.
    return SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#else
bool stdoutIsTerminal() {
    const int fd = fileno(stdout);
    return fd >= 0 && isatty(fd) != 0;
}
#endif

// Present and non-empty, per the convention each variable documents.
bool envSet(const char* name) {
    return !qEnvironmentVariableIsEmpty(name);
}

} // namespace

TextStyle TextStyle::forStdout() {
    // no-color.org: any non-empty value disables colour, and it outranks every
    // opt-in below — a user who exports NO_COLOR means it.
    if (envSet("NO_COLOR"))
        return TextStyle(false);

    // The escape hatch the terminal check can't cover: `arraw info *.arw |
    // less -R` is a pipe, so isatty says no, but the user does want colour.
    if (qgetenv("CLICOLOR_FORCE") != QByteArray("0") && envSet("CLICOLOR_FORCE"))
        return TextStyle(true);

    if (qgetenv("TERM") == QByteArray("dumb"))
        return TextStyle(false);

    return TextStyle(stdoutIsTerminal());
}

QString TextStyle::paint(const QString& text, Ink ink) const {
    if (!enabled_ || text.isEmpty())
        return text;
    return QLatin1String(escapeFor(ink)) + text + QLatin1String("\033[0m");
}

} // namespace cli
