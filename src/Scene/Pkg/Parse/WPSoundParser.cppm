module;

export module wescene.pkg.parse:wp_sound_parser;
import rstd.cppstd;
import wavsen.audio;
import wescene.fs;
import wescene.scene;
import wescene.pkg.scene_obj;

export namespace owe
{

class WPSoundParser {
public:
    static std::shared_ptr<SceneSoundControl> Parse(const wpscene::SoundObject&, fs::VFS&,
                                                    wavsen::audio::SoundManager&, Scene*);
};

} // namespace owe
