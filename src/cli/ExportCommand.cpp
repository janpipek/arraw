#include "cli/ExportCommand.h"
#include "cli/ExportPreflight.h"
#include "DevelopSession.h"
#include "ExportWorkflow.h"
#include "ImageLoadWorkflow.h"
#include "core/CropGeometry.h"
#include "render/HeadlessRenderContext.h"
#include "render/OffscreenRender.h"
#include "render/RendererCore.h"
#include <atomic>
#include <memory>
#include <QDir>
#include <QFileInfo>

namespace cli {

int runExport(const ExportInvocation& inv, QTextStream& out, QTextStream& err) {
    const ExportPlan plan = planExports(inv.inputs, inv.outDir, inv.options.format);
    if (!plan.error.isEmpty()) {
        err << "arraw export: " << plan.error << "\n";
        return 2;
    }
    if (!QDir().mkpath(inv.outDir)) {
        err << "arraw export: cannot create output directory '" << inv.outDir << "'\n";
        return 2;
    }

    QString rhiError;
    const auto ctx = HeadlessRenderContext::create(&rhiError);
    if (!ctx) {
        err << "arraw export: " << rhiError << "\n";
        return 2;
    }
    RendererCore core;
    core.initialize(ctx->rhi());

    int exported = 0;
    int failed = 0;
    const auto cancel = std::make_shared<std::atomic<bool>>(false);

    out << "Exporting " << plan.items.size() << " images...\n";

    for (const ExportPlanItem& item : plan.items) {
        // Same recipe as the GUI batch export (src/MainWindow.cpp): the
        // demosaic choice parameterises the decode itself (docs/adr/0036).
        const DemosaicAlgorithm algo
            = resolveImageAdjustments(item.input, QRectF(0, 0, 1, 1)).demosaicAlgorithm;
        const LoadResult loaded = decodeImage(item.input, nullptr, cancel, algo);
        if (!loaded.error.isEmpty()) {
            err << item.input << ": " << loaded.error << "\n";
            ++failed;
            continue;
        }

        // DevelopSession owns the corrected negative (lens corrections then
        // spots, docs/adr/0032/0017) — reuse it rather than fork the order.
        const ResolvedLoadedImage resolved = resolveLoadedImage(item.input, loaded);
        DevelopSession session;
        session.beginLoading(item.input);
        session.setLoadedImage(item.input, loaded, resolved.adjustments, resolved.sidecarState,
                               resolved.metadata, resolved.metadataPresence, resolved.snapshots);
        const ImageBuffer& buf = session.fullResForExport();
        if (!buf.valid()) {
            err << item.input << ": no image data after develop\n";
            ++failed;
            continue;
        }

        const QSize natural = crop::cropPixelSize(
            buf.width, buf.height, resolved.adjustments.cropRect, resolved.adjustments.orientation);
        const int outW = inv.options.width > 0 ? inv.options.width : natural.width();
        const int outH = inv.options.height > 0 ? inv.options.height : natural.height();

        QImage linear = offscreen::renderToImage(core, buf, resolved.adjustments, outW, outH);
        if (linear.isNull()) {
            err << item.input << ": GPU render failed\n";
            ++failed;
            continue;
        }

        const ExportTailResult tail = runExportTail(
            std::move(linear), inv.options, item.output, item.input, resolved.metadata);
        if (!tail.saved) {
            err << item.input << ": could not write '" << item.output << "'\n";
            ++failed;
            continue;
        }
        if (tail.metadata == ExportMetadataStatus::Failed)
            err << item.input << ": warning: metadata embed failed (image written)\n";

        out << "Exported: " << item.input << " -> " << item.output << "\n";
        out.flush(); // progress must appear per file, not at exit
        ++exported;
    }

    core.release();
    out << exported << " exported, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace cli
