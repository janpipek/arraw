#include "HeadlessIntegration.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtPlugin>
#include <qpa/qplatformintegrationplugin.h>

namespace arraw::headless {

/// @brief Entry point Qt finds the headless platform through, by its key.
///
/// Built as a static plugin, so a program that wants the platform links this
/// library and says `Q_IMPORT_PLUGIN(HeadlessPlatformPlugin)` at global scope;
/// Qt then offers it, under the key in arraw-headless.json, alongside the
/// platforms it loads from disk. Nothing is installed next to Qt's own plugins.
class HeadlessPlatformPlugin final : public QPlatformIntegrationPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "arraw-headless.json")

public:
    // The argc/argv overload stays Qt's, which forwards to the one below.
    using QPlatformIntegrationPlugin::create;

    /// @brief Creates the platform when it is the one asked for.
    /// @param key Platform name Qt was given, compared as Qt compares platform names.
    /// @return The platform, or `nullptr` for any other key.
    QPlatformIntegration* create(const QString& key, const QStringList& /*parameters*/) override {
        if (key.compare(QLatin1StringView(platformKey), Qt::CaseInsensitive) != 0) {
            return nullptr;
        }
        return new HeadlessIntegration;
    }
};

} // namespace arraw::headless

// Outside the namespace: moc's output, which carries the static plugin's entry
// point, is written at global scope.
#include "HeadlessPlugin.moc"
