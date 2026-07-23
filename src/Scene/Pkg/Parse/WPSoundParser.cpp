module wescene.pkg.parse;
import wescene.core;
import wescene.scene;
import rstd.cppstd;
import rstd.log;
import rstd;

using namespace rstd::prelude;
using namespace owe;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

enum class PlaybackMode
{
    Random,
    Loop,
    Single
};

static PlaybackMode ToPlaybackMode(std::string_view s) {
    if (s == "loop")
        return PlaybackMode::Loop;
    else if (s == "random")
        return PlaybackMode::Random;
    else if (s == "single")
        return PlaybackMode::Single;
    return PlaybackMode::Loop;
};

namespace
{

struct WPSoundState {
    Atomic<bool> playing { false };
    Atomic<f32>  volume { f32(1.0f) };
    Atomic<u32>  play_seq {};
    Atomic<u32>  stop_seq {};
};

class WPSoundControl final {
public:
    explicit WPSoundControl(Arc<WPSoundState> state): m_state(rstd::move(state)) {}

    void Play() {
        m_state->playing.store(true, Ordering::Release);
        m_state->play_seq.fetch_add(u32(1), Ordering::AcqRel);
    }
    void Stop() {
        m_state->playing.store(false, Ordering::Release);
        m_state->stop_seq.fetch_add(u32(1), Ordering::AcqRel);
    }
    void Pause() { m_state->playing.store(false, Ordering::Release); }
    bool IsPlaying() const { return m_state->playing.load(Ordering::Acquire); }
    void SetVolume(float volume) {
        m_state->volume.store(f32(volume).clamp(f32(), f32(1.0f)), Ordering::Release);
    }

private:
    Arc<WPSoundState> m_state;
};

} // namespace

class WPSoundStream : public wavsen::audio::SoundStream {
public:
    struct Config {
        float        maxtime { 10.0f };
        float        mintime { 0.0f };
        float        volume { 1.0f };
        PlaybackMode mode { PlaybackMode::Loop };
    };
    WPSoundStream(const std::vector<std::string>& paths, fs::VFS& vfs, Config c,
                  Arc<WPSoundState> state, Option<Arc<SceneAudioAverage>> audio_average)
        : vfs(vfs),
          m_config(c),
          m_state(rstd::move(state)),
          m_soundPaths(paths),
          m_audio_average(rstd::move(audio_average)) {};
    virtual ~WPSoundStream() = default;

    uint64_t next_pcm(void* pData, uint32_t frameCount) override {
        SyncControl();
        if (m_dead) return 0;
        if (! m_state->playing.load(Ordering::Acquire)) return 0;

        if (! m_curActive) {
            Switch();
        }

        uint64_t frameReads = m_curActive ? m_curActive->next_pcm(pData, frameCount) : 0;
        if (frameReads == 0 && ! m_dead) {
            m_curActive.reset();
            if (m_config.mode == PlaybackMode::Single) {
                m_state->playing.store(false, Ordering::Release);
                return 0;
            }
            Switch();
            frameReads = m_curActive ? m_curActive->next_pcm(pData, frameCount) : 0;
        }
        UpdateAudioAverage(pData, frameReads);
        {
            float*     pData_float = static_cast<float*>(pData);
            const auto num         = frameReads * m_desc.channels;
            const auto volume      = m_state->volume.load(Ordering::Acquire).to_primitive();
            for (unsigned i = 0; i < num; i++, pData_float++) {
                (*pData_float) *= volume;
            }
        }
        return frameReads;
    };
    void pass_desc(const Desc& d) override { m_desc = d; }

    // Walk paths until one opens. If all fail, disable the stream so the
    // audio callback stops re-trying every tick (which spammed FFmpeg's
    // demuxer-probe errors at audio-callback rate).
    void Switch() {
        m_curActive.reset();
        const uint32_t n = static_cast<uint32_t>(m_soundPaths.size());
        if (n == 0) {
            m_dead = true;
            return;
        }
        const uint32_t base = SelectStartIndex(n);
        for (uint32_t tried = 0; tried < n; ++tried) {
            const std::string& path   = m_soundPaths[(base + tried) % n];
            auto               source = vfs.open_read(fs::ToPath("/assets/" + path));
            if (source.is_err()) continue;
            auto handle =
                rstd::io::ReadSeekHandle::make(rstd::move(source).unwrap_unchecked().into_reader());
            auto stream = wavsen::audio::make_stream(rstd::move(handle), m_desc);
            if (stream) {
                m_curActive = std::move(stream);
                return;
            }
        }
        m_dead = true;
        m_state->playing.store(false, Ordering::Release);
        rstd::log::warn("WPSoundStream: all {} sound path(s) failed to open; disabling stream", n);
    }
    uint32_t SelectStartIndex(uint32_t n) {
        if (n == 0) return 0;
        if (m_config.mode == PlaybackMode::Random) {
            return Random::get<uint32_t>(0, n - 1);
        }
        if (m_config.mode == PlaybackMode::Single) {
            return Random::get<uint32_t>(0, n - 1);
        }
        uint32_t idx = m_curIndex;
        m_curIndex   = (m_curIndex + 1) % n;
        return idx;
    }

private:
    void SyncControl() {
        const uint32_t stop_seq = m_state->stop_seq.load(Ordering::Acquire).to_primitive();
        if (stop_seq != m_seenStopSeq) {
            m_seenStopSeq = stop_seq;
            m_curActive.reset();
            m_dead = false;
        }
        const uint32_t play_seq = m_state->play_seq.load(Ordering::Acquire).to_primitive();
        if (play_seq != m_seenPlaySeq) {
            m_seenPlaySeq = play_seq;
            m_curActive.reset();
            m_dead = false;
        }
    }

    void UpdateAudioAverage(const void* pData, uint64_t frameReads) {
        if (m_audio_average.is_none() || frameReads == 0 || m_desc.channels == 0) return;

        const float* samples = static_cast<const float*>(pData);
        const auto   total   = static_cast<std::size_t>(frameReads * m_desc.channels);
        if (total == 0) return;

        const std::size_t bin_count = (*m_audio_average)->Len().to_primitive();
        for (std::size_t bin = 0; bin < bin_count; ++bin) {
            const auto begin = bin * total / bin_count;
            const auto end   = (bin + 1) * total / bin_count;
            if (end <= begin) continue;

            float sum = 0.0f;
            for (std::size_t i = begin; i < end; ++i) sum += std::abs(samples[i]);
            float level = std::clamp(sum / static_cast<float>(end - begin), 0.0f, 1.0f);

            auto        index = usize(bin);
            const float old   = (*m_audio_average)->Load(index).to_primitive();
            (*m_audio_average)->Store(index, f32(std::max(old * 0.75f, level)));
        }
    }

    fs::VFS&          vfs;
    Config            m_config;
    Desc              m_desc;
    Arc<WPSoundState> m_state;
    uint32_t          m_curIndex { 0 };
    uint32_t          m_seenPlaySeq { 0 };
    uint32_t          m_seenStopSeq { 0 };
    bool              m_dead { false };

    const std::vector<std::string>              m_soundPaths;
    std::unique_ptr<wavsen::audio::SoundStream> m_curActive;
    Option<Arc<SceneAudioAverage>>              m_audio_average;
};

Arc<dyn<SceneSoundControl>> WPSoundParser::Parse(const wpscene::SoundObject& obj, fs::VFS& vfs,
                                                 wavsen::audio::SoundManager& sm, Scene* scene) {
    WPSoundStream::Config config { .maxtime = obj.maxtime,
                                   .mintime = obj.mintime,
                                   .volume  = std::clamp(obj.volume, 0.0f, 1.0f),
                                   .mode    = ToPlaybackMode(obj.playbackmode) };

    Option<Arc<SceneAudioAverage>> audio_average = None();
    if (scene != nullptr) audio_average = Some(scene->AudioAverageHandle());
    auto state = Arc<WPSoundState>::make();
    state->playing.store(! obj.startsilent, Ordering::Release);
    state->volume.store(f32(config.volume), Ordering::Release);
    auto control = Arc<dyn<SceneSoundControl>>::make(WPSoundControl(state.clone()));
    auto ss      = std::make_unique<WPSoundStream>(
        obj.sound, vfs, config, rstd::move(state), rstd::move(audio_average));
    sm.mount(std::move(ss));
    return control;
}
