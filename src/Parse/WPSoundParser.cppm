module;


export module wescene.parse:wp_sound_parser;
import wavsen.audio;

import :wp_sound_object;

export namespace wallpaper
{

class WPSoundParser {
public:
    static void Parse(const wpscene::WPSoundObject&, fs::VFS&, wavsen::audio::SoundManager&);
};

}