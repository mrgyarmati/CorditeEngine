#include "cordite/renderer/GpuTimer.h"

#include "cordite/core/Assert.h"

namespace cordite {

GpuTimer::GpuTimer(nvrhi::IDevice* device, bool supported) : m_device(device) {
    CORDITE_ASSERT(device != nullptr, "a GPU timer needs a device");
    if (!supported || device == nullptr) {
        return;
    }
    for (Query& query : m_queries) {
        // A query that cannot be created stays null and is never used.
        query.timer = device->createTimerQuery();
        query.submitted = device->createEventQuery();
    }
}

void GpuTimer::collect() {
    for (Query& query : m_queries) {
        // A result is only read once the GPU has finished the whole command list. While
        // it is still at it, a driver may hand out the new start time together with the
        // end time of the measurement before, which makes a duration of nonsense.
        if (query.state != State::Pending || !m_device->pollEventQuery(query.submitted) ||
            !m_device->pollTimerQuery(query.timer)) {
            continue;
        }
        const double milliseconds = static_cast<double>(m_device->getTimerQueryTime(query.timer)) * 1000.0;
        m_device->resetTimerQuery(query.timer);
        m_device->resetEventQuery(query.submitted);
        query.state = State::Free;
        // Zero is what a driver reports whose counters have run past what the query can
        // hold; no stretch of real work takes no time at all.
        if (milliseconds > 0.0) {
            m_latest = milliseconds;
            ++m_measurementCount;
        }
    }
}

void GpuTimer::begin(nvrhi::ICommandList* commandList) {
    CORDITE_ASSERT(commandList != nullptr, "begin needs a command list");
    if (commandList == nullptr) {
        return;
    }
    if (m_current) {
        // A stretch that was begun and never ended: its command list was abandoned, and
        // so is the measurement.
        m_device->resetTimerQuery(m_queries[*m_current].timer);
        m_queries[*m_current].state = State::Free;
        m_current.reset();
    }
    for (Query& query : m_queries) {
        if (query.state == State::Ended) {
            // Its command list has been executed since: from now on the GPU can be asked
            // whether it has got that far.
            m_device->setEventQuery(query.submitted, nvrhi::CommandQueue::Graphics);
            query.state = State::Pending;
        }
    }
    collect();

    for (size_t index = 0; index < m_queries.size(); ++index) {
        Query& query = m_queries[index];
        if (query.timer != nullptr && query.submitted != nullptr && query.state == State::Free) {
            commandList->beginTimerQuery(query.timer);
            query.state = State::Measuring;
            m_current = index;
            return;
        }
    }
}

void GpuTimer::end(nvrhi::ICommandList* commandList) {
    CORDITE_ASSERT(commandList != nullptr, "end needs a command list");
    if (commandList == nullptr || !m_current) {
        return;
    }
    Query& query = m_queries[*m_current];
    commandList->endTimerQuery(query.timer);
    query.state = State::Ended;
    m_current.reset();
}

} // namespace cordite
