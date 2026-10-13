#include "cordite/audio/AudioSystem.h"

#include "cordite/assets/Sound.h"
#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/world/Entity.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// Everything is mixed in stereo.
constexpr ma_uint32 kChannels = 2;

/// What miniaudio has to say. Its errors are not failures of the engine yet - looking for
/// a device it tries one way of playing sound after another, and says so of each that
/// is not there - so they are warnings, and the rest is for whoever is looking closely.
void logFromMiniaudio(void* /*userData*/, ma_uint32 level, const char* message) {
    std::string_view text = message == nullptr ? std::string_view() : std::string_view(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return;
    }
    if (level == MA_LOG_LEVEL_ERROR) {
        CORDITE_LOG_WARN("Audio", "miniaudio: {}", text);
    } else if (level == MA_LOG_LEVEL_WARNING) {
        CORDITE_LOG_INFO("Audio", "miniaudio: {}", text);
    } else {
        CORDITE_LOG_DEBUG("Audio", "miniaudio: {}", text);
    }
}

/// Called with everything that was mixed, on whichever thread mixed it. miniaudio adds
/// sounds up and leaves the sum as it is; what goes to a device is from -1 to 1.
void clipMix(void* /*userData*/, float* frames, ma_uint64 frameCount) {
    ma_clip_samples_f32(frames, frames, frameCount * kChannels);
}

size_t indexOf(AudioBus bus) {
    const auto index = static_cast<size_t>(bus);
    CORDITE_ASSERT(index < kAudioBuses.size(), "{} is no audio bus", index);
    return std::min(index, kAudioBuses.size() - 1);
}

/// `value` when it is a number from `low` to `high`, the nearer of the two when it is a
/// number outside, and `fallback` when it is none. Components are plain data: code can
/// put anything into one, whatever the ranges of its properties say.
float within(float value, float low, float high, float fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

/// How a voice is to be heard: what a source or a description says, made safe.
struct VoiceSettings {
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatial = false;
    Vec3 position{0.0f};
    AudioRolloff rolloff = AudioRolloff::Inverse;
    float minDistance = 1.0f;
    float maxDistance = 50.0f;

    bool operator==(const VoiceSettings&) const = default;
};

/// Something that is playing: a sound of miniaudio's, reading the samples of an asset.
struct Voice {
    Voice() = default;

    ~Voice() {
        // The sound first: it reads from the buffer, and the buffer from the samples.
        if (soundReady) {
            ma_sound_uninit(&sound);
        }
        if (bufferReady) {
            ma_audio_buffer_ref_uninit(&buffer);
        }
    }

    CORDITE_NON_MOVABLE(Voice);

    std::shared_ptr<const SoundData> data;
    Uuid soundId;
    AudioBus bus = AudioBus::Effects;
    ma_audio_buffer_ref buffer{};
    ma_sound sound{};
    bool bufferReady = false;
    bool soundReady = false;
    /// What the mixer was last told of this voice; nothing before it was told anything.
    std::optional<VoiceSettings> applied;
    /// Whether the mixer was told to play it.
    bool started = false;
    /// Whether update() met a source that this voice is still right for, the last time
    /// it looked.
    bool seen = false;
};

/// A source that is to be heard, as update() met it. The pointer is good for as long
/// as no entity is made or destroyed, which update() does not do.
struct WantedSource {
    Entity entity;
    AudioSourceComponent* source = nullptr;
};

Status checkVolume(float volume, std::string_view what) {
    if (!std::isfinite(volume) || volume < 0.0f || volume > kMaxAudioVolume) {
        return makeError(ErrorCode::InvalidArgument, "{} is from 0 to {}, not {}", what, kMaxAudioVolume, volume);
    }
    return {};
}

Status checkPlayDesc(const AudioPlayDesc& desc) {
    if (desc.sound.isNil()) {
        return makeError(ErrorCode::InvalidArgument, "there is no sound to play: its id is nil");
    }
    if (static_cast<size_t>(desc.bus) >= kAudioBuses.size()) {
        return makeError(ErrorCode::InvalidArgument, "{} is no audio bus", static_cast<size_t>(desc.bus));
    }
    CORDITE_TRY(checkVolume(desc.volume, "the volume of a sound"));
    if (!std::isfinite(desc.pitch) || desc.pitch < kMinAudioPitch || desc.pitch > kMaxAudioPitch) {
        return makeError(ErrorCode::InvalidArgument, "the pitch of a sound is from {} to {}, not {}", kMinAudioPitch,
                         kMaxAudioPitch, desc.pitch);
    }
    if (desc.position && !isFinite(*desc.position)) {
        return makeError(ErrorCode::InvalidArgument, "the position of a sound is three numbers");
    }
    for (const float distance : {desc.minDistance, desc.maxDistance}) {
        if (!std::isfinite(distance) || distance < kMinAudioDistance || distance > kMaxAudioDistance) {
            return makeError(ErrorCode::InvalidArgument, "the distances of a sound are from {} to {} m, not {}",
                             kMinAudioDistance, kMaxAudioDistance, distance);
        }
    }
    return {};
}

} // namespace

std::string_view nameOf(AudioOutput output) {
    switch (output) {
        case AudioOutput::Offline:
            return "offline";
        case AudioOutput::Device:
            return "device";
        case AudioOutput::Silent:
            return "silent";
    }
    return "unknown";
}

struct AudioSystem::Impl {
    Impl() = default;

    ~Impl() {
        // In the order of what uses what: voices play into buses, buses into the engine,
        // the engine onto a device of the context, and all of them write to the log.
        sources.clear();
        shots.clear();
        for (size_t index = busesReady; index > 0; --index) {
            ma_sound_group_uninit(&buses[index - 1]);
        }
        if (everythingReady) {
            ma_sound_group_uninit(&everything);
        }
        if (engineReady) {
            ma_engine_uninit(&engine);
        }
        if (contextReady) {
            ma_context_uninit(&context);
        }
        if (logReady) {
            ma_log_uninit(&log);
        }
    }

    CORDITE_NON_MOVABLE(Impl);

    /// A context that knows one way of playing sound: to nobody.
    [[nodiscard]] Status makeSilentContext() {
        const std::array<ma_backend, 1> backends = {ma_backend_null};
        ma_context_config config = ma_context_config_init();
        config.pLog = &log;
        if (const ma_result result = ma_context_init(backends.data(), 1, &config, &context); result != MA_SUCCESS) {
            return makeError(ErrorCode::DeviceError, "a device that plays to nobody could not be made: {}",
                             ma_result_description(result));
        }
        contextReady = true;
        return {};
    }

    [[nodiscard]] Status start() {
        if (const ma_result result = ma_log_init(nullptr, &log); result != MA_SUCCESS) {
            return makeError(ErrorCode::DeviceError, "the log of the mixer could not be made: {}",
                             ma_result_description(result));
        }
        logReady = true;
        if (const ma_result result = ma_log_register_callback(&log, ma_log_callback_init(logFromMiniaudio, nullptr));
            result != MA_SUCCESS) {
            return makeError(ErrorCode::DeviceError, "the log of the mixer could not be listened to: {}",
                             ma_result_description(result));
        }

        ma_engine_config config = ma_engine_config_init();
        config.pLog = &log;
        config.channels = kChannels;
        config.sampleRate = desc.sampleRate;
        config.listenerCount = 1;
        config.onProcess = clipMix;
        output = desc.output;
        if (output == AudioOutput::Offline) {
            config.noDevice = MA_TRUE;
        } else if (output == AudioOutput::Silent) {
            CORDITE_TRY(makeSilentContext());
            config.pContext = &context;
        }

        ma_result result = ma_engine_init(&config, &engine);
        if (result != MA_SUCCESS && output == AudioOutput::Device) {
            // No way of playing sound could be opened at all. The game goes on without.
            CORDITE_LOG_WARN("Audio", "no sound device could be opened ({}); nothing will be heard",
                             ma_result_description(result));
            CORDITE_TRY(makeSilentContext());
            config.pContext = &context;
            output = AudioOutput::Silent;
            result = ma_engine_init(&config, &engine);
        }
        if (result != MA_SUCCESS) {
            return makeError(ErrorCode::DeviceError, "the mixer could not be made: {}", ma_result_description(result));
        }
        engineReady = true;

        // miniaudio falls back to its device for nobody by itself when a machine has no
        // other. It is worth saying, and output() tells the truth.
        if (output == AudioOutput::Device) {
            const ma_device* device = ma_engine_get_device(&engine);
            if (device != nullptr && device->pContext != nullptr && device->pContext->backend == ma_backend_null) {
                CORDITE_LOG_WARN("Audio", "this machine has no sound device; nothing will be heard");
                output = AudioOutput::Silent;
            }
        }

        // A group is where sounds are added up, not something that is played faster or
        // slower. (Nor is it a place in the world: miniaudio leaves groups out of that by
        // itself.)
        const ma_uint32 flags = MA_SOUND_FLAG_NO_PITCH;
        if (result = ma_sound_group_init(&engine, flags, nullptr, &everything); result != MA_SUCCESS) {
            return makeError(ErrorCode::DeviceError, "the mixer could not be made: {}", ma_result_description(result));
        }
        everythingReady = true;
        for (size_t index = 0; index < buses.size(); ++index) {
            if (result = ma_sound_group_init(&engine, flags, &everything, &buses[index]); result != MA_SUCCESS) {
                return makeError(ErrorCode::DeviceError, "the bus {} could not be made: {}", nameOf(kAudioBuses[index]),
                                 ma_result_description(result));
            }
            busesReady = index + 1;
        }
        busVolumes.fill(1.0f);
        return {};
    }

    [[nodiscard]] size_t voiceCount() const { return sources.size() + shots.size(); }

    /// A voice that plays `data` on `bus`, not started yet.
    [[nodiscard]] Result<std::unique_ptr<Voice>> makeVoice(std::shared_ptr<const SoundData> data, const Uuid& id,
                                                           AudioBus bus) {
        // What the assets hand out has been through decodeSound(). This is against a
        // mistake in the engine, and looks at no sample: a voice is made too often to
        // read a whole sound for it.
        if (data == nullptr || (data->channels != 1 && data->channels != 2) || data->frames() == 0 ||
            data->sampleRate < kMinSoundSampleRate || data->sampleRate > kMaxSoundSampleRate) {
            return makeError(ErrorCode::InvalidState, "the sound is not one that can be played");
        }
        auto voice = std::make_unique<Voice>();
        voice->data = std::move(data);
        voice->soundId = id;
        voice->bus = bus;
        ma_result result = ma_audio_buffer_ref_init(ma_format_f32, voice->data->channels, voice->data->samples.data(),
                                                    voice->data->frames(), &voice->buffer);
        if (result != MA_SUCCESS) {
            return makeError(ErrorCode::DeviceError, "the sound could not be read: {}", ma_result_description(result));
        }
        voice->bufferReady = true;
        // miniaudio leaves the rate of such a buffer open, which it reads as "the rate of
        // the mix"; said here, a sound of another rate is converted as it plays.
        voice->buffer.sampleRate = voice->data->sampleRate;

        result = ma_sound_init_from_data_source(&engine, &voice->buffer, 0, &buses[indexOf(bus)], &voice->sound);
        if (result != MA_SUCCESS) {
            return makeError(ErrorCode::DeviceError, "the sound could not be given a voice: {}",
                             ma_result_description(result));
        }
        voice->soundReady = true;
        ma_sound_set_rolloff(&voice->sound, 1.0f);
        return voice;
    }

    /// Tells the mixer how a voice is to be heard - of what it was told before, only
    /// what is different now. The mixer smooths what it is told; telling it the same
    /// thing every frame would be asking it to begin again every frame.
    static void apply(Voice& voice, const VoiceSettings& settings) {
        if (voice.applied == settings) {
            return;
        }
        const auto changed = [&](auto VoiceSettings::* member) {
            return !voice.applied || (*voice.applied).*member != settings.*member;
        };
        if (changed(&VoiceSettings::volume)) {
            ma_sound_set_volume(&voice.sound, settings.volume);
        }
        if (changed(&VoiceSettings::pitch)) {
            ma_sound_set_pitch(&voice.sound, settings.pitch);
        }
        if (changed(&VoiceSettings::loop)) {
            ma_sound_set_looping(&voice.sound, settings.loop ? MA_TRUE : MA_FALSE);
        }
        if (changed(&VoiceSettings::spatial)) {
            ma_sound_set_spatialization_enabled(&voice.sound, settings.spatial ? MA_TRUE : MA_FALSE);
        }
        if (changed(&VoiceSettings::position)) {
            ma_sound_set_position(&voice.sound, settings.position.x, settings.position.y, settings.position.z);
        }
        if (changed(&VoiceSettings::rolloff)) {
            ma_sound_set_attenuation_model(&voice.sound, settings.rolloff == AudioRolloff::Linear
                                                             ? ma_attenuation_model_linear
                                                             : ma_attenuation_model_inverse);
        }
        if (changed(&VoiceSettings::minDistance) || changed(&VoiceSettings::maxDistance)) {
            // With the far distance at or below the near one miniaudio gives up and makes
            // nothing quieter. A source like that is as loud as it gets up to its near
            // distance, and beyond it as quiet.
            ma_sound_set_min_distance(&voice.sound, settings.minDistance);
            ma_sound_set_max_distance(&voice.sound, std::max(settings.maxDistance, settings.minDistance * 1.001f));
        }
        voice.applied = settings;
    }

    /// Voices fall silent first - those of sources that play no more, are disabled or
    /// gone, or play something else on another bus now - so that whoever waits for a
    /// voice can have theirs in the same update.
    void dropVoicesNobodyWants() {
        for (auto& [entity, voice] : sources) {
            voice->seen = false;
        }
        for (const WantedSource& want : wanted) {
            const auto found = sources.find(want.entity);
            if (found != sources.end() && found->second->soundId == want.source->sound &&
                found->second->bus == want.source->bus) {
                found->second->seen = true;
            }
        }
        std::erase_if(sources, [](const auto& entry) { return !entry.second->seen; });
    }

    /// Files may have been put right since the assets were last looked at: what could
    /// not be played is tried again then.
    void noticeAssets(const AssetManager& assets) {
        if (const uint64_t revision = assets.database().revision(); revision != assetRevision) {
            unplayable.clear();
            assetRevision = revision;
        }
    }

    /// A voice for the sound asset `sound` on `bus`, not started yet. A sound that
    /// cannot be played is remembered with the reason, and not read again - a file
    /// that is broken would otherwise be read for every frame its source waits - until
    /// the assets change or everything is cleared.
    [[nodiscard]] Result<std::unique_ptr<Voice>> voiceFor(AssetManager& assets, const Uuid& sound, AudioBus bus) {
        if (const auto known = unplayable.find(sound); known != unplayable.end()) {
            return tl::unexpected(known->second.error);
        }
        Result<std::shared_ptr<const SoundData>> data = assets.loadSound(sound);
        Result<std::unique_ptr<Voice>> voice =
            data ? makeVoice(std::move(*data), sound, bus) : tl::unexpected(data.error());
        if (!voice) {
            unplayable.emplace(sound, Unplayable{.error = voice.error()});
        }
        return voice;
    }

    /// The voice of a source: the one it has, or a new one. None when as many voices as
    /// can be are playing - it waits, and is counted - and none for a sound that cannot
    /// be played, which is said once.
    [[nodiscard]] Voice* voiceOf(const WantedSource& want, AssetManager& assets, AudioStatistics& statistics) {
        if (const auto found = sources.find(want.entity); found != sources.end()) {
            return found->second.get();
        }
        if (voiceCount() >= desc.maxVoices) {
            ++statistics.waiting;
            return nullptr;
        }
        Result<std::unique_ptr<Voice>> voice = voiceFor(assets, want.source->sound, want.source->bus);
        if (!voice) {
            report(want.source->sound);
            return nullptr;
        }
        return sources.emplace(want.entity, std::move(*voice)).first->second.get();
    }

    /// What has come to its end is over, and a source says so.
    void endWhatIsOver(World& world) {
        for (auto entry = sources.begin(); entry != sources.end();) {
            if (ma_sound_at_end(&entry->second->sound) == MA_FALSE) {
                ++entry;
                continue;
            }
            if (auto* source = world.tryGet<AudioSourceComponent>(entry->first)) {
                source->playing = false;
            }
            entry = sources.erase(entry);
        }
        std::erase_if(shots, [](const auto& entry) { return ma_sound_at_end(&entry.second->sound) != MA_FALSE; });
    }

    /// Counts what is playing into `statistics`.
    void count(AudioStatistics& statistics) const {
        statistics.sources = static_cast<uint32_t>(sources.size());
        statistics.voices = static_cast<uint32_t>(voiceCount());
        std::unordered_set<const SoundData*> sounds;
        const auto add = [&](const Voice& voice) {
            if (sounds.insert(voice.data.get()).second) {
                statistics.soundBytes += voice.data->samples.size() * sizeof(float);
            }
        };
        for (const auto& [entity, voice] : sources) {
            add(*voice);
        }
        for (const auto& [id, voice] : shots) {
            add(*voice);
        }
        statistics.sounds = static_cast<uint32_t>(sounds.size());
    }

    /// Says once of a sound that no source can play it, until the assets change.
    void report(const Uuid& sound) {
        const auto known = unplayable.find(sound);
        if (known != unplayable.end() && !known->second.reported) {
            known->second.reported = true;
            CORDITE_LOG_ERROR("Audio", "an audio source cannot use the asset {}: {}", sound.toString(),
                              known->second.error.describe());
        }
    }

    /// Where everything is heard from, and how loud.
    void placeListener(const World& world, Entity listener) {
        Vec3 position(0.0f);
        Vec3 forward(0.0f, 0.0f, -1.0f);
        Vec3 up(0.0f, 1.0f, 0.0f);
        float heard = 1.0f;
        if (!listener.isNull()) {
            const Mat4& placement = world.worldMatrix(listener);
            const Vec3 back(placement[2]);
            const Vec3 above(placement[1]);
            const Vec3 at(placement[3]);
            // An entity scaled to nothing, or placed nowhere, faces nowhere: it hears as
            // if it stood at the origin.
            if (isFinite(at) && isFinite(back) && isFinite(above) && glm::length(back) > 1.0e-6f &&
                glm::length(above) > 1.0e-6f) {
                position = at;
                forward = -glm::normalize(back);
                up = glm::normalize(above);
            }
            if (const auto* component = world.tryGet<AudioListenerComponent>(listener)) {
                heard = within(component->volume, 0.0f, kMaxAudioVolume, 1.0f);
            }
        }
        ma_engine_listener_set_position(&engine, 0, position.x, position.y, position.z);
        ma_engine_listener_set_direction(&engine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&engine, 0, up.x, up.y, up.z);
        listenerVolume = heard;
        ma_engine_set_volume(&engine, masterVolume * listenerVolume);
    }

    /// Offline: mixes as much as `seconds` of sound. With a device, the device does.
    void mix(double seconds) {
        mixed.clear();
        if (output != AudioOutput::Offline) {
            return;
        }
        pendingFrames += seconds * static_cast<double>(desc.sampleRate);
        const auto frames = static_cast<size_t>(pendingFrames);
        pendingFrames -= static_cast<double>(frames);
        if (frames == 0) {
            return;
        }
        mixed.resize(frames * kChannels);
        // What cannot be read is silence: miniaudio fills the rest of the buffer.
        ma_uint64 read = 0;
        const ma_result result = ma_engine_read_pcm_frames(&engine, mixed.data(), frames, &read);
        if (result == MA_SUCCESS) {
            return;
        }
        std::ranges::fill(mixed, 0.0f);
        // Said once: what fails for one frame fails for the next.
        if (!mixFailureReported) {
            mixFailureReported = true;
            CORDITE_LOG_ERROR("Audio", "{} frames could not be mixed: {}", frames, ma_result_description(result));
        }
    }

    void resume() {
        if (paused) {
            ma_sound_group_start(&everything);
            paused = false;
        }
    }

    AudioSystemDesc desc;
    AudioOutput output = AudioOutput::Offline;

    ma_log log{};
    ma_context context{};
    ma_engine engine{};
    /// Everything that is of the world plays into this, so that it can be held as one.
    ma_sound_group everything{};
    std::array<ma_sound_group, kAudioBuses.size()> buses{};
    bool logReady = false;
    bool contextReady = false;
    bool engineReady = false;
    bool everythingReady = false;
    size_t busesReady = 0;

    std::array<float, kAudioBuses.size()> busVolumes{};
    float masterVolume = 1.0f;
    float listenerVolume = 1.0f;
    bool paused = false;

    /// The voices of audio sources, by their entity.
    std::unordered_map<Entity, std::unique_ptr<Voice>> sources;
    /// What play() started, in the order it was started.
    std::map<AudioVoiceId, std::unique_ptr<Voice>> shots;
    AudioVoiceId lastShot = 0;
    /// The sources update() met that are to be heard, in the order of the hierarchy.
    /// Kept between updates only for its memory.
    std::vector<WantedSource> wanted;

    /// A sound that could not be given a voice: why, and whether that has been said.
    struct Unplayable {
        Error error;
        bool reported = false;
    };

    std::unordered_map<Uuid, Unplayable> unplayable;
    uint64_t assetRevision = 0;
    bool mixFailureReported = false;

    /// Offline: the part of a frame that time has passed for and that was not mixed yet.
    double pendingFrames = 0.0;
    std::vector<float> mixed;
};

AudioSystem::AudioSystem(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

AudioSystem::~AudioSystem() = default;

Result<std::unique_ptr<AudioSystem>> AudioSystem::create(const AudioSystemDesc& desc) {
    if (desc.sampleRate < kMinSoundSampleRate || desc.sampleRate > kMaxSoundSampleRate) {
        return makeError(ErrorCode::InvalidArgument, "sound is mixed at {} to {} frames a second, not {}",
                         kMinSoundSampleRate, kMaxSoundSampleRate, desc.sampleRate);
    }
    if (desc.maxVoices == 0 || desc.maxVoices > kMaxAudioVoices) {
        return makeError(ErrorCode::InvalidArgument, "an audio system has 1 to {} voices, not {}", kMaxAudioVoices,
                         desc.maxVoices);
    }
    if (desc.output != AudioOutput::Offline && desc.output != AudioOutput::Device &&
        desc.output != AudioOutput::Silent) {
        return makeError(ErrorCode::InvalidArgument, "{} is no audio output", static_cast<uint32_t>(desc.output));
    }
    auto impl = std::make_unique<Impl>();
    impl->desc = desc;
    CORDITE_TRY(impl->start());
    return std::unique_ptr<AudioSystem>(new AudioSystem(std::move(impl)));
}

AudioOutput AudioSystem::output() const {
    return m_impl->output;
}

uint32_t AudioSystem::sampleRate() const {
    return m_impl->desc.sampleRate;
}

AudioStatistics AudioSystem::update(World& world, AssetManager& assets, double deltaSeconds) {
    Impl& impl = *m_impl;
    impl.resume();
    impl.noticeAssets(assets);

    // Who is to be heard, and who hears, in the order of the hierarchy.
    AudioStatistics statistics;
    Entity listener;
    impl.wanted.clear();
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (!world.isEnabledInHierarchy(entity)) {
            return;
        }
        if (world.has<AudioListenerComponent>(entity)) {
            if (statistics.listeners == 0) {
                listener = entity;
            }
            ++statistics.listeners;
        }
        auto* source = world.tryGet<AudioSourceComponent>(entity);
        if (source != nullptr && source->playing && !source->sound.isNil()) {
            impl.wanted.push_back({.entity = entity, .source = source});
        }
    });

    impl.dropVoicesNobodyWants();
    for (const WantedSource& want : impl.wanted) {
        Voice* voice = impl.voiceOf(want, assets, statistics);
        if (voice == nullptr) {
            continue;
        }
        const AudioSourceComponent& source = *want.source;
        // A source that is nowhere is heard like one that has no place.
        const Vec3 position(world.worldMatrix(want.entity)[3]);
        const bool placed = source.spatial && isFinite(position);
        Impl::apply(*voice, {.volume = within(source.volume, 0.0f, kMaxAudioVolume, 1.0f),
                             .pitch = within(source.pitch, kMinAudioPitch, kMaxAudioPitch, 1.0f),
                             .loop = source.loop,
                             .spatial = placed,
                             .position = placed ? position : Vec3(0.0f),
                             .rolloff = source.rolloff,
                             .minDistance = within(source.minDistance, kMinAudioDistance, kMaxAudioDistance, 1.0f),
                             .maxDistance = within(source.maxDistance, kMinAudioDistance, kMaxAudioDistance, 50.0f)});
        // Started here, once, with everything about it said: not a frame of it is heard
        // from the wrong place.
        if (!voice->started) {
            voice->started = true;
            if (const ma_result result = ma_sound_start(&voice->sound); result != MA_SUCCESS) {
                CORDITE_LOG_ERROR("Audio", "the sound of '{}' could not be started: {}", world.name(want.entity),
                                  ma_result_description(result));
            }
        }
    }

    impl.placeListener(world, listener);
    const double seconds = std::isfinite(deltaSeconds) ? std::clamp(deltaSeconds, 0.0, kMaxAudioUpdateSeconds) : 0.0;
    impl.mix(seconds);

    impl.endWhatIsOver(world);
    impl.count(statistics);
    return statistics;
}

void AudioSystem::pause() {
    Impl& impl = *m_impl;
    if (!impl.paused) {
        ma_sound_group_stop(&impl.everything);
        impl.paused = true;
    }
    impl.mixed.clear();
}

void AudioSystem::clear() {
    Impl& impl = *m_impl;
    impl.sources.clear();
    impl.shots.clear();
    impl.unplayable.clear();
    impl.mixed.clear();
    impl.pendingFrames = 0.0;
}

Result<AudioVoiceId> AudioSystem::play(AssetManager& assets, const AudioPlayDesc& desc) {
    Impl& impl = *m_impl;
    CORDITE_TRY(checkPlayDesc(desc));
    if (impl.voiceCount() >= impl.desc.maxVoices) {
        return makeError(ErrorCode::OutOfRange, "{} sounds are playing already, which is as many as can be",
                         impl.voiceCount());
    }
    impl.noticeAssets(assets);
    CORDITE_TRY_ASSIGN(std::unique_ptr<Voice> voice, impl.voiceFor(assets, desc.sound, desc.bus));
    Impl::apply(*voice, {.volume = desc.volume,
                         .pitch = desc.pitch,
                         .loop = desc.loop,
                         .spatial = desc.position.has_value(),
                         .position = desc.position.value_or(Vec3(0.0f)),
                         .rolloff = desc.rolloff,
                         .minDistance = desc.minDistance,
                         .maxDistance = desc.maxDistance});
    if (const ma_result result = ma_sound_start(&voice->sound); result != MA_SUCCESS) {
        return makeError(ErrorCode::DeviceError, "the sound could not be started: {}", ma_result_description(result));
    }
    const AudioVoiceId id = ++impl.lastShot;
    impl.shots.emplace(id, std::move(voice));
    return id;
}

void AudioSystem::stop(AudioVoiceId voice) {
    m_impl->shots.erase(voice);
}

bool AudioSystem::isPlaying(AudioVoiceId voice) const {
    const auto found = m_impl->shots.find(voice);
    return found != m_impl->shots.end() && ma_sound_at_end(&found->second->sound) == MA_FALSE;
}

float AudioSystem::volume(AudioBus bus) const {
    return m_impl->busVolumes[indexOf(bus)];
}

Status AudioSystem::setVolume(AudioBus bus, float volume) {
    if (static_cast<size_t>(bus) >= kAudioBuses.size()) {
        return makeError(ErrorCode::InvalidArgument, "{} is no audio bus", static_cast<size_t>(bus));
    }
    CORDITE_TRY(checkVolume(volume, "the volume of a bus"));
    m_impl->busVolumes[indexOf(bus)] = volume;
    ma_sound_group_set_volume(&m_impl->buses[indexOf(bus)], volume);
    return {};
}

float AudioSystem::masterVolume() const {
    return m_impl->masterVolume;
}

Status AudioSystem::setMasterVolume(float volume) {
    CORDITE_TRY(checkVolume(volume, "the volume of everything"));
    m_impl->masterVolume = volume;
    ma_engine_set_volume(&m_impl->engine, m_impl->masterVolume * m_impl->listenerVolume);
    return {};
}

std::span<const float> AudioSystem::mixed() const {
    return m_impl->mixed;
}

} // namespace cordite
