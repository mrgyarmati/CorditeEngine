#pragma once

/// @file
/// Keyframed curves for animating values over time: light intensity, camera paths,
/// post-process parameters, color gradients.
///
/// @code
/// FloatCurve flicker;
/// flicker.addKey(0.0f, 1.0f);
/// flicker.addKey(0.1f, 0.2f, Interpolation::Step);
/// flicker.addKey(0.15f, 1.0f);
/// flicker.setWrapMode(WrapMode::Loop);
/// light.intensity = baseIntensity * flicker.evaluate(timeSeconds);
/// @endcode

#include "cordite/math/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace cordite {

/// How a curve moves from a key to the next one.
enum class Interpolation : uint8_t {
    /// Holds the value of the key until the next key.
    Step,
    /// Straight line to the next key.
    Linear,
    /// Cubic Hermite spline using the tangents of the two keys.
    Cubic,
};

/// What a curve returns for times outside its keys.
enum class WrapMode : uint8_t {
    /// The value of the first or last key.
    Clamp,
    /// Repeats from the start.
    Loop,
    /// Plays forward, then backward, repeatedly.
    PingPong,
};

template <typename T>
struct Keyframe {
    float time = 0.0f;
    T value{};
    /// Slope (value per second) arriving at and leaving the key. Used by Cubic segments.
    T inTangent{};
    T outTangent{};
    /// Interpolation of the segment that starts at this key.
    Interpolation interpolation = Interpolation::Linear;

    bool operator==(const Keyframe&) const = default;
};

/// A curve through keyframes of type T, where T supports addition and scaling by float
/// (float, Vec2, Vec3, Vec4).
template <typename T>
class Curve {
public:
    /// Inserts a key, keeping keys ordered by time. A key at an existing time replaces it.
    void addKey(const Keyframe<T>& key) {
        const auto position =
            std::lower_bound(m_keys.begin(), m_keys.end(), key.time,
                             [](const Keyframe<T>& existing, float time) { return existing.time < time; });
        if (position != m_keys.end() && position->time == key.time) {
            *position = key;
        } else {
            m_keys.insert(position, key);
        }
    }

    void addKey(float time, const T& value, Interpolation interpolation = Interpolation::Linear) {
        Keyframe<T> key;
        key.time = time;
        key.value = value;
        key.interpolation = interpolation;
        addKey(key);
    }

    /// Removes the key at `index`. Returns false when the index is out of range.
    bool removeKey(size_t index) {
        if (index >= m_keys.size()) {
            return false;
        }
        m_keys.erase(m_keys.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    void clear() { m_keys.clear(); }

    [[nodiscard]] std::span<const Keyframe<T>> keys() const { return m_keys; }

    [[nodiscard]] bool empty() const { return m_keys.empty(); }

    [[nodiscard]] float startTime() const { return m_keys.empty() ? 0.0f : m_keys.front().time; }

    [[nodiscard]] float endTime() const { return m_keys.empty() ? 0.0f : m_keys.back().time; }

    [[nodiscard]] float duration() const { return endTime() - startTime(); }

    [[nodiscard]] WrapMode wrapMode() const { return m_wrapMode; }

    void setWrapMode(WrapMode mode) { m_wrapMode = mode; }

    /// Sets the tangents of every key to the Catmull-Rom slope through its neighbours,
    /// which gives a smooth curve through all Cubic keys.
    void computeSmoothTangents() {
        for (size_t index = 0; index < m_keys.size(); ++index) {
            const Keyframe<T>& previous = m_keys[index > 0 ? index - 1 : index];
            const Keyframe<T>& next = m_keys[index + 1 < m_keys.size() ? index + 1 : index];
            const float span = next.time - previous.time;
            const T slope = span > 0.0f ? (next.value - previous.value) * (1.0f / span) : T{};
            m_keys[index].inTangent = slope;
            m_keys[index].outTangent = slope;
        }
    }

    /// Returns the value at `time`. An empty curve returns a value-initialized T, and a
    /// NaN time returns the value of the first key.
    [[nodiscard]] T evaluate(float time) const {
        if (m_keys.empty()) {
            return T{};
        }
        if (m_keys.size() == 1) {
            return m_keys.front().value;
        }

        // Written as a negated comparison so that a NaN time takes this branch instead
        // of falling through to the key search with an unordered value.
        const float wrapped = wrapTime(time);
        if (!(wrapped > m_keys.front().time)) {
            return m_keys.front().value;
        }
        if (wrapped >= m_keys.back().time) {
            return m_keys.back().value;
        }

        // First key strictly after the sample time; the segment starts at the key before it.
        const auto after =
            std::upper_bound(m_keys.begin(), m_keys.end(), wrapped,
                             [](float sampleTime, const Keyframe<T>& key) { return sampleTime < key.time; });
        const Keyframe<T>& from = *(after - 1);
        const Keyframe<T>& to = *after;

        const float segment = to.time - from.time;
        const float s = (wrapped - from.time) / segment;

        switch (from.interpolation) {
            case Interpolation::Step:
                return from.value;
            case Interpolation::Linear:
                return from.value * (1.0f - s) + to.value * s;
            case Interpolation::Cubic: {
                const float s2 = s * s;
                const float s3 = s2 * s;
                const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
                const float h10 = s3 - 2.0f * s2 + s;
                const float h01 = -2.0f * s3 + 3.0f * s2;
                const float h11 = s3 - s2;
                return from.value * h00 + from.outTangent * (h10 * segment) + to.value * h01 +
                       to.inTangent * (h11 * segment);
            }
        }
        return from.value;
    }

private:
    [[nodiscard]] float wrapTime(float time) const {
        const float start = startTime();
        const float length = duration();
        if (m_wrapMode == WrapMode::Clamp || length <= 0.0f) {
            return time;
        }

        const float offset = time - start;
        if (m_wrapMode == WrapMode::Loop) {
            return start + (offset - length * std::floor(offset / length));
        }

        // PingPong: a period of twice the length, mirrored in its second half.
        const float period = 2.0f * length;
        const float phase = offset - period * std::floor(offset / period);
        return start + (phase <= length ? phase : period - phase);
    }

    std::vector<Keyframe<T>> m_keys;
    WrapMode m_wrapMode = WrapMode::Clamp;

public:
    /// Two curves are equal when they have the same keys and wrap the same way.
    bool operator==(const Curve&) const = default;
};

using FloatCurve = Curve<float>;
using Vec3Curve = Curve<Vec3>;

/// A color gradient over time. Keys hold linear RGBA.
using ColorCurve = Curve<Vec4>;

} // namespace cordite
