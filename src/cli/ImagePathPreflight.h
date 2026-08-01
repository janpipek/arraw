// The batch-input pre-flight shared by every command that takes a list of
// image files (docs/adr/0051, docs/adr/0053): each path must exist, must not
// be a directory (no folder mode in v1), and must carry an extension one of
// the loaders recognises. Checked as a whole before any file is touched —
// refusing up front is cheaper than half-finishing and discovering the typo
// at file 30 of 40. Reuses the loaders' own extension lists rather than
// growing a third definition of "image file".

#pragma once

#include <QString>
#include <QStringList>

namespace cli {

// Empty when the whole list is runnable; otherwise the first failure,
// phrased for a stderr line ("no such file: shot.arw").
QString preflightImagePaths(const QStringList& paths);

} // namespace cli
