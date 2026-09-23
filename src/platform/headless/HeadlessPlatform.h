#pragma once

/// @brief arraw's own Qt platform, for a process with no display server (Linux only).
///
/// A program makes it available by linking `arraw-headless-platform` and
/// writing `Q_IMPORT_PLUGIN(HeadlessPlatformPlugin)` at global scope, and
/// selects it by setting `QT_QPA_PLATFORM` to ::arraw::headless::platformKey
/// before it constructs its `QGuiApplication`. This header names no Qt type, so
/// that saying which platform to use does not drag in Qt's private API.
namespace arraw::headless {

/// @brief Key the platform is selected by, as `QT_QPA_PLATFORM` spells it.
inline constexpr char platformKey[] = "arraw-headless";

} // namespace arraw::headless
