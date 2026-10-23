#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace arraw {

/// @brief What whoever ran arraw should make of one diagnostic.
enum class Severity {
    Info,    ///< What happened, when it went as asked.
    Warning, ///< It worked, but not as well as it should have.
    Error,   ///< What was asked did not happen: a photograph did not come out, or a check failed.
};

/// @brief Something worth telling whoever ran arraw about, by name.
///
/// The name rather than a sentence, because the sentence is presentation:
/// a log reader matches on this, a person reads what ::arraw::describe makes
/// of it, and a translation would replace only the latter.
enum class Notice {
    /// @brief A RAW declared no white balance, so one was substituted.
    SubstitutedWhiteBalance,

    /// @brief A photograph was rendered and written.
    Exported,

    /// @brief A photograph could not be rendered or written.
    InputFailed,

    /// @brief A batch reached its end, with a count of what did not come out.
    BatchFinished,

    /// @brief The GPU probe refused a software rasteriser, named, as not a GPU.
    GpuSoftwareRefused,

    /// @brief The GPU probe accepted a software rasteriser, named, because it was told to.
    GpuSoftwareAccepted,

    /// @brief The GPU probe left a software rasteriser untested, because it tests
    /// every adapter and was not told to accept one: the adapter's label, then its name.
    GpuAdapterSkipped,

    /// @brief The device has no RGBA32F textures, which development needs.
    GpuNoFloatTextures,

    /// @brief The named backend does not promise float readback, so the round trip decides.
    GpuReadBackNotPromised,

    /// @brief The device could not be created or used, with the reason.
    GpuFailed,

    /// @brief The image read back is not described as the one uploaded.
    GpuRoundTripRedescribed,

    /// @brief The round trip changed samples: how many, then the first one's
    /// channel, column, row, and value sent and received.
    GpuRoundTripChanged,

    /// @brief The GPU was turned off, by the named environment variable.
    GpuDisabled,

    /// @brief An export batch ran on the GPU: the backend, then the device's name.
    GpuUsed,

    /// @brief An export batch ran on the CPU, by choice or by fallback.
    CpuUsed,

    /// @brief The GPU was not used where it might have been, with the reason;
    /// about one photograph if it has a subject, otherwise about the whole batch.
    GpuFallback,

    /// @brief A setting read from a document lay outside its range and was
    /// clamped into it: the key, the value read, then the limit used instead.
    SettingClamped,

    /// @brief A document named a setting that does not exist, and it was
    /// ignored: the key.
    SettingUnknown,

    /// @brief A setting read from a document had the wrong shape, and it was
    /// ignored: the key, then what was expected.
    SettingMalformed,

    /// @brief A settings document was written by a newer arraw, and was read
    /// anyway: its version, then the newest version this arraw knows.
    NewerSettingsVersion,

    /// @brief A photograph's sidecar could not be read, so the photograph was
    /// opened without its settings and marks: the reason.
    SidecarUnreadable,

    /// @brief A photograph's EXIF could not be read, or it records none, so
    /// no capture information is shown: the reason.
    ExifUnreadable,

    /// @brief A folder among the inputs holds no photographs.
    NoPhotographs,

    /// @brief A batch left out the photographs its marks filter did not want: how many.
    FilteredOut,

    /// @brief An option was given that has no effect without another one: the
    /// option, then the one it needs.
    OptionIgnored,

    /// @brief A photograph's embedded preview could not be looked for, so none
    /// is shown: the reason.
    PreviewUnreadable,

    /// @brief A local adjustment read from a document was dropped: its position in the list
    /// (from 1), its id (empty when unreadable), its type (empty when unreadable), then why.
    LocalAdjustmentDropped,

    /// @brief A field of a local adjustment read from a document is not one arraw knows, and was
    /// ignored: the adjustment's position (from 1), its id, then the key.
    LocalAdjustmentFieldIgnored,

    /// @brief Strokes of a brush mask read from a document were dropped, cut or clamped, and the
    /// mask was kept: the mask's position (from 1), its id, how many strokes or fields, then why
    /// in words. One per mask and reason.
    BrushStrokesTrimmed,

    /// @brief An export left out metadata it could not read or copy from its
    /// source, but was written: what was left out and why.
    MetadataNotCarried,

    /// @brief A lens correction is on, but the photograph names no lens that can be told from
    /// another, such as an adapted manual lens, so it was skipped (ADR 050).
    LensUnidentified,

    /// @brief A lens correction is on, but a zoom's focal length was not recorded, so no profile
    /// applies and it was skipped: the lens.
    LensFocalUnknown,

    /// @brief The camera is not in the lens database. Informational: the lens was matched without
    /// it: the camera's maker, then its model.
    CameraNotInDatabase,

    /// @brief A lens correction is on, but no lens in the database fits the photograph, so it was
    /// skipped: the name the photograph gives the lens, empty if none.
    LensNotInDatabase,

    /// @brief A lens correction is on, but two lenses fit equally well, so none was applied and a
    /// photographer has to choose: the lenses' names, joined by ` | `.
    LensAmbiguous,

    /// @brief A lens correction is on, but the lens recorded for the photograph is not in the
    /// database, so it was skipped, not matched again: the lens.
    LensProfileMissing,

    /// @brief The lens database holds another stamp than the one the lens was recorded against, so
    /// its calibration may have been revised. Informational: the stamp recorded, then the stamp
    /// now.
    LensDatabaseChanged,

    /// @brief A lens correction is on, but no lens database could be loaded, so it was skipped.
    LensDatabaseMissing,

    /// @brief A lens correction is on, but this build has no lensfun (`ARRAW_WITH_LENSFUN`), so it
    /// was skipped.
    LensfunUnavailable,

    /// @brief The lens has no calibration for a correction that is on, so that correction was
    /// skipped: the lens, then the corrections, joined by `, `.
    LensNotCalibrated,

    /// @brief An export preset in the presets file cannot be used by this arraw, and was kept
    /// as it is: its name (empty when it has none), then why.
    ExportPresetUnusable,

    /// @brief The export presets file cannot be read, so it is treated as holding no presets and
    /// is never written over: the reason.
    ExportPresetsUnreadable,

    /// @brief Part of a photograph's snapshot list cannot be used and was left out or repaired
    /// when read: the position in the list (from 1, empty when about the list), the snapshot's
    /// id (empty when unreadable), then why in words. The sidecar keeps what it holds until a
    /// snapshot edit, which refuses a list it cannot keep whole.
    SnapshotDamaged,
};

/// @brief One detail of a diagnostic, kept as a value rather than as prose.
using DiagnosticValue = std::variant<std::string, double>;

/// @brief One thing that happened, in a form both a person and a program can read.
struct Diagnostic {
    /// @brief What happened; the one member a caller has to name, because a
    /// diagnostic that says nothing in particular is not one.
    Notice notice;

    /// @brief What a photographer should make of it.
    Severity severity = Severity::Info;

    /// @brief Photograph it concerns, absent when it concerns none, as a
    /// batch's own summary and the GPU probe do not.
    std::optional<std::filesystem::path> subject = std::nullopt;

    /// @brief Details the notice needs to be specific.
    std::vector<DiagnosticValue> values = {};
};

/// @brief Writes a diagnostic as a sentence.
/// @param diagnostic Diagnostic to describe.
/// @return The sentence, without the subject or a severity label: a log
/// decides how to present those.
[[nodiscard]] std::string describe(const Diagnostic& diagnostic);

/// @brief Somewhere for diagnostics to go.
///
/// Passed to the operations rather than held by them, so that a caller decides
/// what becomes of them: printed as they happen, collected and collapsed, or
/// dropped. One thread's work goes to one log; a log shared between threads
/// has to be able to say so itself.
class DiagnosticLog {
public:
    DiagnosticLog() = default;
    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;
    DiagnosticLog(DiagnosticLog&&) = delete;
    DiagnosticLog& operator=(DiagnosticLog&&) = delete;
    virtual ~DiagnosticLog() = default;

    /// @brief Takes one diagnostic.
    /// @param diagnostic What happened.
    virtual void record(const Diagnostic& diagnostic) = 0;
};

/// @brief A log that throws everything away.
///
/// The default for callers that do not want to hear, so that nothing has to
/// check whether it has somewhere to report to.
/// @return A log shared by every caller that wants none.
[[nodiscard]] DiagnosticLog& discardedDiagnostics();

/// @brief A log that keeps what it is given.
class CollectedDiagnostics final : public DiagnosticLog {
public:
    void record(const Diagnostic& diagnostic) override;

    /// @brief Diagnostics recorded so far, oldest first.
    [[nodiscard]] const std::vector<Diagnostic>& entries() const noexcept {
        return entries_;
    }

private:
    std::vector<Diagnostic> entries_;
};

} // namespace arraw
