module;

#include "WPJson.hpp"
#include <nlohmann/json.hpp>

export module wescene.parse:wp_material;
import cppstd;
import wescene.fs;
import :wp_scene;

export namespace wallpaper

{
namespace wpscene
{

class WPMaterialPassBindItem {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    int32_t     index;
};

class WPMaterialPass {
public:
    bool                                                FromJson(const nlohmann::json&);
    void                                                Update(const WPMaterialPass&);
    std::vector<std::string>                            textures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    std::string                                         target;
    std::vector<WPMaterialPassBindItem>                 bind;
};

class WPMaterial {
public:
    bool                                                FromJson(const nlohmann::json&);                  // legacy
    bool                                                FromJson(const nlohmann::json&, SceneVersion);    // canonical
    void                                                MergePass(const WPMaterialPass&);
    std::string                                         blending { "translucent" };
    std::string                                         cullmode { "nocull" };
    std::string                                         shader;
    std::string                                         depthtest { "disabled" };
    std::string                                         depthwrite { "disabled" };
    std::vector<std::string>                            textures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;

    bool use_puppet { false };
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterialPassBindItem, name, index);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterialPass, bind, target, textures, combos,
                                   constantshadervalues);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPMaterial, blending, shader, textures, combos,
                                   constantshadervalues);
} // namespace wpscene
} // namespace wallpaper
