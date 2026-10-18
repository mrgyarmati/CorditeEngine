#pragma once

/// @file
/// Sequences: what happens in a scene over a few seconds, on a timeline - a camera that
/// flies past, a light that comes on, a cut to another camera, a moment a script is told
/// of. A sequence is an asset; a SequencePlayer component plays it on an entity
/// (animation/SequenceSystem.h), and what its tracks name is found from there.
///
/// @code
/// // assets/sequences/flyby.sequence.json:
/// //   {"format": "cordite.sequence", "version": 1, "duration": 6.0, "tracks": [
/// //       {"kind": "camera", "keys": [{"time": 0.0, "camera": "Cinema camera"}]},
/// //       {"kind": "transform", "target": "Cinema camera", "interpolation": "smooth",
/// //        "lookAt": "/Stage/Sentry",
/// //        "keys": [{"time": 0.0, "position": [6, 2, 6]}, {"time": 6.0, "position": [-4, 3, 5]}]},
/// //       {"kind": "property", "target": "/Stage/Wall/Wall light", "component": "Light",
/// //        "property": "intensity", "interpolation": "linear",
/// //        "keys": [{"time": 1.0, "value": 0.0}, {"time": 2.0, "value": 6.0}]},
/// //       {"kind": "event", "keys": [{"time": 3.0, "name": "thunder"}]}]}
/// CORDITE_TRY_ASSIGN(const SequenceData sequence, parseSequenceFile(document));
/// @endcode
///
/// A target is a path of entity names, as the editor's commands take them: from the entity
/// that plays the sequence - "" is that entity, "Rig/Camera" a child of a child - or, when
/// it begins with '/', from the roots of the scene. What a path names is the engine's to
/// find when the sequence plays; the file only says it.

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/math/Curve.h"
#include "cordite/math/Math.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// The most a sequence file may hold, in bytes: it is read whole.
inline constexpr size_t kMaxSequenceFileBytes = size_t{16} * 1024 * 1024;
/// The longest a sequence may be, in seconds: an hour.
inline constexpr float kMaxSequenceDuration = 3600.0f;
/// The most tracks a sequence may have, and keys a track.
inline constexpr size_t kMaxSequenceTracks = 256;
inline constexpr size_t kMaxSequenceKeys = 4096;
/// The longest a target path or an event name may be, in bytes.
inline constexpr size_t kMaxSequenceText = 256;

/// How a track goes from a key to the next.
enum class SequenceInterpolation : uint8_t {
    /// Holds each key until the next.
    Step,
    /// Straight to the next; a rotation along the arc between the two.
    Linear,
    /// A curve through every key without corners (Catmull-Rom).
    Smooth,
};

[[nodiscard]] std::string_view nameOf(SequenceInterpolation interpolation);

/// Where an entity is, how it is turned and how large, over time: what its Transform
/// holds, against what it hangs under. Each of the three is its own curve, of the keys
/// that have it; one that no key has is left as it is.
struct SequenceTransformTrack {
    std::string target;
    SequenceInterpolation interpolation = SequenceInterpolation::Linear;
    Curve<Vec3> positions;
    /// Rotations as four numbers [x, y, z, w], each key the same way round as the one
    /// before it (sampleRotation in animation/Pose.h reads them).
    Curve<Vec4> rotations;
    Curve<Vec3> scales;
    /// A path the entity is turned to face, all the time, instead of by `rotations`:
    /// a camera that keeps its subject in view. Empty for none.
    std::string lookAt;
};

/// What a value of a property track is: a number, two to four numbers, or yes or no.
enum class SequenceValueKind : uint8_t {
    Number,
    Vector2,
    Vector3,
    Vector4,
    Boolean,
};

/// A property of a component of an entity over time, as the reflection describes it:
/// the brightness of a light, the field of view of a camera, a color. The values of all
/// its keys are of one kind; yes or no, and whole numbers, step from key to key.
struct SequencePropertyTrack {
    std::string target;
    std::string component;
    std::string property;
    SequenceInterpolation interpolation = SequenceInterpolation::Linear;
    SequenceValueKind kind = SequenceValueKind::Number;
    /// Every value as four numbers, those a kind does not have nought; yes is 1.
    Curve<Vec4> values;
};

/// Which camera the game is seen through from a key on, until the next: a path, or
/// none - the game's own camera again.
struct SequenceCut {
    float time = 0.0f;
    std::optional<std::string> camera;

    bool operator==(const SequenceCut&) const = default;
};

/// A moment a script of the entity that plays the sequence is told of, by its name.
struct SequenceEventKey {
    float time = 0.0f;
    std::string name;

    bool operator==(const SequenceEventKey&) const = default;
};

/// What a sequence file says.
struct SequenceData {
    /// How long it is, in seconds: more than nothing, at most kMaxSequenceDuration.
    float duration = 1.0f;
    std::vector<SequenceTransformTrack> transforms;
    std::vector<SequencePropertyTrack> properties;
    /// The cuts, in the order of their times; empty for a sequence that cuts nowhere.
    std::vector<SequenceCut> cuts;
    /// The events of all its event tracks, in the order of their times - and of the
    /// file, for two at the same time.
    std::vector<SequenceEventKey> events;
};

/// Reads the document of a sequence file. Fails for a document of another kind or
/// version, a member that is not known, a track of a kind there is none of, a time
/// outside the sequence or not after the key before, a value of another kind than the
/// first of its track, a rotation that is none, more than one camera track, a track that
/// both turns its entity and has it look at something, and limits that are exceeded -
/// naming the track and the key.
[[nodiscard]] Result<SequenceData> parseSequenceFile(const Json& document);

} // namespace cordite
