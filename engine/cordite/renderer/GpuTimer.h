#pragma once

/// @file
/// Measures how long the GPU takes over a stretch of a command list, frame after frame.
///
/// A GPU does its work some time after the commands were recorded, so a measurement
/// arrives a few frames late. The timer keeps a small ring of queries, starts a new one
/// each frame and hands out the newest result that has come back - which it only reads
/// once the GPU has finished the whole command list the measurement is in.
///
/// @code
/// GpuTimer timer(device, graphics.adapter().supportsTimerQueries);
///
/// commandList->open();
/// timer.begin(commandList);
/// drawEverything(commandList);
/// timer.end(commandList);
/// commandList->close();
/// device->executeCommandList(commandList);
///
/// if (const std::optional<double> milliseconds = timer.latestMilliseconds()) {
///     showGpuTime(*milliseconds);
/// }
/// @endcode
///
/// Not thread-safe; use from the thread that records the command list.

#include "cordite/core/Base.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace cordite {

class GpuTimer {
public:
    /// `device` must outlive the timer. With `supported` false - an adapter that cannot
    /// time its work - the timer does nothing and never has a result.
    GpuTimer(nvrhi::IDevice* device, bool supported);
    ~GpuTimer() = default;
    CORDITE_NON_COPYABLE(GpuTimer);

    /// Marks the start of the stretch to measure. The command list must be open, and
    /// the one that held the previous stretch must have been executed by now. A frame
    /// is skipped, silently, while every query is still waiting for the GPU.
    void begin(nvrhi::ICommandList* commandList);

    /// Marks the end of the stretch, in the command list begin() was given.
    void end(nvrhi::ICommandList* commandList);

    /// The newest measurement that has arrived, in milliseconds. Nothing before the
    /// first one arrives, which takes a few frames, and never anything on an adapter
    /// that cannot measure.
    [[nodiscard]] std::optional<double> latestMilliseconds() const { return m_latest; }

    /// How many measurements have arrived so far.
    [[nodiscard]] uint64_t measurementCount() const { return m_measurementCount; }

private:
    enum class State : uint8_t {
        Free,
        /// Between begin() and end().
        Measuring,
        /// Recorded; its command list is about to be executed.
        Ended,
        /// Handed to the GPU; the result has not come back yet.
        Pending,
    };

    struct Query {
        nvrhi::TimerQueryHandle timer;
        /// Tells when the GPU has finished the command list the measurement is in.
        nvrhi::EventQueryHandle submitted;
        State state = State::Free;
    };

    /// Takes in every result that has come back and frees its query.
    void collect();

    /// More than the number of frames a GPU is ever behind.
    static constexpr size_t kQueryCount = 4;

    nvrhi::IDevice* m_device;
    std::array<Query, kQueryCount> m_queries;
    /// The query of the stretch being measured, when there is one.
    std::optional<size_t> m_current;
    std::optional<double> m_latest;
    uint64_t m_measurementCount = 0;
};

} // namespace cordite
