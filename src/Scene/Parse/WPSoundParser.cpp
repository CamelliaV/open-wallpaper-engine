module;

#include <sys/types.h>
module wescene.parse;
import wescene.core;
import cppstd;
import rstd.log;

using namespace owe;

enum class PlaybackMode
{
    Random,
    Loop
};

static PlaybackMode ToPlaybackMode(std::string_view s) {
    if (s == "loop")
        return PlaybackMode::Loop;
    else if (s == "random")
        return PlaybackMode::Random;
    return PlaybackMode::Loop;
};

namespace {

// Adapter: owe::fs::IBinaryStream → wavsen::audio::IByteStream.
class BStreamAdapter : public wavsen::audio::IByteStream {
public:
    explicit BStreamAdapter(std::shared_ptr<fs::IBinaryStream> s): inner(std::move(s)) {}

    std::size_t read(void* dst, std::size_t bytes) override {
        return inner->Read(dst, bytes);
    }
    bool seek(std::int64_t offset, Origin origin) override {
        switch (origin) {
        case Origin::Begin:   return inner->SeekSet(offset);
        case Origin::Current: return inner->SeekCur(offset);
        case Origin::End:     return inner->SeekEnd(offset);
        }
        return false;
    }

private:
    std::shared_ptr<fs::IBinaryStream> inner;
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
    WPSoundStream(const std::vector<std::string>& paths, fs::VFS& vfs, Config c)
        : vfs(vfs), m_config(c), m_soundPaths(paths) {};
    virtual ~WPSoundStream() = default;

    uint64_t next_pcm(void* pData, uint32_t frameCount) override {
        if (m_dead) return 0;

        if (! m_curActive) {
            Switch();
        }

        // loop on EOF
        uint64_t frameReads = m_curActive ? m_curActive->next_pcm(pData, frameCount) : 0;
        if (frameReads == 0 && ! m_dead) {
            Switch();
            frameReads = m_curActive ? m_curActive->next_pcm(pData, frameCount) : 0;
        }
        // volume
        {
            float*     pData_float = static_cast<float*>(pData);
            const auto num         = frameReads * m_desc.channels;
            for (unsigned i = 0; i < num; i++, pData_float++) {
                (*pData_float) *= m_config.volume;
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
        for (uint32_t tried = 0; tried < n; ++tried) {
            const std::string& path = m_soundPaths[LoopIndex()];
            auto bin = vfs.Open("/assets/" + path);
            if (! bin) continue;
            auto adapter = std::make_shared<BStreamAdapter>(std::move(bin));
            auto stream  = wavsen::audio::make_stream(std::move(adapter), m_desc);
            if (stream) {
                m_curActive = std::move(stream);
                return;
            }
        }
        m_dead = true;
        rstd::log::warn(
            "WPSoundStream: all {} sound path(s) failed to open; disabling stream",
            n);
    }
    uint32_t LoopIndex() {
        m_curIndex++;
        if (m_curIndex == m_soundPaths.size()) m_curIndex = 0;
        return m_curIndex;
    }

private:
    fs::VFS& vfs;
    Config   m_config;
    Desc     m_desc;
    uint32_t m_curIndex { 0 };
    bool     m_dead { false };

    const std::vector<std::string>             m_soundPaths;
    std::unique_ptr<wavsen::audio::SoundStream> m_curActive;
};

void WPSoundParser::Parse(const wpscene::WPSoundObject& obj, fs::VFS& vfs,
                          wavsen::audio::SoundManager& sm) {
    WPSoundStream::Config config { .maxtime = obj.maxtime,
                                   .mintime = obj.mintime,
                                   .volume  = obj.volume > 1.0f ? 1.0f : obj.volume,
                                   .mode    = ToPlaybackMode(obj.playbackmode) };

    auto ss = std::make_unique<WPSoundStream>(obj.sound, vfs, config);
    sm.mount(std::move(ss));
}
