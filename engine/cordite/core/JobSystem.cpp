#include "cordite/core/JobSystem.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"

#include <TaskScheduler.h>

#include <algorithm>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// Remembers the first exception thrown by a job so the waiting thread can rethrow it.
class ExceptionSlot {
public:
    void capture() noexcept {
        const std::lock_guard lock(m_mutex);
        if (!m_exception) {
            m_exception = std::current_exception();
        }
    }

    /// Returns the captured exception, if any, and clears the slot.
    std::exception_ptr take() noexcept {
        const std::lock_guard lock(m_mutex);
        return std::exchange(m_exception, nullptr);
    }

    void rethrowIfSet() {
        if (const std::exception_ptr exception = take()) {
            std::rethrow_exception(exception);
        }
    }

private:
    std::mutex m_mutex;
    std::exception_ptr m_exception;
};

/// Logs an exception that nobody is left to receive. A failed job must never disappear
/// silently just because its handle was dropped.
void logUnhandled(const std::exception_ptr& exception) noexcept {
    if (!exception) {
        return;
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        CORDITE_LOG_ERROR("Jobs", "unhandled exception in a job that was never waited on: {}", error.what());
    } catch (...) {
        CORDITE_LOG_ERROR("Jobs", "unhandled non-standard exception in a job that was never waited on");
    }
}

class RangeTask final : public enki::ITaskSet {
public:
    RangeTask(uint32_t count, uint32_t minBatchSize, const JobSystem::RangeFunction& body)
        : enki::ITaskSet(count, std::max(1u, minBatchSize)), m_body(body) {}

    void ExecuteRange(enki::TaskSetPartition range, uint32_t /*threadIndex*/) override {
        try {
            m_body(range.start, range.end);
        } catch (...) {
            m_exception.capture();
        }
    }

    void rethrowIfFailed() { m_exception.rethrowIfSet(); }

private:
    const JobSystem::RangeFunction& m_body;
    ExceptionSlot m_exception;
};

} // namespace

struct JobHandle::State final : public enki::ITaskSet {
    explicit State(std::function<void()> function) : enki::ITaskSet(1), job(std::move(function)) {}

    void ExecuteRange(enki::TaskSetPartition /*range*/, uint32_t /*threadIndex*/) override {
        try {
            job();
        } catch (...) {
            exception.capture();
        }
    }

    std::function<void()> job;
    ExceptionSlot exception;
};

struct JobSystem::Impl {
    enki::TaskScheduler scheduler;

    // Submitted jobs are kept alive here until they complete, so that dropping a
    // JobHandle never destroys a task the scheduler still references.
    std::mutex pendingMutex;
    std::vector<std::shared_ptr<JobHandle::State>> pending;

    void releaseCompleted() {
        const std::lock_guard lock(pendingMutex);
        std::erase_if(pending, [](const std::shared_ptr<JobHandle::State>& state) {
            if (!state->GetIsComplete()) {
                return false;
            }
            // use_count() == 1 means every JobHandle is gone, so wait() can no longer
            // deliver the exception to anyone.
            if (state.use_count() == 1) {
                logUnhandled(state->exception.take());
            }
            return true;
        });
    }

    /// enkiTS may only be driven from the thread that created the scheduler or from one
    /// of its worker threads. Asserts on any other thread and returns false, so callers
    /// can fall back to something safe when assertions are compiled out.
    [[nodiscard]] bool isSchedulerThread(std::string_view operation) const {
        const bool owned = scheduler.GetThreadNum() != enki::NO_THREAD_NUM;
        CORDITE_ASSERT(owned, "JobSystem::{} called from a thread that neither owns the job system nor runs its jobs",
                       operation);
        return owned;
    }
};

JobSystem::JobSystem(uint32_t threadCount) : m_impl(std::make_unique<Impl>()) {
    enki::TaskSchedulerConfig config;
    const uint32_t total = threadCount == 0 ? hardwareThreadCount() : threadCount;
    // enkiTS counts the calling thread as one of its task threads.
    config.numTaskThreadsToCreate = std::max(1u, total) - 1;
    m_impl->scheduler.Initialize(config);
}

JobSystem::~JobSystem() {
    m_impl->scheduler.WaitforAllAndShutdown();
    for (const std::shared_ptr<JobHandle::State>& state : m_impl->pending) {
        logUnhandled(state->exception.take());
    }
}

uint32_t JobSystem::threadCount() const {
    return m_impl->scheduler.GetNumTaskThreads();
}

void JobSystem::parallelFor(uint32_t count, uint32_t minBatchSize, const RangeFunction& body) {
    if (count == 0) {
        return;
    }
    if (!m_impl->isSchedulerThread("parallelFor")) {
        body(0, count);
        return;
    }

    RangeTask task(count, minBatchSize, body);
    m_impl->scheduler.AddTaskSetToPipe(&task);
    m_impl->scheduler.WaitforTask(&task);
    task.rethrowIfFailed();
}

JobHandle JobSystem::submit(std::function<void()> job) {
    JobHandle handle;
    handle.m_state = std::make_shared<JobHandle::State>(std::move(job));

    if (!m_impl->isSchedulerThread("submit")) {
        // Run inline: the handle then refers to a job that has already finished.
        handle.m_state->ExecuteRange({0, 1}, 0);
        return handle;
    }

    m_impl->releaseCompleted();
    {
        const std::lock_guard lock(m_impl->pendingMutex);
        m_impl->pending.push_back(handle.m_state);
    }
    m_impl->scheduler.AddTaskSetToPipe(handle.m_state.get());
    return handle;
}

void JobSystem::wait(const JobHandle& handle) {
    if (!handle.valid()) {
        return;
    }
    if (m_impl->isSchedulerThread("wait")) {
        m_impl->scheduler.WaitforTask(handle.m_state.get());
    } else {
        while (!handle.m_state->GetIsComplete()) {
            std::this_thread::yield();
        }
    }
    handle.m_state->exception.rethrowIfSet();
}

void JobSystem::waitAll() {
    if (!m_impl->isSchedulerThread("waitAll")) {
        return;
    }
    m_impl->scheduler.WaitforAll();

    std::vector<std::shared_ptr<JobHandle::State>> finished;
    {
        const std::lock_guard lock(m_impl->pendingMutex);
        finished.swap(m_impl->pending);
    }

    // The caller receives the first failure; any further ones are logged.
    std::exception_ptr first;
    for (const std::shared_ptr<JobHandle::State>& state : finished) {
        const std::exception_ptr exception = state->exception.take();
        if (!first) {
            first = exception;
        } else {
            logUnhandled(exception);
        }
    }
    if (first) {
        std::rethrow_exception(first);
    }
}

} // namespace cordite
