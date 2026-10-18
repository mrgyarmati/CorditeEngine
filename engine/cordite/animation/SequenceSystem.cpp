#include "cordite/animation/SequenceSystem.h"

#include "cordite/animation/Pose.h"
#include "cordite/core/Log.h"
#include "cordite/core/Reflection.h"
#include "cordite/math/Transform.h"
#include "cordite/world/Components.h"
#include "cordite/world/EntityPath.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cordite {
namespace {

/// The value of the last key at or before `time`. Whole numbers, and yes or no, step from
/// key to key whatever their track says: a light's channels must not run through every
/// number between two keys.
Vec4 steppedValue(const Curve<Vec4>& curve, float time) {
    const std::span<const Keyframe<Vec4>> keys = curve.keys();
    if (keys.empty()) {
        return Vec4(0.0f);
    }
    const auto after = std::ranges::upper_bound(keys, time, {}, &Keyframe<Vec4>::time);
    return after == keys.begin() ? keys.front().value : (after - 1)->value;
}

/// Whether values of `kind` can be written into a property of `type`.
bool fits(SequenceValueKind kind, PropertyType type) {
    switch (kind) {
        case SequenceValueKind::Number:
            return type == PropertyType::Float || type == PropertyType::Int;
        case SequenceValueKind::Vector2:
            return type == PropertyType::Vec2;
        case SequenceValueKind::Vector3:
            return type == PropertyType::Vec3;
        case SequenceValueKind::Vector4:
            return type == PropertyType::Vec4;
        case SequenceValueKind::Boolean:
            return type == PropertyType::Bool;
    }
    return false;
}

/// How many numbers a value of `kind` is: one for yes or no.
int numbersOf(SequenceValueKind kind) {
    switch (kind) {
        case SequenceValueKind::Vector2:
            return 2;
        case SequenceValueKind::Vector3:
            return 3;
        case SequenceValueKind::Vector4:
            return 4;
        case SequenceValueKind::Number:
        case SequenceValueKind::Boolean:
            return 1;
    }
    return 1;
}

/// A number brought into the range of a property: a smooth track overshoots its keys.
double withinRange(double value, const PropertyAttributes& attributes) {
    if (attributes.minimum) {
        value = std::max(value, *attributes.minimum);
    }
    if (attributes.maximum) {
        value = std::min(value, *attributes.maximum);
    }
    return value;
}

/// What a track gives a property at a time, as an edit gives it.
Json valueAt(const SequencePropertyTrack& track, const PropertyInfo& property, float time) {
    const PropertyAttributes& attributes = property.attributes();
    if (track.kind == SequenceValueKind::Boolean) {
        return steppedValue(track.values, time).x >= 0.5f;
    }
    if (property.type() == PropertyType::Int) {
        return static_cast<int64_t>(std::llround(withinRange(steppedValue(track.values, time).x, attributes)));
    }
    const Vec4 value = track.values.evaluate(time);
    if (track.kind == SequenceValueKind::Number) {
        return withinRange(value.x, attributes);
    }
    Json numbers = Json::array();
    for (int component = 0; component < numbersOf(track.kind); ++component) {
        numbers.push_back(withinRange(value[component], attributes));
    }
    return numbers;
}

/// Moves the entity a transform track names as the track says at `time`.
Status moveEntity(World& world, Entity player, const SequenceTransformTrack& track, float time) {
    CORDITE_TRY_ASSIGN(const Entity target, findEntityByPath(world, track.target, player));
    Transform& transform = world.transform(target);
    if (!track.positions.empty()) {
        transform.position = track.positions.evaluate(time);
    }
    if (!track.rotations.empty()) {
        transform.rotation = sampleRotation(track.rotations, time);
    }
    if (!track.scales.empty()) {
        transform.scale = track.scales.evaluate(time);
    }
    if (track.lookAt.empty()) {
        return {};
    }
    CORDITE_TRY_ASSIGN(const Entity subject, findEntityByPath(world, track.lookAt, player));
    // Where the two are now, with what each hangs under as it is at this moment: the
    // world matrices are those of the frame before, and the entity has just been moved.
    const Entity parent = world.parent(target);
    const Mat4 parentToWorld = parent.isNull() ? Mat4(1.0f) : world.computeWorldMatrix(parent);
    const Vec3 eye = Vec3(parentToWorld * Vec4(transform.position, 1.0f));
    const Vec3 direction = Vec3(world.computeWorldMatrix(subject)[3]) - eye;
    // Where the two are one, there is nowhere to look: the entity stays turned as it is.
    if (glm::dot(direction, direction) > kEpsilon * kEpsilon) {
        const Quat facing = lookRotation(direction, kWorldUp);
        transform.rotation = normalizeRotation(glm::inverse(Transform::fromMatrix(parentToWorld).rotation) * facing);
    }
    return {};
}

/// Gives the property a property track names what the track says at `time`, as the
/// editor would: through the reflection, checked.
Status setProperty(World& world, const ComponentRegistry& components, Entity player, const SequencePropertyTrack& track,
                   float time) {
    CORDITE_TRY_ASSIGN(const Entity target, findEntityByPath(world, track.target, player));
    const ComponentDescriptor* descriptor = components.find(track.component);
    if (descriptor == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no component named '{}'", track.component);
    }
    void* instance = descriptor->get(world.registry(), target.handle);
    if (instance == nullptr) {
        return makeError(ErrorCode::NotFound, "'{}' has no {}", world.name(target), track.component);
    }
    const PropertyInfo* property = descriptor->type.findProperty(track.property);
    if (property == nullptr) {
        return makeError(ErrorCode::NotFound, "{} has no property '{}'", track.component, track.property);
    }
    if (!fits(track.kind, property->type())) {
        return makeError(ErrorCode::InvalidArgument, "{}.{} holds a {}, which the values of the track are not",
                         track.component, track.property, nameOf(property->type()));
    }
    Json edit = Json::object();
    edit[track.property] = valueAt(track, *property, time);
    CORDITE_TRY_ASSIGN(const PropertyPatch patch, descriptor->type.parseEdit(edit));
    patch.applyTo(instance);
    return {};
}

} // namespace

void SequenceSystem::clear() {
    m_sequences.clear();
    m_reported.clear();
    m_events.clear();
}

std::shared_ptr<const SequenceData> SequenceSystem::sequenceFor(const Uuid& id, AssetManager& assets) {
    if (const auto found = m_sequences.find(id); found != m_sequences.end()) {
        return found->second;
    }
    std::shared_ptr<const SequenceData> sequence;
    if (Result<std::shared_ptr<const SequenceData>> loaded = assets.loadSequence(id)) {
        sequence = std::move(*loaded);
    } else {
        CORDITE_LOG_ERROR("Sequence", "a sequence player cannot play the asset {}: {}", id.toString(),
                          loaded.error().message);
    }
    // Kept also when there is nothing to keep, so that it is not loaded again every frame.
    return m_sequences.emplace(id, std::move(sequence)).first->second;
}

void SequenceSystem::report(const World& world, Entity entity, const std::string& what, const Error& error) {
    if (m_reported.insert(fmt::format("{} {} {}", world.id(entity).toString(), what, error.message)).second) {
        CORDITE_LOG_ERROR("Sequence", "the sequence of '{}' cannot play its {}: {}", world.name(entity), what,
                          error.message);
    }
}

void SequenceSystem::pass(Entity entity, const SequenceData& sequence, float begin, float end, bool toEnd) {
    const auto first = std::ranges::lower_bound(sequence.events, begin, {}, &SequenceEventKey::time);
    for (auto event = first; event != sequence.events.end(); ++event) {
        if (event->time > end || (!toEnd && event->time == end)) {
            break;
        }
        m_events.push_back({.player = entity, .name = event->name});
    }
}

float SequenceSystem::advance(Entity entity, SequencePlayerComponent& player, const SequenceData& sequence,
                              float step) {
    const float duration = sequence.duration;
    const float from = std::isfinite(player.time) ? std::clamp(player.time, 0.0f, duration) : 0.0f;
    const float speed = std::isfinite(player.speed) ? std::max(player.speed, 0.0f) : 0.0f;
    // Further than a sequence is long is as far as that: it is passed once at the most.
    const float moved = std::min(step * speed, duration);
    if (!player.loop) {
        if (from + moved >= duration) {
            pass(entity, sequence, from, duration, true);
            player.time = duration;
            player.playing = false;
            return duration;
        }
        pass(entity, sequence, from, from + moved, false);
        player.time = from + moved;
        return player.time;
    }
    if (from + moved < duration) {
        pass(entity, sequence, from, from + moved, false);
        player.time = from + moved;
        return player.time;
    }
    // Round: to the end, and on from the beginning - as far as where it set out from at
    // the most, since it moved by a duration at the most: a long frame passes every event
    // once.
    pass(entity, sequence, from, duration, true);
    const float wrapped = from + moved - duration;
    pass(entity, sequence, 0.0f, wrapped, false);
    player.time = wrapped;
    return wrapped;
}

uint32_t SequenceSystem::play(World& world, const ComponentRegistry& components, Entity entity,
                              const SequenceData& sequence, float time) {
    uint32_t played = 0;
    for (size_t index = 0; index < sequence.transforms.size(); ++index) {
        if (const Status moved = moveEntity(world, entity, sequence.transforms[index], time)) {
            ++played;
        } else {
            report(world, entity, fmt::format("transform track {}", index), moved.error());
        }
    }
    for (size_t index = 0; index < sequence.properties.size(); ++index) {
        if (const Status set = setProperty(world, components, entity, sequence.properties[index], time)) {
            ++played;
        } else {
            report(world, entity, fmt::format("property track {}", index), set.error());
        }
    }
    return played;
}

Uuid SequenceSystem::cutAt(const World& world, Entity entity, const SequenceData& sequence, float time) {
    const auto after = std::ranges::upper_bound(sequence.cuts, time, {}, &SequenceCut::time);
    if (after == sequence.cuts.begin() || !(after - 1)->camera) {
        return {};
    }
    const Result<Entity> camera = findEntityByPath(world, *(after - 1)->camera, entity);
    if (!camera) {
        report(world, entity, "camera track", camera.error());
        return {};
    }
    if (!world.has<CameraComponent>(*camera)) {
        report(
            world, entity, "camera track",
            Error{ErrorCode::InvalidArgument, fmt::format("'{}' has no Camera to see through", world.name(*camera))});
        return {};
    }
    return world.id(*camera);
}

SequenceStatistics SequenceSystem::update(World& world, const ComponentRegistry& components, AssetManager& assets,
                                          float deltaSeconds) {
    if (const uint64_t revision = assets.database().revision(); revision != m_assetRevision) {
        clear();
        m_assetRevision = revision;
    }
    m_events.clear();
    const float step = std::isfinite(deltaSeconds) ? std::max(deltaSeconds, 0.0f) : 0.0f;

    // In the order of the hierarchy: of two sequences that set the same thing the later
    // has the last word, and events are told in that order.
    m_players.clear();
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (world.has<SequencePlayerComponent>(entity)) {
            m_players.push_back(entity);
        }
    });

    SequenceStatistics statistics;
    for (const Entity entity : m_players) {
        auto* player = world.tryGet<SequencePlayerComponent>(entity);
        if (player == nullptr) {
            continue;
        }
        std::shared_ptr<const SequenceData> sequence;
        if (player->playing && !player->sequence.isNil() && world.isEnabledInHierarchy(entity)) {
            sequence = sequenceFor(player->sequence, assets);
        }
        if (!sequence) {
            // What does not play cuts to nothing.
            if (!player->camera.isNil()) {
                player->camera = Uuid{};
            }
            continue;
        }
        ++statistics.players;
        const float time = advance(entity, *player, *sequence, step);
        const bool playing = player->playing;
        statistics.tracks += play(world, components, entity, *sequence, time);
        // Asked for again: a track may have written to this very component.
        player = world.tryGet<SequencePlayerComponent>(entity);
        if (player != nullptr) {
            player->camera = playing ? cutAt(world, entity, *sequence, time) : Uuid{};
        }
    }
    return statistics;
}

} // namespace cordite
