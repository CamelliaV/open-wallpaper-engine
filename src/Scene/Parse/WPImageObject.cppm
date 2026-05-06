module;


#include "WPJson.hpp"
#include <nlohmann/json.hpp>

export module wescene.parse:wp_image_object;
import wescene.core;
import cppstd;
import wescene.fs;

export import :wp_animation;
export import :wp_material;
export import wescene.puppet;
import :wp_scene;

export namespace owe

{

namespace wpscene
{

class WPEffectCommand {
public:
    bool        FromJson(const nlohmann::json&);
    std::string command;
    std::string target;
    std::string source;

    i32 afterpos { 0 }; // 0 for begin, start from 1
};

class WPEffectFbo {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    std::string format;
    uint32_t    scale { 1 };
};

// objects[].instance — PKGV0018+. Embedded WE-format material binding
// (compiled-shader id + textures + combos). The renderer doesn't currently
// substitute it, but the parser needs to accept the shape.
class WPObjectInstance {
public:
    bool                                          FromJson(const nlohmann::json&);
    bool                                          present { false };
    std::uint32_t                                 id { 0 };
    std::unordered_map<std::string, std::int32_t> combos;
    std::vector<std::string>                      textures;
    // usertextures elements are polymorphic: bare property-name strings
    // (PKGV0022+) and `{name, type}` system bindings (PKGV0018+). Stored
    // as raw json so both shapes are preserved.
    std::vector<nlohmann::json>                   usertextures;
};

class WPImageEffect {
private:
    static const std::unordered_set<std::string> BLACKLISTED_WORKSHOP_EFFECTS;
    bool IsEffectBlacklisted(const std::string& filePath);
public:
    bool                         FromJson(const nlohmann::json&, fs::VFS& vfs);                  // legacy
    bool                         FromJson(const nlohmann::json&, fs::VFS& vfs, SceneVersion);    // canonical
    bool                         FromFileJson(const nlohmann::json&, fs::VFS& vfs);
    int32_t                      id;
    std::string                  name;
    std::string                  username;       // PKGV0001+; per-instance label override
    bool                         visible { true };
    int32_t                      version;
    std::vector<WPMaterial>      materials;
    std::vector<WPMaterialPass>  passes;
    std::vector<WPEffectCommand> commands;
    std::vector<WPEffectFbo>     fbos;
};

class WPImageObject {
public:
    struct Config {
        bool passthrough { false };
    };
    bool                       FromJson(const nlohmann::json&, fs::VFS&);                  // legacy
    bool                       FromJson(const nlohmann::json&, fs::VFS&, SceneVersion);    // canonical
    int32_t                    id { 0 };
    std::string                name;
    std::array<float, 3>       origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>       scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3>       angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2>       size { 2.0f, 2.0f };
    std::array<float, 2>       parallaxDepth { 0.0f, 0.0f };
    std::array<float, 3>       color { 1.0f, 1.0f, 1.0f };
    int32_t                    colorBlendMode { 0 };
    float                      alpha { 1.0f };
    float                      brightness { 1.0f };
    bool                       fullscreen { false };
    bool                       nopadding { false };
    bool                       visible { true };
    std::string                image;
    std::string                alignment { "center" };
    WPMaterial                 material;
    std::vector<WPImageEffect> effects;
    Config                     config;

    // Common cross-kind metadata (PKGV0001+ unless noted).
    bool                       locktransforms { false };
    bool                       muteineditor { false };
    bool                       nointerpolation { false };  // PKGV0021+
    std::uint32_t              parent { 0 };               // PKGV0019+; 0 = no parent
    std::vector<std::int32_t>  dependencies;               // PKGV0001+; referenced object ids
    WPObjectInstance           instance;                   // PKGV0018+; instance binding

    // Image-kind specifics (gates listed for reference; reads are unconditional via _NOWARN).
    bool                       perspective { false };          // PKGV0002+
    bool                       copybackground { false };       // PKGV0001+
    bool                       solid { false };                // PKGV0002+
    bool                       opaquebackground { false };     // PKGV0005+
    bool                       clampuvs { false };             // PKGV0022+
    bool                       castshadow { false };           // PKGV0019+
    bool                       disablepropagation { false };   // PKGV0023+
    std::string                depthtest { "enabled" };        // PKGV0020+
    std::array<float, 3>       backgroundcolor { 0.0f, 0.0f, 0.0f }; // PKGV0005+
    float                      backgroundbrightness { 1.0f };  // PKGV0010+

    std::string                                puppet;
    std::vector<WPPuppetLayer::AnimationLayer> puppet_layers;

    // Per-field property-binding side channel; populated when scalar
    // fields (origin/scale/alpha/...) carry an `animation` curve or a
    // `scriptproperties` subtree. See WPAnimation.cppm.
    WPFieldBindings                            field_bindings;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPEffectFbo, name, scale);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPImageEffect, name, visible, passes, fbos, materials);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPImageObject, name, origin, angles, scale, size, visible,
                                   material, effects);

} // namespace wpscene
} // namespace owe
