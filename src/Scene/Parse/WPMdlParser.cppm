module;


#include <Eigen/Dense>

export module wescene.parse:wp_mdl_parser;
import wescene.core;
import cppstd;
import wescene.fs;
import wescene.scene;

export import wescene.puppet;
import :wp_shader_parser;  // WPShaderInfo
import :wp_material;       // wpscene::WPMaterial

export namespace wallpaper

{

struct WPMdl {
    i32 mdlv { 13 };
    i32 mdls { 1 };
    i32 mdla { 1 };

    std::string mat_json_file;
    struct Vertex {
        std::array<float, 3>    position;
        std::array<uint32_t, 4> blend_indices;
        std::array<float, 4>    weight;
        std::array<float, 2>    texcoord;
    };
    std::vector<Vertex>                  vertexs;
    std::vector<std::array<uint16_t, 3>> indices;

    // std::vector<Eigen::Matrix<float, 3, 4>> bones;
    std::shared_ptr<WPPuppet> puppet;
    // combo
    // SKINNING = 1
    // BONECOUNT

    // input
    // uvec4 a_BlendIndices
    // vec4 a_BlendWeights
    // uniform mat4x3 g_Bones[BONECOUNT]
};

class WPMdlParser {
public:
    static bool Parse(std::string_view path, fs::VFS&, WPMdl&);

    static void AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl);
    static void AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl);

    static void GenPuppetMesh(SceneMesh& mesh, const WPMdl& mdl);
};

} // namespace wallpaper
