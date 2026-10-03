#include "QtMessageCapture.h"

#include "DebugDiagnostics.h"
#include "DebugLog.h"

#include <QMessageLogContext>
#include <QString>

#include <mutex>
#include <string_view>

namespace arraw::app {

namespace {

// Qt's handler is a plain function, so what it writes to has to be global.
// The mutex keeps a message from another thread off a model being let go.
std::mutex captureMutex;
QtMessageModel* captureModel = nullptr;
QtMessageHandler captureNext = nullptr;

/// @brief Hands a message on, and keeps a copy unless it is a diagnostic's echo.
void captureMessage(QtMsgType type, const QMessageLogContext& context, const QString& text) {
    // A model adding a row may itself log, which would come straight back
    // here on the same thread while the mutex is held.
    thread_local bool inside = false;
    if (!inside) {
        inside = true;
        {
            const std::scoped_lock lock(captureMutex);
            const std::string_view category = context.category ? context.category : "default";
            if (captureModel && category != diagnosticsCategory) {
                captureModel->append(type, QString::fromLatin1(category), text);
            }
        }
        inside = false;
    }
    if (captureNext) {
        captureNext(type, context, text);
    }
}

} // namespace

QtMessageCapture::QtMessageCapture(QtMessageModel& model) {
    const std::scoped_lock lock(captureMutex);
    captureModel = &model;
    previous_ = qInstallMessageHandler(captureMessage);
    // Qt answers with its default handler when none was installed.
    captureNext = previous_;
}

QtMessageCapture::~QtMessageCapture() {
    const std::scoped_lock lock(captureMutex);
    qInstallMessageHandler(previous_);
    captureModel = nullptr;
    captureNext = nullptr;
}

} // namespace arraw::app
