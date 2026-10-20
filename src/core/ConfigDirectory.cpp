#include "ConfigDirectoryRules.h"

#include <ConfigDirectory.h>

#include <QStandardPaths>
#include <QString>

#include <stdexcept>

namespace arraw {

std::filesystem::path resolveConfigDirectory(const std::u16string& override,
                                             const std::u16string& genericConfig) {
    if (!override.empty()) {
        return std::filesystem::absolute(std::filesystem::path(override)).lexically_normal();
    }
    if (genericConfig.empty()) {
        throw std::runtime_error(
            "cannot find the configuration directory: the platform gives no "
            "user configuration location (is the home directory unset?); set " +
            std::string(configDirectoryVariable) + " to a directory");
    }
    return std::filesystem::path(genericConfig) / "arraw";
}

std::filesystem::path configDirectory() {
    const QString override = qEnvironmentVariable(configDirectoryVariable);
    const QString generic = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return resolveConfigDirectory(override.toStdU16String(), generic.toStdU16String());
}

} // namespace arraw
