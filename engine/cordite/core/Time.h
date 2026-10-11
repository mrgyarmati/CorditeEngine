#pragma once

/// @file
/// Timing utilities.
///
/// @code
/// Stopwatch stopwatch;
/// importAsset(path);
/// CORDITE_LOG_INFO("Assets", "import took {:.1f} ms", stopwatch.elapsedMilliseconds());
///
/// FixedTimestep physicsClock(1.0 / 60.0);
/// const uint32_t steps = physicsClock.advance(frameSeconds);
/// for (uint32_t step = 0; step < steps; ++step) {
///     physics.step(physicsClock.stepSeconds());
/// }
/// renderInterpolated(physicsClock.alpha());
/// @endcode

#include <chrono>
#include <cstdint>

namespace cordite {

/// Measures elapsed wall-clock time with a monotonic clock.
class Stopwatch {
public:
    Stopwatch() : m_start(std::chrono::steady_clock::now()) {}

    void restart() { m_start = std::chrono::steady_clock::now(); }

    [[nodiscard]] double elapsedSeconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    }

    [[nodiscard]] double elapsedMilliseconds() const { return elapsedSeconds() * 1000.0; }

private:
    std::chrono::steady_clock::time_point m_start;
};

/// Converts variable frame times into a whole number of fixed simulation steps.
///
/// Running the simulation at a constant step keeps physics stable and reproducible no
/// matter how the frame rate fluctuates. When a frame takes so long that more than
/// `maxStepsPerFrame` steps would be needed, the surplus time is dropped: the simulation
/// slows down instead of entering a spiral where each frame takes longer than the last.
class FixedTimestep {
public:
    explicit FixedTimestep(double stepSeconds = 1.0 / 60.0, uint32_t maxStepsPerFrame = 8);

    /// Adds the duration of a frame and returns how many fixed steps to simulate now.
    /// Negative and non-finite durations are treated as zero.
    uint32_t advance(double frameSeconds);

    /// Fraction of a step accumulated but not yet simulated, in [0, 1). Rendering
    /// interpolates between the previous and the current simulation state with it.
    [[nodiscard]] double alpha() const;

    [[nodiscard]] double stepSeconds() const { return m_stepSeconds; }

    [[nodiscard]] uint32_t maxStepsPerFrame() const { return m_maxStepsPerFrame; }

    /// Changes the step length. Non-positive and non-finite values are ignored.
    void setStepSeconds(double stepSeconds);

    /// Discards the accumulated time, e.g. after loading a scene or resuming from pause.
    void reset() { m_accumulator = 0.0; }

private:
    double m_stepSeconds;
    uint32_t m_maxStepsPerFrame;
    double m_accumulator = 0.0;
};

} // namespace cordite
