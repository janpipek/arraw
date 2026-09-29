#include "DebugDiagnostics.h"

#include <QDebug>
#include <QString>

namespace arraw::app {

void DebugDiagnostics::record(const Diagnostic& diagnostic) {
    // Through UTF-16, as everywhere a path meets a QString: the narrow
    // string() is in the ANSI code page on Windows.
    const QString about = diagnostic.subject
                              ? QString::fromStdU16String(diagnostic.subject->u16string()) + ": "
                              : QString{};
    const QString message = about + QString::fromStdString(describe(diagnostic));

    switch (diagnostic.severity) {
    case Severity::Info:
        qInfo().noquote() << message;
        return;
    case Severity::Warning:
        qWarning().noquote() << message;
        return;
    case Severity::Error:
        qCritical().noquote() << message;
        return;
    }
}

} // namespace arraw::app
