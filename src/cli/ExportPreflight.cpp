#include "cli/ExportPreflight.h"
#include "ExportWorkflow.h"
#include <QFileInfo>
#include <QHash>

namespace cli {

ExportPlan planExports(
    const QStringList& inputs, const QString& outDir, ExportOptions::Format format) {
    ExportPlan plan;
    QHash<QString, QString> outputToInput;
    for (const QString& input : inputs) {
        if (QFileInfo(input).isDir()) {
            plan.error = QStringLiteral("'%1' is a directory — pass explicit files").arg(input);
            plan.items.clear();
            return plan;
        }
        const QString output = batchExportPath(outDir, input, format);
        const auto it = outputToInput.constFind(output);
        if (it != outputToInput.constEnd()) {
            plan.error = QStringLiteral("'%1' and '%2' both export to '%3' — rename or run separately")
                             .arg(it.value(), input, output);
            plan.items.clear();
            return plan;
        }
        outputToInput.insert(output, input);
        plan.items.push_back({input, output});
    }
    return plan;
}

} // namespace cli
