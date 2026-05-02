module;

export module wescene.parse:wp_scene_parser;
import cppstd;
import wescene.audio;
import wescene.fs;
import wescene.scene;

export namespace wallpaper
{

class ISceneParser {
public:
    ISceneParser()          = default;
    virtual ~ISceneParser() = default;
    virtual std::shared_ptr<Scene>
    Parse(std::string_view scene_id, const std::string&, fs::VFS&, audio::SoundManager&) = 0;
};

class WPSceneParser : public ISceneParser {
public:
    WPSceneParser()  = default;
    ~WPSceneParser() = default;
    std::shared_ptr<Scene> Parse(std::string_view scene_id, const std::string&, fs::VFS&,
                                 audio::SoundManager&) override;
};

} // namespace wallpaper
