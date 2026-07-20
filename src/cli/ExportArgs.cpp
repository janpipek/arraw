#include "cli/ExportArgs.h"
#include <CLI11.hpp>
#include <map>

namespace cli {

ExportParse parseExportArgs(const std::vector<std::string>& args) {
    ExportParse res;
    ExportOptions& opt = res.invocation.options;

    CLI::App app{"Render files through their develop sidecars and write exports", "arraw export"};
    std::vector<std::string> inputs;
    std::string outDir;
    app.add_option("inputs", inputs, "Files to export (RAW or standard images)")->required();
    app.add_option("-o,--out-dir", outDir, "Output directory (created if missing)")->required();

    const std::map<std::string, ExportOptions::Format> formats{
        {"jpeg", ExportOptions::Format::JPEG},
        {"png", ExportOptions::Format::PNG},
        {"tiff", ExportOptions::Format::TIFF}};
    app.add_option("--format", opt.format, "Output format: jpeg|png|tiff (default jpeg)")
        ->transform(CLI::CheckedTransformer(formats, CLI::ignore_case));
    auto* quality = app.add_option("--quality", opt.quality, "JPEG quality 1..100 (default 90)")
                        ->check(CLI::Range(1, 100));
    app.add_option("--width", opt.width, "Output width in px, 0 = original")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--height", opt.height, "Output height in px, 0 = original")
        ->check(CLI::NonNegativeNumber);
    const std::map<std::string, OutputProfile> profiles{
        {"srgb", OutputProfile::SRgb},
        {"p3", OutputProfile::DisplayP3},
        {"adobergb", OutputProfile::AdobeRgb}};
    app.add_option("--profile", opt.profile, "Output profile: srgb|p3|adobergb (default srgb)")
        ->transform(CLI::CheckedTransformer(profiles, CLI::ignore_case));
    auto* bitDepth = app.add_option("--bit-depth", opt.bitDepth, "8 or 16; 16 requires --format tiff")
                         ->check(CLI::IsMember({8, 16}));
    app.add_option("--sharpen", opt.sharpening, "Output sharpening amount (default 0)")
        ->check(CLI::NonNegativeNumber);
    // Each metadata flag flips its field away from the product default
    // (docs/adr/0043), so no flag ever restates a default.
    app.add_flag_callback(
        "--include-location", [&opt] { opt.metadata.includeLocation = true; },
        "Embed GPS tags (off by default)");
    app.add_flag_callback(
        "--no-capture-info", [&opt] { opt.metadata.includeCaptureInfo = false; },
        "Omit camera/capture EXIF (on by default)");
    app.add_flag_callback(
        "--no-descriptive", [&opt] { opt.metadata.includeDescriptive = false; },
        "Omit title/caption/keywords/rating metadata (on by default)");

    // CLI11's vector overload consumes back-to-front; hand it argc/argv.
    std::vector<const char*> argv;
    argv.push_back("arraw export");
    for (const std::string& a : args)
        argv.push_back(a.c_str());
    try {
        app.parse(int(argv.size()), argv.data());
    } catch (const CLI::CallForHelp&) {
        res.exitCode = 0;
        res.message = QString::fromStdString(app.help());
        return res;
    } catch (const CLI::ParseError& e) {
        res.exitCode = 2;
        res.message = QStringLiteral("arraw export: %1").arg(e.what());
        return res;
    }

    // A silently ignored flag is a lie (docs/adr/0049): error where the GUI
    // would merely disable the field.
    if (quality->count() > 0 && opt.format != ExportOptions::Format::JPEG) {
        res.exitCode = 2;
        res.message = "arraw export: --quality applies only to --format jpeg";
        return res;
    }
    if (bitDepth->count() > 0 && opt.bitDepth == 16 && opt.format != ExportOptions::Format::TIFF) {
        res.exitCode = 2;
        res.message = "arraw export: --bit-depth 16 requires --format tiff";
        return res;
    }

    for (const std::string& s : inputs)
        res.invocation.inputs << QString::fromStdString(s);
    res.invocation.outDir = QString::fromStdString(outDir);
    return res;
}

} // namespace cli
