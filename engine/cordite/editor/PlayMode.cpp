#include "cordite/editor/PlayMode.h"

namespace cordite {

std::string_view nameOf(PlayState state) {
    switch (state) {
        case PlayState::Stopped:
            return "stopped";
        case PlayState::Playing:
            return "playing";
        case PlayState::Paused:
            return "paused";
    }
    return "unknown";
}

PlayMode::PlayMode(Engine& engine, EditorContext& context) : m_engine(engine), m_context(context) {}

Status PlayMode::start() {
    if (m_state != PlayState::Stopped) {
        return makeError(ErrorCode::InvalidState, "the scene is being played already");
    }
    CORDITE_TRY(m_context.beginPlay());
    m_state = PlayState::Playing;
    m_steps = 0;
    m_playedFrames = 0;
    return {};
}

Status PlayMode::stop() {
    if (m_state == PlayState::Stopped) {
        return makeError(ErrorCode::InvalidState, "the scene is not being played");
    }
    // What runs ends first, while the world it ran in is still there.
    CORDITE_TRY(m_engine.endScene());
    m_state = PlayState::Stopped;
    m_steps = 0;
    return m_context.endPlay();
}

Status PlayMode::pause() {
    if (m_state != PlayState::Playing) {
        return makeError(ErrorCode::InvalidState, "there is nothing to pause: the scene is {}", nameOf(m_state));
    }
    m_state = PlayState::Paused;
    return {};
}

Status PlayMode::resume() {
    if (m_state != PlayState::Paused) {
        return makeError(ErrorCode::InvalidState, "there is nothing to resume: the scene is {}", nameOf(m_state));
    }
    m_state = PlayState::Playing;
    m_steps = 0;
    return {};
}

Status PlayMode::step(uint32_t frames) {
    if (m_state != PlayState::Paused) {
        return makeError(ErrorCode::InvalidState,
                         "only a game that is paused is played frame by frame; the scene is {}", nameOf(m_state));
    }
    if (frames == 0) {
        return makeError(ErrorCode::InvalidArgument, "a step is at least one frame");
    }
    m_steps = frames;
    return {};
}

WorldClock PlayMode::clock() const {
    const bool passes = m_state == PlayState::Playing || (m_state == PlayState::Paused && m_steps > 0);
    return passes ? WorldClock::Running : WorldClock::Stopped;
}

void PlayMode::frameEnded() {
    if (clock() == WorldClock::Running) {
        ++m_playedFrames;
    }
    if (m_steps > 0) {
        --m_steps;
    }
}

} // namespace cordite
