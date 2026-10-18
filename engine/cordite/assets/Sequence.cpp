#include "cordite/assets/Sequence.h"

#include "cordite/core/Document.h"
#include "cordite/math/MathJson.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <span>
#include <utility>

namespace cordite {
namespace {

constexpr DocumentFormat kSequenceFormat{.name = "cordite.sequence", .version = 1, .kind = "sequence"};

constexpr std::array<std::string_view, 4> kSequenceMembers = {"format", "version", "duration", "tracks"};
constexpr std::array<std::string_view, 5> kTransformMembers = {"kind", "target", "interpolation", "lookAt", "keys"};
constexpr std::array<std::string_view, 4> kTransformKeyMembers = {"time", "position", "rotation", "scale"};
constexpr std::array<std::string_view, 6> kPropertyMembers = {"kind",     "target",        "component",
                                                              "property", "interpolation", "keys"};
constexpr std::array<std::string_view, 2> kPropertyKeyMembers = {"time", "value"};
constexpr std::array<std::string_view, 2> kListMembers = {"kind", "keys"};
constexpr std::array<std::string_view, 2> kCutKeyMembers = {"time", "camera"};
constexpr std::array<std::string_view, 2> kEventKeyMembers = {"time", "name"};

/// A key of a track: its time, and the object it is.
struct Key {
    float time = 0.0f;
    const Json* object = nullptr;
};

Error mistake(std::string message) {
    return Error{ErrorCode::ParseError, std::move(message)};
}

Result<float> readNumber(const Json& value, std::string_view what) {
    if (!value.is_number()) {
        return makeError(ErrorCode::ParseError, "{} is not a number", what);
    }
    const auto number = static_cast<float>(value.get<double>());
    if (!std::isfinite(number)) {
        return makeError(ErrorCode::ParseError, "{} is not a number a float can hold", what);
    }
    return number;
}

Result<std::string> readText(const Json& value, std::string_view what) {
    if (!value.is_string()) {
        return makeError(ErrorCode::ParseError, "{} is not text", what);
    }
    std::string text = value.get<std::string>();
    if (text.size() > kMaxSequenceText) {
        return makeError(ErrorCode::ParseError, "{} is longer than {} bytes", what, kMaxSequenceText);
    }
    return text;
}

/// A member that must be there, and be text.
Result<std::string> readMemberText(const Json& object, std::string_view member) {
    const auto found = object.find(member);
    if (found == object.end()) {
        return makeError(ErrorCode::ParseError, "it has no '{}'", member);
    }
    return readText(*found, fmt::format("'{}'", member));
}

Result<SequenceInterpolation> readInterpolation(const Json& track) {
    const auto found = track.find("interpolation");
    if (found == track.end()) {
        return SequenceInterpolation::Linear;
    }
    CORDITE_TRY_ASSIGN(const std::string name, readText(*found, "'interpolation'"));
    for (const SequenceInterpolation interpolation : std::initializer_list<SequenceInterpolation>{
             SequenceInterpolation::Step, SequenceInterpolation::Linear, SequenceInterpolation::Smooth}) {
        if (name == nameOf(interpolation)) {
            return interpolation;
        }
    }
    return makeError(ErrorCode::ParseError, "'interpolation' is '{}', which is none of step, linear and smooth", name);
}

Interpolation curveInterpolation(SequenceInterpolation interpolation) {
    switch (interpolation) {
        case SequenceInterpolation::Step:
            return Interpolation::Step;
        case SequenceInterpolation::Linear:
            return Interpolation::Linear;
        case SequenceInterpolation::Smooth:
            return Interpolation::Cubic;
    }
    return Interpolation::Linear;
}

/// The keys of a track: objects of the members `allowed`, each with a time within the
/// sequence that comes after that of the key before it.
Result<std::vector<Key>> readKeys(const Json& track, float duration, std::span<const std::string_view> allowed) {
    const auto found = track.find("keys");
    if (found == track.end() || !found->is_array() || found->empty()) {
        return makeError(ErrorCode::ParseError, "'keys' is not a list of at least one key");
    }
    if (found->size() > kMaxSequenceKeys) {
        return makeError(ErrorCode::ParseError, "it has {} keys, more than {}", found->size(), kMaxSequenceKeys);
    }
    std::vector<Key> keys;
    keys.reserve(found->size());
    for (size_t index = 0; index < found->size(); ++index) {
        const Json& key = (*found)[index];
        const auto failed = [&](const Error& error) -> Result<std::vector<Key>> {
            return tl::unexpected(error.withContext(fmt::format("key {}", index)));
        };
        if (!key.is_object()) {
            return failed(mistake("it is not an object"));
        }
        if (const Status known = rejectUnknownMembers(key, allowed); !known) {
            return failed(known.error());
        }
        const auto time = key.find("time");
        if (time == key.end()) {
            return failed(mistake("it has no 'time'"));
        }
        const Result<float> seconds = readNumber(*time, "'time'");
        if (!seconds) {
            return failed(seconds.error());
        }
        if (*seconds < 0.0f || *seconds > duration) {
            return failed(mistake(fmt::format("'time' {} is outside the sequence, from 0 to {}", *seconds, duration)));
        }
        if (!keys.empty() && !(*seconds > keys.back().time)) {
            return failed(mistake(fmt::format("'time' {} does not come after {}, the time of the key before", *seconds,
                                              keys.back().time)));
        }
        keys.push_back({*seconds, &key});
    }
    return keys;
}

/// Calls `read` with every key, adding which key it was to what fails.
template <typename Read>
Status readEachKey(std::span<const Key> keys, Read read) {
    for (size_t index = 0; index < keys.size(); ++index) {
        if (const Status done = read(keys[index]); !done) {
            return tl::unexpected(done.error().withContext(fmt::format("key {}", index)));
        }
    }
    return {};
}

Result<Vec3> readVector(const Json& value, std::string_view what) {
    Vec3 vector(0.0f);
    if (const Status read = fromJson(value, vector); !read) {
        return tl::unexpected(read.error().withContext(fmt::format("'{}'", what)));
    }
    return vector;
}

/// A rotation, [x, y, z, w], brought to length one.
Result<Quat> readRotation(const Json& value) {
    Vec4 numbers(0.0f);
    if (const Status read = fromJson(value, numbers); !read) {
        return tl::unexpected(read.error().withContext("'rotation'"));
    }
    const Quat rotation(numbers.w, numbers.x, numbers.y, numbers.z);
    if (!isRotation(rotation)) {
        return makeError(ErrorCode::ParseError, "'rotation' is not a rotation: four numbers of a length of more than "
                                                "nothing, given as [x, y, z, w]");
    }
    return normalizeRotation(rotation);
}

Status readTransform(const Json& track, float duration, SequenceData& sequence) {
    CORDITE_TRY(rejectUnknownMembers(track, kTransformMembers));
    SequenceTransformTrack transform;
    CORDITE_TRY_ASSIGN(transform.target, readMemberText(track, "target"));
    CORDITE_TRY_ASSIGN(transform.interpolation, readInterpolation(track));
    if (const auto lookAt = track.find("lookAt"); lookAt != track.end()) {
        CORDITE_TRY_ASSIGN(transform.lookAt, readText(*lookAt, "'lookAt'"));
        if (transform.lookAt.empty()) {
            return makeError(ErrorCode::ParseError, "'lookAt' names nothing: leave it out instead");
        }
    }
    CORDITE_TRY_ASSIGN(const std::vector<Key> keys, readKeys(track, duration, kTransformKeyMembers));
    const Interpolation interpolation = curveInterpolation(transform.interpolation);
    std::optional<Quat> before;
    CORDITE_TRY(readEachKey(keys, [&](const Key& key) -> Status {
        const Json& object = *key.object;
        if (!object.contains("position") && !object.contains("rotation") && !object.contains("scale")) {
            return makeError(ErrorCode::ParseError, "it has none of 'position', 'rotation' and 'scale'");
        }
        if (const auto position = object.find("position"); position != object.end()) {
            CORDITE_TRY_ASSIGN(const Vec3 value, readVector(*position, "position"));
            transform.positions.addKey(key.time, value, interpolation);
        }
        if (const auto rotation = object.find("rotation"); rotation != object.end()) {
            CORDITE_TRY_ASSIGN(Quat value, readRotation(*rotation));
            // The same way round as the key before: the other way round, the arc between
            // the two would be the long one.
            if (before && glm::dot(*before, value) < 0.0f) {
                value = -value;
            }
            before = value;
            transform.rotations.addKey(key.time, Vec4(value.x, value.y, value.z, value.w), interpolation);
        }
        if (const auto scale = object.find("scale"); scale != object.end()) {
            CORDITE_TRY_ASSIGN(const Vec3 value, readVector(*scale, "scale"));
            transform.scales.addKey(key.time, value, interpolation);
        }
        return {};
    }));
    if (!transform.lookAt.empty() && !transform.rotations.empty()) {
        return makeError(ErrorCode::ParseError,
                         "it turns its entity by 'rotation' keys and to face 'lookAt' both: give it one of the two");
    }
    if (transform.interpolation == SequenceInterpolation::Smooth) {
        transform.positions.computeSmoothTangents();
        transform.rotations.computeSmoothTangents();
        transform.scales.computeSmoothTangents();
    }
    sequence.transforms.push_back(std::move(transform));
    return {};
}

/// The value of a key of a property track, and its kind.
Result<std::pair<SequenceValueKind, Vec4>> readValue(const Json& value) {
    if (value.is_boolean()) {
        return std::pair{SequenceValueKind::Boolean, Vec4(value.get<bool>() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f)};
    }
    if (value.is_number()) {
        CORDITE_TRY_ASSIGN(const float number, readNumber(value, "'value'"));
        return std::pair{SequenceValueKind::Number, Vec4(number, 0.0f, 0.0f, 0.0f)};
    }
    if (value.is_array() && value.size() >= 2 && value.size() <= 4) {
        Vec4 numbers(0.0f);
        for (size_t index = 0; index < value.size(); ++index) {
            CORDITE_TRY_ASSIGN(numbers[static_cast<int>(index)], readNumber(value[index], "a number of 'value'"));
        }
        // Two, three or four numbers, in that order.
        constexpr std::array<SequenceValueKind, 3> kVectors = {SequenceValueKind::Vector2, SequenceValueKind::Vector3,
                                                               SequenceValueKind::Vector4};
        return std::pair{kVectors.at(value.size() - 2), numbers};
    }
    return makeError(ErrorCode::ParseError, "'value' is none of a number, two to four numbers, and true or false");
}

std::string_view describe(SequenceValueKind kind) {
    switch (kind) {
        case SequenceValueKind::Number:
            return "a number";
        case SequenceValueKind::Vector2:
            return "two numbers";
        case SequenceValueKind::Vector3:
            return "three numbers";
        case SequenceValueKind::Vector4:
            return "four numbers";
        case SequenceValueKind::Boolean:
            return "true or false";
    }
    return "a value";
}

Status readProperty(const Json& track, float duration, SequenceData& sequence) {
    CORDITE_TRY(rejectUnknownMembers(track, kPropertyMembers));
    SequencePropertyTrack property;
    CORDITE_TRY_ASSIGN(property.target, readMemberText(track, "target"));
    CORDITE_TRY_ASSIGN(property.component, readMemberText(track, "component"));
    CORDITE_TRY_ASSIGN(property.property, readMemberText(track, "property"));
    if (property.component.empty() || property.property.empty()) {
        return makeError(ErrorCode::ParseError, "'component' and 'property' must name something");
    }
    CORDITE_TRY_ASSIGN(property.interpolation, readInterpolation(track));
    CORDITE_TRY_ASSIGN(const std::vector<Key> keys, readKeys(track, duration, kPropertyKeyMembers));
    CORDITE_TRY(readEachKey(keys, [&](const Key& key) -> Status {
        const auto value = key.object->find("value");
        if (value == key.object->end()) {
            return makeError(ErrorCode::ParseError, "it has no 'value'");
        }
        CORDITE_TRY_ASSIGN(const auto read, readValue(*value));
        if (property.values.empty()) {
            property.kind = read.first;
        } else if (read.first != property.kind) {
            return makeError(ErrorCode::ParseError, "'value' is {} where the first key of the track has {}",
                             describe(read.first), describe(property.kind));
        }
        // Yes or no has nothing between.
        const Interpolation interpolation = property.kind == SequenceValueKind::Boolean
                                                ? Interpolation::Step
                                                : curveInterpolation(property.interpolation);
        property.values.addKey(key.time, read.second, interpolation);
        return {};
    }));
    if (property.interpolation == SequenceInterpolation::Smooth && property.kind != SequenceValueKind::Boolean) {
        property.values.computeSmoothTangents();
    }
    sequence.properties.push_back(std::move(property));
    return {};
}

Status readCuts(const Json& track, float duration, SequenceData& sequence) {
    CORDITE_TRY(rejectUnknownMembers(track, kListMembers));
    if (!sequence.cuts.empty()) {
        return makeError(ErrorCode::ParseError, "it is the second camera track: a sequence has one at the most");
    }
    CORDITE_TRY_ASSIGN(const std::vector<Key> keys, readKeys(track, duration, kCutKeyMembers));
    std::vector<SequenceCut> cuts;
    CORDITE_TRY(readEachKey(keys, [&](const Key& key) -> Status {
        const auto camera = key.object->find("camera");
        if (camera == key.object->end()) {
            return makeError(ErrorCode::ParseError, "it has no 'camera': a path, or null for the game's own camera");
        }
        SequenceCut cut{.time = key.time};
        if (!camera->is_null()) {
            CORDITE_TRY_ASSIGN(cut.camera, readText(*camera, "'camera'"));
        }
        cuts.push_back(std::move(cut));
        return {};
    }));
    sequence.cuts = std::move(cuts);
    return {};
}

Status readEvents(const Json& track, float duration, SequenceData& sequence) {
    CORDITE_TRY(rejectUnknownMembers(track, kListMembers));
    CORDITE_TRY_ASSIGN(const std::vector<Key> keys, readKeys(track, duration, kEventKeyMembers));
    return readEachKey(keys, [&](const Key& key) -> Status {
        CORDITE_TRY_ASSIGN(std::string name, readMemberText(*key.object, "name"));
        if (name.empty()) {
            return makeError(ErrorCode::ParseError, "'name' is empty: a script would not know what it is told");
        }
        sequence.events.push_back({.time = key.time, .name = std::move(name)});
        return {};
    });
}

Status readTrack(const Json& track, float duration, SequenceData& sequence) {
    if (!track.is_object()) {
        return makeError(ErrorCode::ParseError, "it is not an object");
    }
    CORDITE_TRY_ASSIGN(const std::string kind, readMemberText(track, "kind"));
    if (kind == "transform") {
        return readTransform(track, duration, sequence);
    }
    if (kind == "property") {
        return readProperty(track, duration, sequence);
    }
    if (kind == "camera") {
        return readCuts(track, duration, sequence);
    }
    if (kind == "event") {
        return readEvents(track, duration, sequence);
    }
    return makeError(ErrorCode::ParseError, "'kind' is '{}', which is none of transform, property, camera and event",
                     kind);
}

} // namespace

std::string_view nameOf(SequenceInterpolation interpolation) {
    switch (interpolation) {
        case SequenceInterpolation::Step:
            return "step";
        case SequenceInterpolation::Linear:
            return "linear";
        case SequenceInterpolation::Smooth:
            return "smooth";
    }
    return "unknown";
}

Result<SequenceData> parseSequenceFile(const Json& document) {
    CORDITE_TRY(readDocumentHeader(document, kSequenceFormat));
    CORDITE_TRY(rejectUnknownMembers(document, kSequenceMembers));

    SequenceData sequence;
    const auto duration = document.find("duration");
    if (duration == document.end()) {
        return makeError(ErrorCode::ParseError, "the sequence has no 'duration'");
    }
    CORDITE_TRY_ASSIGN(sequence.duration, readNumber(*duration, "'duration'"));
    if (!(sequence.duration > 0.0f) || sequence.duration > kMaxSequenceDuration) {
        return makeError(ErrorCode::ParseError, "'duration' is {} seconds, which is not more than 0 and at most {}",
                         sequence.duration, kMaxSequenceDuration);
    }

    const auto tracks = document.find("tracks");
    if (tracks == document.end() || !tracks->is_array()) {
        return makeError(ErrorCode::ParseError, "'tracks' is not a list");
    }
    if (tracks->size() > kMaxSequenceTracks) {
        return makeError(ErrorCode::ParseError, "it has {} tracks, more than {}", tracks->size(), kMaxSequenceTracks);
    }
    for (size_t index = 0; index < tracks->size(); ++index) {
        if (const Status read = readTrack((*tracks)[index], sequence.duration, sequence); !read) {
            return tl::unexpected(read.error().withContext(fmt::format("track {}", index)));
        }
    }
    // In the order of their times; of two at the same time, the one the file has first.
    std::ranges::stable_sort(sequence.events, {}, &SequenceEventKey::time);
    return sequence;
}

} // namespace cordite
