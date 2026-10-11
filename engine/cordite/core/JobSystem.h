#pragma once

/// @file
/// Multithreaded job system.
///
/// A fixed pool of worker threads executes short, CPU-bound jobs. The calling thread
/// helps run jobs while it waits, so a single-threaded pool still makes progress.
///
/// @code
/// JobSystem jobs;
/// jobs.parallelFor(static_cast<uint32_t>(items.size()), 64, [&](uint32_t begin, uint32_t end) {
///     for (uint32_t index = begin; index < end; ++index) {
///         process(items[index]);
///     }
/// });
///
/// JobHandle handle = jobs.submit([&] { bakeLighting(); });
/// jobs.wait(handle);
/// @endcode
///
/// An exception that escapes a job is captured and rethrown from wait()/parallelFor().
/// If nobody waits on the job, the exception is logged as an error instead.
///
/// Threading: call parallelFor, submit, wait and waitAll only from the thread that
/// created the JobSystem or from inside a job. Any other thread is a programmer error
/// and asserts; in builds without assertions the work then runs on the calling thread.

#include "cordite/core/Base.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace cordite {

/// Identifies a job submitted with JobSystem::submit. Copyable; an empty handle is valid
/// to wait on and completes immediately.
class JobHandle {
public:
    JobHandle() = default;

    [[nodiscard]] bool valid() const { return m_state != nullptr; }

private:
    friend class JobSystem;
    struct State;
    std::shared_ptr<State> m_state;
};

class JobSystem {
public:
    using RangeFunction = std::function<void(uint32_t begin, uint32_t end)>;

    /// Creates the pool. `threadCount` includes the calling thread; 0 means one thread
    /// per hardware thread.
    explicit JobSystem(uint32_t threadCount = 0);
    ~JobSystem();
    CORDITE_NON_MOVABLE(JobSystem);

    /// Number of threads that execute jobs, including the thread that owns the JobSystem.
    [[nodiscard]] uint32_t threadCount() const;

    /// Splits [0, count) into ranges and runs `body` on them in parallel. Blocks until
    /// every index has been visited exactly once. `minBatchSize` is the smallest amount
    /// of work worth scheduling on its own; it is a hint, and remainder ranges may be
    /// shorter. `body` must be safe to call from several threads at once.
    void parallelFor(uint32_t count, uint32_t minBatchSize, const RangeFunction& body);

    /// Schedules a job and returns immediately.
    JobHandle submit(std::function<void()> job);

    /// Blocks until the job has finished, running other jobs meanwhile.
    void wait(const JobHandle& handle);

    /// Blocks until every submitted job has finished.
    void waitAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
