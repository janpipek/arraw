#pragma once

#include "ThumbnailCache.h"

#include <ImageBuffer.h>
#include <Progress.h>

#include <QImage>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace arraw::app {

/// @brief Outcome of decoding a photograph for the window.
struct DecodedPhoto {
    /// Identifier PhotoLoader::decode returned.
    std::uint64_t request = 0;
    /// File that was decoded.
    std::filesystem::path path;
    /// Decoded pixels; null when the decode failed.
    std::shared_ptr<const ImageBuffer> decoded;
    /// Description of the failure; empty when the decode succeeded.
    std::string error;
};

/// @brief Outcome of reading a photograph's embedded camera preview.
struct CameraPreview {
    /// Identifier PhotoLoader::readCameraPreview returned.
    std::uint64_t request = 0;
    /// File the preview is of.
    std::filesystem::path path;
    /// The preview, upright, at most ::arraw::app::ThumbnailCache::maxEdge on a side; null when
    /// the file has none or it cannot be read.
    QImage image;
};

/// @brief Worker threads that read photographs off the thread that asks for them (ADR 043).
///
/// Two lanes, each one thread serving only its newest job: the decode of the photograph being
/// opened, and the embedded camera preview the crop mode shows until its first render
/// (ADR 040). Separate, so that a slow preview read never holds up the decode of the next
/// photograph. A newer decode cancels the one in progress through its ProgressChannel
/// (ADR 042): a RAW decode stops at LibRaw's next check, and a cancelled decode delivers
/// nothing. The newest decode is never cancelled by another, so it always ends with a result,
/// unless cancelDecode() drops it. A preview read cannot be stopped part-way; a newer one
/// replaces one not yet started.
///
/// Uses no Qt signals, so it works without an event loop; the callbacks run on the worker
/// threads and the caller marshals them.
class PhotoLoader {
public:
    /// @brief Receiver of each decode that finished or failed, on the decode thread.
    using DecodedCallback = std::function<void(DecodedPhoto)>;
    /// @brief Receiver of each camera preview read, on the preview thread.
    using PreviewCallback = std::function<void(CameraPreview)>;
    /// @brief Receiver of a decode's progress, on the decode thread, with the decode's request.
    using ProgressCallback = std::function<void(std::uint64_t request, const Progress& progress)>;
    /// @brief Decoder of a file, reporting into and stopping on the channel it is given.
    using Decoder =
        std::function<ImageBuffer(const std::filesystem::path& path, ProgressChannel& channel)>;

    /// @brief Gives the decoder the window uses: ::arraw::loadImage, with diagnostics to Qt's
    /// debug output.
    [[nodiscard]] static Decoder imageDecoder();

    /// @brief Starts the threads.
    /// @param cache Where embedded previews are kept, as the film strip keeps them.
    /// @param onDecoded Receives each decode that finished or failed. Must not throw; what it
    /// throws is dropped.
    /// @param onPreview Receives each camera preview read; may be empty.
    /// @param onProgress Receives each decode's progress; may be empty.
    /// @param decoder Decodes a file; a test may give one it controls.
    PhotoLoader(ThumbnailCache cache, DecodedCallback onDecoded, PreviewCallback onPreview = {},
                ProgressCallback onProgress = {}, Decoder decoder = imageDecoder());

    PhotoLoader(const PhotoLoader&) = delete;
    PhotoLoader& operator=(const PhotoLoader&) = delete;
    PhotoLoader(PhotoLoader&&) = delete;
    PhotoLoader& operator=(PhotoLoader&&) = delete;

    /// @brief Cancels the decode in progress, drops queued jobs, and waits for both threads,
    /// stopped together.
    ///
    /// The callbacks are not called once this returns.
    ~PhotoLoader();

    /// @brief Queues the decode of a photograph, cancelling the one in progress.
    /// @param path File to decode.
    /// @return Identifier of the request, increasing with each call of this or
    /// readCameraPreview().
    std::uint64_t decode(std::filesystem::path path);

    /// @brief Drops the queued decode and cancels the one in progress; neither delivers.
    void cancelDecode();

    /// @brief Queues the read of a photograph's embedded camera preview, from the cache or the
    /// file.
    /// @param path File whose preview to read.
    /// @return Identifier of the request.
    std::uint64_t readCameraPreview(std::filesystem::path path);

private:
    /// One thread serving the newest job posted to it; defined in the source.
    class Lane;

    ThumbnailCache cache_;
    DecodedCallback onDecoded_;
    PreviewCallback onPreview_;
    ProgressCallback onProgress_;
    Decoder decoder_;
    /// Identifier of the newest request of either lane.
    std::atomic<std::uint64_t> lastId_{0};
    /// Declared after everything the jobs use, so that the threads stop first.
    std::unique_ptr<Lane> decodes_;
    std::unique_ptr<Lane> previews_;
};

} // namespace arraw::app
