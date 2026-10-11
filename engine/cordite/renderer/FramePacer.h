#pragma once

/// @file
/// Keeps the CPU from getting further ahead of the GPU than a few frames.
///
/// Recording a frame is usually quicker than drawing it. When frames are shown in a
/// window, waiting for the display holds the CPU back; without a window nothing does, and
/// a program that renders in a loop would hand the GPU frame after frame, each keeping
/// its buffers and constants alive until its turn comes. A pacer bounds that: once the
/// given number of frames is waiting, handing over another waits for the oldest.
///
/// @code
/// FramePacer pacer(device);
///
/// device->executeCommandList(commandList);   // the frame
/// pacer.frameSubmitted();                    // returns at once, unless the GPU is three frames behind
/// @endcode
///
/// Not thread-safe; use from the thread that submits the frames.

#include "cordite/core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cordite {

class FramePacer {
public:
    /// How many frames may be waiting for the GPU unless told otherwise: enough to keep
    /// it busy, few enough that what the user does shows up without noticeable delay.
    static constexpr uint32_t kDefaultFramesInFlight = 3;

    /// `device` must outlive the pacer. `maxFramesInFlight` is at least 1.
    explicit FramePacer(nvrhi::IDevice* device, uint32_t maxFramesInFlight = kDefaultFramesInFlight);
    ~FramePacer() = default;
    CORDITE_NON_COPYABLE(FramePacer);

    /// Call after a frame's command lists have been executed; "the frame" is everything
    /// executed since the last call. Waits, if it has to, until no more than the maximum
    /// number of frames are waiting for the GPU, this one included.
    void frameSubmitted();

    /// How many of the frames it was told of the GPU has not finished yet. Never more
    /// than the maximum once frameSubmitted() has returned.
    [[nodiscard]] uint32_t framesInFlight() const;

    [[nodiscard]] uint32_t maxFramesInFlight() const { return static_cast<uint32_t>(m_frames.size()); }

private:
    struct Frame {
        /// Tells when the GPU has finished the frame.
        nvrhi::EventQueryHandle finished;
        bool submitted = false;
    };

    nvrhi::IDevice* m_device;
    /// A ring: the next frame takes the slot of the oldest.
    std::vector<Frame> m_frames;
    size_t m_next = 0;
};

} // namespace cordite
