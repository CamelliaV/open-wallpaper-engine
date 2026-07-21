export module viewer.common:arg;

import rstd.argparse;
import rstd.cppstd;
import wescene.cli;

using namespace rstd::prelude;
using namespace rstd::argparse;

export namespace viewer
{

struct Resolution {
    unsigned w;
    unsigned h;
};

struct SceneViewerArgs {
    std::string assets_dir;
    std::string scene_path;
    std::string cache_path;
    std::string user_properties_path;
    std::string mouse_position;
    Resolution  resolution;
    i32         fps;
    u32         msaa_samples;
    bool        enable_valid_layer;
    bool        graphviz;
    bool        stdin_json;
};

SceneViewerArgs ParseSceneViewerArgs(int argc, char** argv);

} // namespace viewer

namespace viewer
{

std::string ToStdString(const String& value) {
    return { value.data(), value.size().to_primitive() };
}

template<typename T>
const T& Value(const Matches& matches, const ArgKey<T>& key) {
    auto value = matches.get_one(key);
    if (value.is_err() || value->is_none()) rstd::unreachable();
    return ***value;
}

bool Flag(const Matches& matches, const ArgKey<bool>& key) {
    auto value = matches.get_one(key);
    if (value.is_err()) rstd::unreachable();
    return value->is_some() && ***value;
}

auto ResolutionParser(ref<rstd::ffi::OsStr> raw) -> Result<Resolution, ValueError> {
    auto text = raw.to_str();
    if (text.is_none()) return Err(ValueError::InvalidUtf8());

    std::string_view value { reinterpret_cast<const char*>((*text).data()),
                             (*text).size().to_primitive() };
    const auto       separator = value.find('x');
    unsigned         width     = 1280;
    unsigned         height    = 720;
    if (separator == std::string_view::npos) return Ok(Resolution { width, height });

    auto width_text  = value.substr(0, separator);
    auto height_text = value.substr(separator + 1);
    auto width_result =
        std::from_chars(width_text.data(), width_text.data() + width_text.size(), width);
    auto height_result =
        std::from_chars(height_text.data(), height_text.data() + height_text.size(), height);
    if (width_result.ec != std::errc {} ||
        width_result.ptr != width_text.data() + width_text.size() ||
        height_result.ec != std::errc {} ||
        height_result.ptr != height_text.data() + height_text.size()) {
        return Ok(Resolution { 1280, 720 });
    }
    return Ok(Resolution { width, height });
}

SceneViewerArgs ParseSceneViewerArgs(int argc, char** argv) {
    auto command     = Command::make("scene-viewer");
    auto assets      = command.add_arg(Arg<String>::value("assets", string_parser())
                                           .value_name("ASSETS")
                                           .help("assets folder")
                                           .required());
    auto scene       = command.add_arg(Arg<String>::value("scene", string_parser())
                                           .value_name("SCENE")
                                           .help("scene file")
                                           .required());
    auto fps         = command.add_arg(Arg<i32>::value("fps", from_str_parser<i32>())
                                           .short_name('f')
                                           .long_name("fps")
                                           .help("fps")
                                           .default_value("15"));
    auto valid_layer = command.add_arg(Arg<bool>::flag("valid-layer")
                                           .short_name('V')
                                           .long_name("valid-layer")
                                           .help("enable vulkan valid layer"));
    auto graphviz =
        command.add_arg(Arg<bool>::flag("graphviz")
                            .short_name('G')
                            .long_name("graphviz")
                            .help("generate graphviz of render graph, output to 'graph.dot'"));
    auto stdin_json =
        command.add_arg(Arg<bool>::flag("stdin-json")
                            .long_name("stdin-json")
                            .help("read JSONL commands from stdin, for example: "
                                  "{\"command\":\"set_user_property\",\"key\":\"name\","
                                  "\"value\":1}"));
    auto cache_path = command.add_arg(Arg<String>::value("cache-path", string_parser())
                                          .short_name('C')
                                          .long_name("cache-path")
                                          .help("shader cache directory")
                                          .default_value(""));
    auto msaa = command.add_arg(Arg<u32>::value("msaa", from_str_parser<u32>())
                                    .short_name('M')
                                    .long_name("msaa")
                                    .help("MSAA samples for screen RT (1/2/4/8/16; default 1=off)")
                                    .default_value("1"));
    auto user_properties = command.add_arg(
        Arg<String>::value("user-properties", string_parser())
            .short_name('P')
            .long_name("user-properties")
            .help("Path to a JSON file mapping project.json property keys to user-edited values")
            .default_value(""));
    auto mouse_position =
        command.add_arg(Arg<String>::value("mouse-position", string_parser())
                            .long_name("mouse-position")
                            .help("Set initial normalized mouse position, e.g. 0,1")
                            .default_value(""));
    auto resolution = command.add_arg(
        Arg<Resolution>::value("resolution", parse_with<Resolution>(ResolutionParser))
            .short_name('R')
            .long_name("resolution")
            .help("Set the resolution, eg. 1920x1080")
            .default_value("1280x720"));

    auto parsed = owe::cli::ParseArgs(rstd::move(command), argc, argv);
    if (parsed.is_err()) std::exit(parsed.unwrap_err().code);
    auto matches = rstd::move(parsed).unwrap();

    return SceneViewerArgs {
        .assets_dir           = ToStdString(Value(matches, assets)),
        .scene_path           = ToStdString(Value(matches, scene)),
        .cache_path           = ToStdString(Value(matches, cache_path)),
        .user_properties_path = ToStdString(Value(matches, user_properties)),
        .mouse_position       = ToStdString(Value(matches, mouse_position)),
        .resolution           = Value(matches, resolution),
        .fps                  = Value(matches, fps),
        .msaa_samples         = Value(matches, msaa),
        .enable_valid_layer   = Flag(matches, valid_layer),
        .graphviz             = Flag(matches, graphviz),
        .stdin_json           = Flag(matches, stdin_json),
    };
}

} // namespace viewer
