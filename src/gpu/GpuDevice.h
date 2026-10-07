#pragma once

#include "DeviceImage.h"
#include "GpuContext.h"

#include <QOffscreenSurface>
#include <rhi/qrhi.h>

#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// QRhi's own condition for declaring its Vulkan parameters: a Vulkan-enabled
// Qt still hides QVulkanInstance from a build that cannot see vulkan.h.
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define ARRAW_GPU_VULKAN 1
#else
#define ARRAW_GPU_VULKAN 0
#endif

namespace arraw::detail {

/// @brief What one ::arraw::GpuPass needs on a device, built on its first use.
///
/// Member order is load-bearing: the pipeline goes before the layout and the
/// render pass it was created against.
struct PassPipeline {
    /// @brief Render pass every render target of this pass is made compatible with.
    std::unique_ptr<QRhiRenderPassDescriptor> renderPass;

    /// @brief Binding layout the pipeline was created with; never bound itself.
    std::unique_ptr<QRhiShaderResourceBindings> layout;

    /// @brief The pipeline itself.
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
};

/// @brief One QRhi device and everything it depends on, shared by its images.
///
/// Images hold this rather than the context, so a device image may outlive the
/// ::arraw::GpuContext that minted it. Member order is load-bearing: members are
/// destroyed in reverse, so the QRhi goes before the Vulkan instance and the
/// fallback surface it was created from, as QRhi requires.
///
/// Names QRhi, so it is included by arraw-gpu's own sources and the tests
/// that drive a device directly, never by a header core can see.
struct GpuDevice {
#if ARRAW_GPU_VULKAN
    /// @brief Vulkan instance the device was created from, if Vulkan.
    std::unique_ptr<QVulkanInstance> vulkan;
#endif
#if QT_CONFIG(opengl)
    /// @brief Surface an OpenGL device makes its context current against, if OpenGL.
    std::unique_ptr<QOffscreenSurface> fallbackSurface;
#endif
    /// @brief Readback results a failed frame left registered with the device.
    ///
    /// QRhi keeps the address of a readback's result until the readback
    /// completes, and a frame that failed after registering one ends before it
    /// does. The result stays alive here until QRhi itself is gone, which is why
    /// this is declared before it.
    std::vector<std::unique_ptr<QRhiReadbackResult>> abandonedReadbacks;

    /// @brief The device itself.
    std::unique_ptr<QRhi> rhi;

    /// @brief Sampler every pass binds its input with.
    ///
    /// Passes read texels with `texelFetch`, which ignores filtering and
    /// addressing, but a combined image sampler still needs one. Declared
    /// after the device, so it is released before it.
    std::unique_ptr<QRhiSampler> sampler;

    /// @brief Pipelines by ::arraw::GpuPass and then ::arraw::GpuTargetFormat, each empty until
    /// first used.
    std::array<PassPipeline, gpuPassCount * gpuTargetFormatCount> passes;

    /// @brief Identity carried by every image the device mints.
    DeviceId id = DeviceId::None;

    /// @brief Thread that created the device, and the only one that may use it.
    std::thread::id owner = std::this_thread::get_id();

    /// @brief Number of passes rendered to completion, and so of textures they made.
    std::size_t rendersDone = 0;

    /// @brief Whether a frame failed, after which the device is not used again.
    ///
    /// A failed submission may leave work pending that QRhi can neither finish
    /// nor forget, so the device is not trusted with more.
    bool lost = false;

    /// @brief Whether a frame failed or QRhi reports the device lost.
    [[nodiscard]] bool isLost() const noexcept {
        return lost || (rhi && rhi->isDeviceLost());
    }

    /// @brief Whether the calling thread is the owner.
    [[nodiscard]] bool onOwnerThread() const noexcept {
        return std::this_thread::get_id() == owner;
    }

    /// @brief Refuses use from any thread but the owner, or of a lost device.
    ///
    /// Cheap enough to do on every transfer, and the difference between an
    /// exception and a device corrupted in a way nobody can reproduce.
    /// @param action What was attempted, completing "can only ... on".
    /// @throws std::logic_error if called from another thread.
    /// @throws std::runtime_error if an earlier frame failed.
    void requireUsable(const char* action) const {
        if (!onOwnerThread()) {
            throw std::logic_error(std::string("A GPU device can only ") + action +
                                   " on the thread that created it");
        }
        if (isLost()) {
            throw std::runtime_error(std::string("The GPU device cannot ") + action +
                                     ": an earlier transfer failed and left it unusable");
        }
    }
};

/// @brief Submits one batch of resource updates and waits for it to finish.
///
/// An offscreen frame, because only that promises a readback is complete when
/// it ends. The frame is always ended once begun, and a batch never submitted
/// is released, even when @p record throws; if ending the frame fails, the
/// device is marked lost, since work QRhi could not finish may still be pending.
/// @param device Device to submit to.
/// @param purpose What the batch is for, as an infinitive without "to".
/// @param record Records the updates into the batch.
/// @throws std::runtime_error if the frame cannot be begun or fails.
void submitUpdates(GpuDevice& device, const std::string& purpose,
                   const std::function<void(QRhiResourceUpdateBatch&)>& record);

/// @brief Records one fullscreen pass into a render target and waits for it.
///
/// As ::arraw::detail::submitUpdates, in an offscreen frame that is always
/// ended once begun; a failed frame marks the device lost.
/// @param device Device to submit to.
/// @param purpose What the pass is for, as an infinitive without "to".
/// @param target Render target to draw into, created against @p pipeline's render pass.
/// @param pipeline Pipeline to draw with.
/// @param bindings Resources the pipeline reads.
/// @param record Records any updates the pass depends on, such as its uniforms.
/// @throws std::runtime_error if the frame cannot be begun or fails.
void submitPass(GpuDevice& device, const std::string& purpose, QRhiTextureRenderTarget& target,
                QRhiGraphicsPipeline& pipeline, QRhiShaderResourceBindings& bindings,
                const std::function<void(QRhiResourceUpdateBatch&)>& record);

} // namespace arraw::detail
