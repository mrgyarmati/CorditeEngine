#include "cordite/core/Time.h"

#include "cordite/core/Assert.h"

#include <algorithm>
#include <cmath>

namespace cordite {

FixedTimestep::FixedTimestep(double stepSeconds, uint32_t maxStepsPerFrame)
    : m_stepSeconds(stepSeconds), m_maxStepsPerFrame(std::max(1u, maxStepsPerFrame)) {
    CORDITE_ASSERT(std::isfinite(stepSeconds) && stepSeconds > 0.0, "step must be positive, got {}", stepSeconds);
    if (!std::isfinite(m_stepSeconds) || m_stepSeconds <= 0.0) {
        m_stepSeconds = 1.0 / 60.0;
    }
}

uint32_t FixedTimestep::advance(double frameSeconds) {
    if (!std::isfinite(frameSeconds) || frameSeconds <= 0.0) {
        return 0;
    }

    m_accumulator += frameSeconds;

    // The epsilon absorbs floating-point drift so that feeding exactly one step's worth
    // of time reliably yields one step.
    constexpr double kEpsilon = 1e-9;
    const double available = std::floor((m_accumulator + kEpsilon) / m_stepSeconds);
    const uint32_t steps = static_cast<uint32_t>(std::min<double>(available, m_maxStepsPerFrame));

    if (available > static_cast<double>(m_maxStepsPerFrame)) {
        m_accumulator = 0.0;
    } else {
        m_accumulator = std::max(0.0, m_accumulator - static_cast<double>(steps) * m_stepSeconds);
    }
    return steps;
}

double FixedTimestep::alpha() const {
    return std::clamp(m_accumulator / m_stepSeconds, 0.0, std::nextafter(1.0, 0.0));
}

void FixedTimestep::setStepSeconds(double stepSeconds) {
    if (std::isfinite(stepSeconds) && stepSeconds > 0.0) {
        m_stepSeconds = stepSeconds;
    }
}

} // namespace cordite
