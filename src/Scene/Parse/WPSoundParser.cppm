module;

export module wescene.parse:wp_sound_parser;
import wavsen.audio;
import wescene.scene;

import :wp_sound_object;

export namespace owe
{

class WPSoundParser {
public:
    static std::shared_ptr<SceneSoundControl> Parse(const wpscene::WPSoundObject&, fs::VFS&,
                                                    wavsen::audio::SoundManager&, Scene*);
};

} // namespace owe
