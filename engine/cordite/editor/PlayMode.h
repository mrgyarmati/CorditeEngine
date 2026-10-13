#pragma once

/// @file
/// Playing the scene that is being edited, in the editor, and getting it back afterwards.
///
/// @code
/// PlayMode play(engine, context);
///
/// CORDITE_TRY(play.start());                    // between two frames
/// while (...) {
///     CORDITE_TRY_ASSIGN(const bool keepRunning, engine.beginFrame());
///     engine.updateWorld(play.clock());         // running while it plays, stopped otherwise
///     ...
///     CORDITE_TRY(engine.endFrame());
///     play.frameEnded();
/// }
/// CORDITE_TRY(play.stop());                     // the scene is what it was, to the byte
/// @endcode
///
/// An editor shows a scene without running it: no script runs, nothing falls, nothing is
/// heard. Playing is the same world with its clock running - scripts start, bodies move,
/// sources sound - until it is stopped, and then the scene is put back as it was when
/// playing began. What the game did to the world, and what was edited meanwhile, is part
/// of playing and goes with it; the history of the scene is set aside for as long and
/// comes back untouched (EditorContext::beginPlay).
///
/// Paused, the world stands still as it does in an editor, but it is still the played
/// one: step() lets single frames of it pass.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/engine/Engine.h"

#include <cstdint>
#include <string_view>

namespace cordite {

enum class PlayState : uint8_t {
    /// The scene is a document: what an editor shows and changes.
    Stopped,
    /// The scene is a game that runs.
    Playing,
    /// The scene is a game whose time stands still.
    Paused,
};

/// The lowercase name: "stopped", "playing", "paused".
std::string_view nameOf(PlayState state);

class PlayMode {
public:
    /// Both must outlive it. The context is that of the engine's world.
    PlayMode(Engine& engine, EditorContext& context);
    ~PlayMode() = default;
    CORDITE_NON_MOVABLE(PlayMode);

    [[nodiscard]] PlayState state() const { return m_state; }

    /// Begins to play the scene as it is. Fails with InvalidState when it is being
    /// played already, paused or not.
    [[nodiscard]] Status start();

    /// Ends playing: the scripts end, what the systems of the engine held for the game is
    /// forgotten, and the scene is what it was when start() was called. Fails with
    /// InvalidState when the scene is not being played, and during a frame.
    [[nodiscard]] Status stop();

    /// Holds the game where it is, and lets it go on. Each fails with InvalidState when
    /// there is nothing to hold, or to let go on.
    [[nodiscard]] Status pause();
    [[nodiscard]] Status resume();

    /// Lets time pass for `frames` frames of a game that is paused, which is paused
    /// again after them: the frames are those the caller runs next. Fails with
    /// InvalidState unless the game is paused, and with InvalidArgument for no frames.
    [[nodiscard]] Status step(uint32_t frames);

    /// Whether time passes for the world in the frame that is run next.
    [[nodiscard]] WorldClock clock() const;

    /// To be called when a frame has ended: one of the frames step() asked for is done.
    void frameEnded();

    /// How many frames the game has been played for since start(): frames in which
    /// its time passed.
    [[nodiscard]] uint64_t playedFrames() const { return m_playedFrames; }

private:
    Engine& m_engine;
    EditorContext& m_context;
    PlayState m_state = PlayState::Stopped;
    /// Frames of a paused game that are still to be run.
    uint32_t m_steps = 0;
    uint64_t m_playedFrames = 0;
};

} // namespace cordite
