export module wescene.pkg.parse:wp_scene_parser;
import rstd;
import wavsen.audio;
import wescene.fs;
import wescene.json;
import wescene.load_bench;
import wescene.scene;
import wescene.pkg.scene_obj;
import :wp_uniform_source;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe
{

enum class SceneParseErrorKind
{
    Document,
    ObjectExpansion,
    Asset,
    Shader,
    Finalize,
};

struct SceneParseError {
    SceneParseErrorKind kind { SceneParseErrorKind::Document };
    String              message;
};

struct SceneParseOptions {
    SceneLoadBenchRecorderView   load_bench;
    Option<ref<rstd::json::Map>> user_properties;
};

struct ParsedScene {
    Box<Scene>                 scene;
    Arc<WPUniformRuntimeInput> runtime_input;
};

class WPSceneParser {
public:
    auto Parse(ref<str> scene_id, ref<wpscene::SceneDocument> document, mut_ref<fs::VFS> vfs,
               mut_ref<wavsen::audio::SoundManager> sound, SceneParseOptions options = {})
        -> Result<ParsedScene, SceneParseError>;
};

} // namespace owe
