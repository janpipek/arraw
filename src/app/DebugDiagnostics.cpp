#include "DebugDiagnostics.h"

#include "DebugLog.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QString>

namespace arraw::app {

namespace {

Q_LOGGING_CATEGORY(diagnosticsLog, diagnosticsCategory)

} // namespace

void DebugDiagnostics::record(const Diagnostic& diagnostic) {
    if (model_ != nullptr) {
        model_->append(diagnostic);
    }

    // Through UTF-16, as everywhere a path meets a QString: the narrow
    // string() is in the ANSI code page on Windows.
    const QString about = diagnostic.subject
                              ? QString::fromStdU16String(diagnostic.subject->u16string()) + ": "
                              : QString{};
    const QString message = about + QString::fromStdString(describe(diagnostic));

    switch (diagnostic.severity) {
    case Severity::Info:
        qCInfo(diagnosticsLog).noquote() << message;
        return;
    case Severity::Warning:
        qCWarning(diagnosticsLog).noquote() << message;
        return;
    case Severity::Error:
        qCCritical(diagnosticsLog).noquote() << message;
        return;
    }
}

} // namespace arraw::app
