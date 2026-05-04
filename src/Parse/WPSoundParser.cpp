module;

#include "Core/Literals.hpp"

#include "Utils/Logging.h"

module wescene.parse;
import cppstd;

using namespace wallpaper;

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

// Adapter: wallpaper::fs::IBinaryStream → wavsen::audio::IByteStream.
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
        if (! m_curActive) {
            Switch();
        }

        // loop on EOF
        uint64_t frameReads = m_curActive ? m_curActive->next_pcm(pData, frameCount) : 0;
        if (frameReads == 0) {
            Switch();
            frameReads = m_curActive ? m_curActive->next_pcm(pData, frameCount) : 0;
        }
        // volume
        {
            float*     pData_float = static_cast<float*>(pData);
            const auto num         = frameReads * m_desc.channels;
            for (uint i = 0; i < num; i++, pData_float++) {
                (*pData_float) *= m_config.volume;
            }
        }
        return frameReads;
    };
    void pass_desc(const Desc& d) override { m_desc = d; }
    void Switch() {
        std::string path  = m_soundPaths[LoopIndex()];
        auto        bin   = vfs.Open("/assets/" + path);
        if (! bin) {
            m_curActive.reset();
            return;
        }
        auto adapter = std::make_shared<BStreamAdapter>(std::move(bin));
        m_curActive = wavsen::audio::make_stream(std::move(adapter), m_desc);
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
