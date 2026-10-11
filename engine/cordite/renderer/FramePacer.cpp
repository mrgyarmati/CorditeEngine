#include "cordite/renderer/FramePacer.h"

#include "cordite/core/Assert.h"

#include <algorithm>

namespace cordite {

FramePacer::FramePacer(nvrhi::IDevice* device, uint32_t maxFramesInFlight) : m_device(device) {
    CORDITE_ASSERT(device != nullptr, "a frame pacer needs a device");
    CORDITE_ASSERT(maxFramesInFlight >= 1, "at least one frame has to be allowed in flight");
    m_frames.resize(std::max(maxFramesInFlight, 1u));
    if (device == nullptr) {
        return;
    }
    for (Frame& frame : m_frames) {
        // A query that cannot be created stays null; its slot then never holds a frame
        // back, which costs the bound but not the program.
        frame.finished = device->createEventQuery();
    }
}

void FramePacer::frameSubmitted() {
    if (m_device == nullptr) {
        return;
    }
    // The slot of the oldest frame. If the GPU has not finished that one, the frame just
    // submitted would be one too many: wait for it.
    Frame& frame = m_frames[m_next];
    m_next = (m_next + 1) % m_frames.size();
    if (frame.finished == nullptr) {
        return;
    }
    if (frame.submitted) {
        m_device->waitEventQuery(frame.finished);
        m_device->resetEventQuery(frame.finished);
    }
    m_device->setEventQuery(frame.finished, nvrhi::CommandQueue::Graphics);
    frame.submitted = true;
}

uint32_t FramePacer::framesInFlight() const {
    uint32_t count = 0;
    for (const Frame& frame : m_frames) {
        if (frame.submitted && frame.finished != nullptr && !m_device->pollEventQuery(frame.finished)) {
            ++count;
        }
    }
    return count;
}

} // namespace cordite
