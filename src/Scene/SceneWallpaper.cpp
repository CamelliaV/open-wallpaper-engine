module;

#include <rstd/macro.hpp>

module wescene.scene_wallpaper;
import wescene.types;
import wescene.utils;
import wescene.scene;
import wescene.spec_names;

import eigen;
import owe.user_property;
import rstd;
import rstd.log;
import rstd.cppstd;
import wavsen.audio;
import wescene.fs;
import wescene.timer;
import wescene.pkg.parse;
import wescene.pkg_fs;
import wescene.rgraph;
import wescene.resource;
import wescene.script;
import wescene.vulkan_render;

using namespace owe;
using namespace rstd::prelude;

namespace owe
{

// ---- Render-thread messages -------------------------------------------------

struct RenderInit {
    std::shared_ptr<RenderInitInfo> info;
};
struct RenderSetScene {
    std::shared_ptr<Scene>                 scene;
    std::shared_ptr<WPUniformRuntimeInput> uniform_input;
};
struct RenderSetFillMode {
    FillMode mode;
};
struct RenderSetSpeed {
    float speed;
};
struct RenderSetUserProperty {
    std::string key;
    Json        property;
};
struct RenderSetMediaStatus {
    MediaStatus status;
};
struct RenderSetAudioResponseDemandCallback {
    AudioResponseDemandCallback callback;
};
struct RenderSetAudioResponseEnabled {
    bool enabled;
};
struct RenderSetAudioSpectrum {
    std::array<float, 64>                 left;
    std::array<float, 64>                 right;
    std::chrono::steady_clock::time_point received;
};
struct RenderStop {
    bool stop;
};
struct RenderDraw {};
struct RenderShutdown {};
struct RenderSwapchainReady {
    bool     ready;
    uint32_t width;
    uint32_t height;
};
struct RenderRequestPreparedPassDiagnostics {
    RenderPassDiagnosticCallback cb;
};

// Wrapped in a non-std struct so the rstd channel's internal `addressof`
// calls don't fall into ADL ambiguity with std::addressof when the element
// type sits in namespace std.
struct RenderMsg {
    std::variant<RenderInit, RenderSetScene, RenderSetFillMode, RenderSetSpeed,
                 RenderSetUserProperty, RenderSetMediaStatus, RenderSetAudioResponseDemandCallback,
                 RenderSetAudioResponseEnabled, RenderSetAudioSpectrum, RenderStop, RenderDraw,
                 RenderSwapchainReady, RenderRequestPreparedPassDiagnostics, RenderShutdown>
        v;
};

// ---- Main-thread messages ---------------------------------------------------

struct MainLoadScene {};
struct MainStop {
    bool     stop;
    uint32_t fade_ms { 0 };
    bool     scale_audio { false };
};
struct MainPauseAudio {
    uint64_t generation { 0 };
};
struct MainFirstFrame {};
struct MainShutdown {};
struct MainConfigure {
    SceneWallpaperConfig config;
};
struct MainSetFps {
    uint32_t fps { 0 };
};
struct MainSetVolume {
    float volume { 1.0f };
};
struct MainSetVolumeScale {
    float    scale { 1.0f };
    uint32_t fade_ms { 0 };
};
struct MainSetMuted {
    bool muted { false };
};
struct MainSetFillMode {
    FillMode mode { FillMode::ASPECTCROP };
};
struct MainSetSpeed {
    float speed { 1.0f };
};
struct MainSetUserProperty {
    std::string key;
    Json        value;
};
struct MainSetFirstFrameCallback {
    FirstFrameCallback cb;
};
struct MainSetUserPropertyDiagnosticCallback {
    UserPropertyDiagnosticCallback cb;
};
struct MainUserPropertyDiagnostics {
    std::vector<SceneUserPropertyDiagnostic> diagnostics;
};
struct MainPreparedPassDiagnostics {
    RenderPassDiagnosticCallback                cb;
    std::vector<vulkan::PreparedPassDiagnostic> diagnostics;
};

struct MainMsg {
    std::variant<MainLoadScene, MainConfigure, MainSetFps, MainSetVolume, MainSetVolumeScale,
                 MainSetMuted, MainSetFillMode, MainSetSpeed, MainSetUserProperty,
                 MainSetFirstFrameCallback, MainSetUserPropertyDiagnosticCallback,
                 MainUserPropertyDiagnostics, MainPreparedPassDiagnostics, MainStop, MainPauseAudio,
                 MainFirstFrame, MainShutdown>
        v;
};

namespace
{

Json MakeUserPropertyDescriptor(Json value) {
    if (value.get("value").is_some()) return value;
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make(rstd::cppstd::as_str("value")), rstd::move(value));
    return Json::Object(rstd::move(object));
}

Json RawUserProperty(std::string_view value) { return MakeUserPropertyWirePatch(value); }

Json InitialUserProperty(Json value) {
    if (value.is_string()) {
        auto raw = rstd::cppstd::to_string(*value.as_str());
        return RawUserProperty(raw);
    }
    return MakeUserPropertyDescriptor(std::move(value));
}

bool IsShaderGraphUserProperty(const Json& prop) {
    auto type = prop.get("type");
    if (type.is_none()) return false;
    auto string = (*type)->as_str();
    return string.is_some() && rstd::cppstd::as_string_view(*string) == "combo";
}

constexpr std::string_view kSchemeColorKey          = "schemecolor";
constexpr std::string_view kWaywallenSchemeColorKey = "waywallen.scheme_color";

std::string CanonicalUserPropertyKey(std::string_view key) {
    if (key == kWaywallenSchemeColorKey) return std::string(kSchemeColorKey);
    return std::string(key);
}

// Parse a "r g b" / "r g b a" / "x y z w ..." space-separated float string into
// a small float vector. Trailing / leading whitespace is tolerated. Returns
// false when no numbers parse — caller treats as coercion failure.
bool ParseFloatList(std::string_view s, std::vector<float>& out) {
    out.clear();
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        std::string tok(s.substr(start, i - start));
        try {
            out.push_back(std::stof(tok));
        } catch (...) {
            return false;
        }
    }
    return ! out.empty();
}

// Coerce a project.json property entry into a ShaderValue. Returns ok=false
// (with skip_reason) for combo / texture / unsupported types — the handler
// logs and skips those.
struct UserPropertyCoerceResult {
    bool        ok { false };
    ShaderValue value;
    const char* skip_reason { nullptr };
};

UserPropertyCoerceResult CoerceUserPropertyValue(const Json& prop) {
    UserPropertyCoerceResult r;

    // Pull the explicit type if present; project.json properties always have
    // one, but inline {"value": ...} descriptors don't.
    std::string type;
    if (auto member = prop.get("type"); member.is_some()) {
        auto string = (*member)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }

    // Combo / texture / file paths can't write a uniform.
    if (type == "combo") {
        r.skip_reason = "shader graph mutation is not a uniform update";
        return r;
    }
    if (type == "texture" || type == "replacetexture" || type == "file" || type == "textinput") {
        r.skip_reason = "non-uniform property type";
        return r;
    }

    // Find the raw value.
    auto        value = prop.get("value");
    const Json& v     = value.is_some() ? **value : prop;

    if (type == "color") {
        std::vector<float> nums;
        auto               value = v.as_str();
        if (value.is_some() && ParseFloatList(rstd::cppstd::as_string_view(*value), nums) &&
            nums.size() >= 3) {
            r.ok    = true;
            r.value = ShaderValue(std::span<const float>(nums));
            return r;
        }
        r.skip_reason = "color value not a 'r g b[ a]' float string";
        return r;
    }

    // Fallback inference when type is missing.
    if (v.is_boolean()) {
        r.ok    = true;
        float f = *v.as_bool() ? 1.0f : 0.0f;
        r.value = ShaderValue(f);
        return r;
    }
    if (v.is_number()) {
        auto number = v.as_f64();
        if (number.is_some()) {
            const double value = number->to_primitive();
            r.ok               = value >= std::numeric_limits<float>::lowest() &&
                                 value <= std::numeric_limits<float>::max();
            if (r.ok) r.value = ShaderValue(static_cast<float>(value));
        }
        return r;
    }
    if (v.is_string()) {
        std::vector<float> nums;
        auto               value = rstd::cppstd::as_string_view(*v.as_str());
        if (ParseFloatList(value, nums)) {
            if (nums.size() == 1) {
                r.ok    = true;
                r.value = ShaderValue(nums[0]);
                return r;
            }
            r.ok    = true;
            r.value = ShaderValue(std::span<const float>(nums));
            return r;
        }
        r.skip_reason = "string value isn't parseable as float list";
        return r;
    }
    r.skip_reason = "unsupported JSON value shape";
    return r;
}

void ApplyUserPropertyToClear(Scene& scene, const std::string& key, const Json& prop) {
    if (scene.clearColorUserKey.empty()) return;
    if (CanonicalUserPropertyKey(scene.clearColorUserKey) != key) return;
    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < usize(3)) return;
    auto clamp01 = [](float n) {
        return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
    };
    scene.clearColor = {
        clamp01(coerced.value[usize()]),
        clamp01(coerced.value[usize(1)]),
        clamp01(coerced.value[usize(2)]),
    };
}

// Push a user-property value to every material whose shader declared a
// `u_*` uniform with this material-key.
void ApplyUserPropertyToShaderUniforms(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.shader_user_var_index.find(key);
    if (it == scene.shader_user_var_index.end()) return;

    if (IsShaderGraphUserProperty(prop)) {
        rstd_warn("user property '{}' skipped: shader graph mutation is not a uniform update", key);
        return;
    }

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok) {
        rstd_warn("user property '{}' skipped: {}",
                  key,
                  coerced.skip_reason ? coerced.skip_reason : "unknown");
        return;
    }
    for (auto& [material, uniform_name] : it->second) {
        if (! material) continue;
        scene.SetMaterialShaderValue(*material, uniform_name, coerced.value);
    }
}

std::optional<std::string> ResolveRuntimeSceneTextureProperty(const Json& prop) {
    if (prop.is_string()) {
        return rstd::cppstd::to_string(*prop.as_str());
    }
    if (! prop.is_object()) return std::nullopt;

    std::string type;
    if (auto member = prop.get("type"); member.is_some()) {
        auto string = (*member)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }
    if (! type.empty() && type != "scenetexture" && type != "texture" && type != "replacetexture")
        return std::nullopt;
    auto value = prop.get("value");
    if (value.is_none()) return std::nullopt;
    auto string = (*value)->as_str();
    return string.is_some() ? std::optional<std::string>(rstd::cppstd::to_string(*string))
                            : std::nullopt;
}

bool SameSceneMaterialId(SceneMaterialId lhs, SceneMaterialId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

void PushUniqueMaterialId(std::vector<SceneMaterialId>& materials, SceneMaterialId id) {
    auto it = std::find_if(materials.begin(), materials.end(), [id](auto existing) {
        return SameSceneMaterialId(existing, id);
    });
    if (it == materials.end()) materials.push_back(id);
}

vulkan::PassInvalidationFlags MaterialDirtyToPassInvalidationFlags(SceneMaterialDirtyFlags flags) {
    vulkan::PassInvalidationFlags out = vulkan::PassInvalidationNone;
    if ((flags & SceneMaterialDirtyResources) != 0) {
        out |= vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Resources);
    }
    if ((flags & SceneMaterialDirtyPipeline) != 0) {
        out |= vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Pipeline) |
               vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Framebuffer);
    }
    return out;
}

std::vector<SceneMaterialId>
ApplyUserPropertyToMaterialTextures(Scene& scene, const std::string& key, const Json& prop) {
    std::vector<SceneMaterialId> changed_materials;
    auto                         it = scene.material_texture_user_index.find(key);
    if (it == scene.material_texture_user_index.end()) return changed_materials;

    auto texture_value = ResolveRuntimeSceneTextureProperty(prop);
    if (! texture_value.has_value()) return changed_materials;

    for (const auto& binding : it->second) {
        if (binding.material == nullptr) continue;
        std::string next     = texture_value->empty() ? binding.fallback : *texture_value;
        auto        mutation = scene.SetMaterialTextureSlot(*binding.material, binding.slot, next);
        if (mutation.changed && mutation.material.is_some()) {
            PushUniqueMaterialId(changed_materials, *mutation.material);
        }
    }

    return changed_materials;
}

Json RuntimeTextureProperty(std::string value) {
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make(rstd::cppstd::as_str("type")),
                  JsonFromStd("scenetexture"));
    object.insert(::alloc::string::String::make(rstd::cppstd::as_str("value")), JsonFromStd(value));
    return Json::Object(rstd::move(object));
}

owe::script::MediaStatus ToScriptMediaStatus(const MediaStatus& status) {
    return owe::script::MediaStatus { .state            = status.state,
                                      .title            = status.title,
                                      .artist           = status.artist,
                                      .album            = status.album,
                                      .album_artist     = status.album_artist,
                                      .art_url          = status.art_url,
                                      .previous_art_url = status.previous_art_url };
}

std::vector<SceneUserPropertyDiagnostic> CollectUserPropertyDiagnostics(const Scene&     scene,
                                                                        std::string_view key) {
    std::vector<SceneUserPropertyDiagnostic> out;
    for (const auto& diagnostic : scene.UserPropertyDiagnostics()) {
        if (diagnostic.key == key) out.push_back(diagnostic);
    }
    return out;
}

std::optional<std::string>
ResolveRuntimeShaderComboValue(const Json& prop, const Scene::ShaderComboUserBinding& binding) {
    auto        member = prop.get("value");
    const auto& value  = member.is_some() ? **member : prop;

    if (value.is_null()) return binding.fallback;
    if (value.is_boolean()) return *value.as_bool() ? "1" : "0";
    if (value.is_number()) {
        auto number = value.as_f64();
        if (number.is_some()) {
            const double native_number = number->to_primitive();
            if (native_number >= std::numeric_limits<int>::min() &&
                native_number <= std::numeric_limits<int>::max())
                return std::to_string(static_cast<int>(native_number));
        }
        return std::nullopt;
    }
    if (! value.is_string()) return std::nullopt;

    auto text = rstd::cppstd::to_string(*value.as_str());
    if (text.empty()) return binding.fallback;
    if (auto it = binding.options.find(text); it != binding.options.end()) return it->second;
    if (text == "true") return "1";
    if (text == "false") return "0";

    try {
        std::size_t parsed = 0;
        int         number = std::stoi(text, &parsed);
        if (parsed == text.size()) return std::to_string(number);
    } catch (...) {
    }
    return std::nullopt;
}

void RecordShaderComboDiagnostic(Scene& scene, std::string key,
                                 SceneUserPropertyDiagnosticCode code, std::string material,
                                 std::string combo, std::string message) {
    scene.AddUserPropertyDiagnostic(SceneUserPropertyDiagnostic {
        .key      = std::move(key),
        .code     = code,
        .material = std::move(material),
        .combo    = std::move(combo),
        .message  = std::move(message),
    });
}

bool ApplyUserPropertyToShaderCombos(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.shader_combo_user_index.find(key);
    if (it == scene.shader_combo_user_index.end()) return false;

    scene.ClearUserPropertyDiagnostics(key);

    auto* vfs = static_cast<fs::VFS*>(scene.vfs.get());
    if (! vfs) {
        rstd_warn("user property '{}' skipped: scene VFS is not available", key);
        RecordShaderComboDiagnostic(scene,
                                    key,
                                    SceneUserPropertyDiagnosticCode::SceneVfsUnavailable,
                                    {},
                                    {},
                                    "scene VFS is not available");
        return false;
    }

    bool requires_graph_rebuild = false;
    for (const auto& binding : it->second) {
        if (! binding.material) continue;
        auto next = ResolveRuntimeShaderComboValue(prop, binding);
        if (! next.has_value()) {
            rstd_warn(
                "user property '{}' skipped: combo '{}' value is unsupported", key, binding.combo);
            RecordShaderComboDiagnostic(
                scene,
                key,
                SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue,
                binding.material ? binding.material->name : std::string {},
                binding.combo,
                "shader combo value is unsupported");
            continue;
        }
        auto& material = *binding.material;
        if (! material.customShader.variant.has_value()) {
            rstd_warn("user property '{}' skipped: material '{}' has no shader variant descriptor",
                      key,
                      material.name);
            RecordShaderComboDiagnostic(
                scene,
                key,
                SceneUserPropertyDiagnosticCode::MissingShaderVariantDescriptor,
                material.name,
                binding.combo,
                "material has no shader variant descriptor");
            continue;
        }
        const auto& current_variant = *material.customShader.variant;
        if (auto current = current_variant.resolved_combos.find(binding.combo);
            current != current_variant.resolved_combos.end() && current->second == *next) {
            continue;
        }

        auto compiled = WPShaderParser::CompileSceneShaderVariant(
            current_variant, *vfs, { { binding.combo, *next } });
        if (! compiled.ok || ! compiled.shader) {
            rstd_warn("user property '{}' skipped: shader combo '{}' compile failed: {}",
                      key,
                      binding.combo,
                      compiled.error);
            RecordShaderComboDiagnostic(scene,
                                        key,
                                        SceneUserPropertyDiagnosticCode::ShaderComboCompileFailed,
                                        material.name,
                                        binding.combo,
                                        compiled.error);
            continue;
        }
        auto mutation = scene.SetMaterialShaderVariant(material,
                                                       SceneShaderVariantMutation {
                                                           .shader  = std::move(compiled.shader),
                                                           .variant = std::move(compiled.variant),
                                                       });
        if (mutation.changed && (material.DirtyFlags() & SceneMaterialDirtyGraph) != 0) {
            requires_graph_rebuild = true;
        }
    }
    return requires_graph_rebuild;
}

float CurrentImagePropertyAlpha(SceneNode* node) {
    if (! node) return 1.0f;
    return node->IsAlphaOverridden() ? node->EffectiveAlpha() : node->BaseAlpha();
}

Eigen::Vector3f CurrentImagePropertyColor(SceneNode* node) {
    if (! node) return { 1.0f, 1.0f, 1.0f };
    return node->IsColorOverridden() ? node->Color() : node->BaseColor();
}

bool MaterialHasShaderUniform(const SceneMaterial& material, std::string_view uniform_name) {
    const std::string name(uniform_name);
    if (material.customShader.constValues.contains(name)) return true;
    if (material.customShader.shader &&
        material.customShader.shader->default_uniforms.contains(name))
        return true;
    if (material.customShader.variant &&
        material.customShader.variant->default_uniforms.contains(name))
        return true;
    return false;
}

void ApplyUserPropertyToImageColor(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.image_color_user_index.find(key);
    if (it == scene.image_color_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < usize(3)) return;

    Eigen::Vector3f color { coerced.value[usize()],
                            coerced.value[usize(1)],
                            coerced.value[usize(2)] };
    for (const auto& binding : it->second) {
        if (binding.node) binding.node->SetColor(color);

        std::array<float, 3> color3 { color.x(), color.y(), color.z() };
        for (auto* material : binding.materials) {
            if (! material) continue;
            const bool           has_user_alpha = MaterialHasShaderUniform(*material, G_USERALPHA);
            const float          alpha          = has_user_alpha && binding.node
                                                      ? binding.node->BaseAlpha()
                                                      : CurrentImagePropertyAlpha(binding.node);
            std::array<float, 4> color4 { color.x(), color.y(), color.z(), alpha };
            if (MaterialHasShaderUniform(*material, G_COLOR4))
                scene.SetMaterialShaderValue(*material, G_COLOR4, color4);
            if (MaterialHasShaderUniform(*material, G_COLOR))
                scene.SetMaterialShaderValue(*material, G_COLOR, color3);
        }
    }
}

void ApplyUserPropertyToImageAlpha(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.image_alpha_user_index.find(key);
    if (it == scene.image_alpha_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < usize(1)) return;

    const float alpha = std::clamp(coerced.value[usize()], 0.0f, 1.0f);
    for (const auto& binding : it->second) {
        if (binding.node) binding.node->SetUserAlpha(alpha);

        Eigen::Vector3f      color = CurrentImagePropertyColor(binding.node);
        std::array<float, 4> color4 { color.x(), color.y(), color.z(), alpha };
        for (auto* material : binding.materials) {
            if (! material) continue;
            const bool has_user_alpha = MaterialHasShaderUniform(*material, G_USERALPHA);
            if (has_user_alpha) scene.SetMaterialShaderValue(*material, G_USERALPHA, alpha);
            if (MaterialHasShaderUniform(*material, G_ALPHA))
                scene.SetMaterialShaderValue(*material, G_ALPHA, alpha);
            if (! has_user_alpha && MaterialHasShaderUniform(*material, G_COLOR4))
                scene.SetMaterialShaderValue(*material, G_COLOR4, color4);
        }
    }
}

// Push a user-property value into every particle subsystem whose
// instanceoverride was authored as `{user:"<key>", value:...}` for one of
// its fields. The override sits behind a shared_ptr; mutating it through the
// scene-wide binding index is observed by every initializer / operator
// closure on next emission.
void ApplyUserPropertyToParticles(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.particle_user_var_index.find(key);
    if (it == scene.particle_user_var_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok) return;

    auto write_scalar = [&](float& dst) {
        if (coerced.value.size() >= usize(1)) dst = coerced.value[usize()];
    };
    auto write_vec3 = [&](std::array<float, 3>& dst, float scale) {
        if (coerced.value.size() < usize(3)) return;
        dst = { coerced.value[usize()] * scale,
                coerced.value[usize(1)] * scale,
                coerced.value[usize(2)] * scale };
    };

    for (auto& b : it->second) {
        if (! b.state) continue;
        auto*              st = static_cast<owe::wpscene::ParticleInstanceoverride*>(b.state.get());
        const std::string& f  = b.field;
        if (f == "alpha")
            write_scalar(st->alpha);
        else if (f == "size")
            write_scalar(st->size);
        else if (f == "lifetime")
            write_scalar(st->lifetime);
        else if (f == "rate")
            write_scalar(st->rate);
        else if (f == "speed")
            write_scalar(st->speed);
        else if (f == "count")
            write_scalar(st->count);
        else if (f == "brightness")
            write_scalar(st->brightness);
        else if (f == "color") {
            // `color` is 0..255 in the JSON; the init op divides by 255.
            write_vec3(st->color, 255.0f);
            st->overColor = true;
        } else if (f == "colorn") {
            write_vec3(st->colorn, 1.0f);
            st->overColorn = true;
        } else if (f.starts_with("controlpoint") && ! f.starts_with("controlpointangle")) {
            int idx = -1;
            try {
                idx = std::stoi(f.substr(std::string_view("controlpoint").size()));
            } catch (...) {
            }
            if (idx >= 0 && idx < 8) write_vec3(st->controlpoint[idx], 1.0f);
        } else if (f.starts_with("controlpointangle")) {
            int idx = -1;
            try {
                idx = std::stoi(f.substr(std::string_view("controlpointangle").size()));
            } catch (...) {
            }
            if (idx >= 0 && idx < 8) write_vec3(st->controlpointangle[idx], 1.0f);
        }
    }
}

void ApplyUserPropertyToSoundVolume(Scene& scene, const std::string& key, const Json& prop) {
    auto it = scene.sound_volume_user_index.find(key);
    if (it == scene.sound_volume_user_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok || coerced.value.size() < usize(1)) return;
    const float volume = std::clamp(coerced.value[usize()], 0.0f, 1.0f);
    for (auto& control : it->second) {
        if (control) control->SetVolume(volume);
    }
}

void ApplyUserPropertyToCameraPath(Scene& scene, const std::string& key, const Json& prop) {
    scene.ApplyUserCameraPathVisibilityBindings(key, prop);
}

bool ApplyUserPropertyToNodeVisibility(Scene& scene, const std::string& key, const Json& prop) {
    return scene.ApplyUserNodeVisibilityBindings(key, prop);
}

void MergeProjectUserProperties(const std::filesystem::path& project_dir, rstd::json::Map& out) {
    const auto    project_path = project_dir / "project.json";
    std::ifstream is(project_path);
    if (! is) return;

    std::string source(std::istreambuf_iterator<char>(is), {});
    auto        parsed = ParseJson(source, { .allow_comments = true });
    if (parsed.is_err()) {
        rstd_warn("Can't parse {}: {}", project_path.string(), parsed.unwrap_err());
        return;
    }
    auto root    = parsed.unwrap();
    auto general = root.get("general");
    if (general.is_none()) return;
    auto properties = (*general)->get("properties");
    if (properties.is_none()) return;
    auto object = (*properties)->as_object();
    if (object.is_none()) return;

    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  raw_key           = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& value             = *entry_value;
        std::string key               = CanonicalUserPropertyKey(raw_key);
        auto        current           = out.get(rstd::cppstd::as_str(key));
        auto        descriptor = current.is_some() ? MergeUserPropertyDescriptor(value, **current)
                                                   : MakeUserPropertyDescriptor(value.clone());
        out.insert(::alloc::string::String::make(rstd::cppstd::as_str(key)), std::move(descriptor));
    });
}

rstd::json::Map NormalizeUserProperties(const rstd::json::Map& input) {
    auto out = rstd::json::Map::make();
    input.iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  key               = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& value             = *entry_value;
        std::string canonical         = CanonicalUserPropertyKey(key);
        if (key == canonical || out.get(rstd::cppstd::as_str(canonical)).is_none()) {
            out.insert(::alloc::string::String::make(rstd::cppstd::as_str(canonical)),
                       InitialUserProperty(value.clone()));
        }
    });
    return out;
}

} // namespace

using MainSender     = rstd::sync::mpsc::Sender<MainMsg>;
using MainReceiver   = rstd::sync::mpsc::Receiver<MainMsg>;
using RenderSender   = rstd::sync::mpsc::Sender<RenderMsg>;
using RenderReceiver = rstd::sync::mpsc::Receiver<RenderMsg>;

class SceneRenderController;

class SceneRuntimeController {
public:
    SceneRuntimeController();
    ~SceneRuntimeController();

    bool init();
    auto renderController() const { return m_render_controller.get(); }
    bool inited() const { return m_inited; }

    void post(MainMsg);
    void post(RenderMsg);

    void on(MainLoadScene&&);
    void on(MainConfigure&&);
    void on(MainSetFps&&);
    void on(MainSetVolume&&);
    void on(MainSetVolumeScale&&);
    void on(MainSetMuted&&);
    void on(MainSetFillMode&&);
    void on(MainSetSpeed&&);
    void on(MainSetUserProperty&&);
    void on(MainSetFirstFrameCallback&&);
    void on(MainSetUserPropertyDiagnosticCallback&&);
    void on(MainUserPropertyDiagnostics&&);
    void on(MainPreparedPassDiagnostics&&);
    void on(MainStop&&);
    void on(MainPauseAudio&&);
    void on(MainFirstFrame&&);

    bool isGenGraphviz() const { return m_config.graphviz; }

    void setOnClearColor(ClearColorCallback cb) { m_clear_color_cb = std::move(cb); }

private:
    MainSender sender() const;
    void       startMainLoop();
    void       stopMainLoop();
    void       loadScene();

    bool m_inited { false };

    SceneWallpaperConfig m_config;
    rstd::json::Map      m_user_properties;

    WPSceneParser                                m_scene_parser;
    std::unique_ptr<wavsen::audio::SoundManager> m_sound_manager;
    FirstFrameCallback                           m_first_frame_callback;
    UserPropertyDiagnosticCallback               m_user_property_diagnostic_cb;
    ClearColorCallback                           m_clear_color_cb;
    uint64_t                                     m_audio_pause_generation { 0 };

    std::optional<MainSender>              m_main_tx;
    std::optional<MainReceiver>            m_main_rx;
    std::thread                            m_main_thread;
    std::unique_ptr<SceneRenderController> m_render_controller;
};

class SceneRenderController {
public:
    explicit SceneRenderController(SceneRuntimeController& main)
        : m_main(main), m_render(Box<vulkan::VulkanRender>::make()) {
        auto [tx, rx] = rstd::sync::mpsc::channel<RenderMsg>();
        m_tx.emplace(std::move(tx));
        m_rx.emplace(std::move(rx));
    }
    ~SceneRenderController() {
        stop();
        m_render->destroy();
        rstd_info("render handler deleted");
    }

    void start();
    void stop();
    void post(RenderMsg);
    auto sender() const -> RenderSender;

    void on(RenderInit&&);
    void on(RenderSetScene&&);
    void on(RenderSetFillMode&&);
    void on(RenderSetSpeed&&);
    void on(RenderSetUserProperty&&);
    void on(RenderSetMediaStatus&&);
    void on(RenderSetAudioResponseDemandCallback&&);
    void on(RenderSetAudioResponseEnabled&&);
    void on(RenderSetAudioSpectrum&&);
    void on(RenderStop&&);
    void on(RenderDraw&&);
    void on(RenderSwapchainReady&&);
    void on(RenderRequestPreparedPassDiagnostics&&);

    ExSwapchain* exSwapchain() const { return m_render->exSwapchain(); }
    int          takeLastFrameSyncFd() { return m_render->takeLastFrameSyncFd(); }
    bool         getDrmRenderNode(uint32_t& major, uint32_t& minor) const {
        return m_render->getDrmRenderNode(major, minor);
    }
    const vulkan::VulkanRender* render() const { return m_render.as_ptr().as_raw_ptr(); }

    bool renderInited() const { return m_render->inited(); }

    void setMousePos(double x, double y) { m_mouse_pos.store(std::array { (float)x, (float)y }); }

    // Edge-events for the cursor button stream. Each call from the input
    // thread sets/clears the held bit and records the edge so the next
    // TickSceneScripts can fire cursorDown/Up. fetch_or guards against
    // press-release-press coalescing between ticks (rare).
    void setMouseButton(int button, bool down) {
        if (button < 0 || button > 31) return;
        const uint32_t mask = 1u << button;
        if (down) {
            m_buttons_down.fetch_or(mask);
            m_buttons_pressed.fetch_or(mask);
        } else {
            m_buttons_down.fetch_and(~mask);
            m_buttons_released.fetch_or(mask);
        }
    }
    void     setMouseInWindow(bool in) { m_cursor_in_window.store(in); }
    uint32_t buttonsDown() const { return m_buttons_down.load(); }
    uint32_t consumePressed() { return m_buttons_pressed.exchange(0); }
    uint32_t consumeReleased() { return m_buttons_released.exchange(0); }
    bool     cursorInWindow() const { return m_cursor_in_window.load(); }

    void setMainSender(MainSender main_tx) { m_main_tx.emplace(std::move(main_tx)); }

    FrameTimer frame_timer { [] {
    } };
    FpsCounter fps_counter;

private:
    void rebuildRenderGraph(vulkan::RenderGraphResourceRetention retention, bool evict_meshes);
    void consumeDirtyEventsCoveredByGraphRebuild();
    void refreshPreparedMeshDirtyEvents();
    void refreshPreparedMaterialDirtyEvents();

    SceneRuntimeController& m_main;

    Box<vulkan::VulkanRender>              m_render;
    std::shared_ptr<Scene>                 m_scene { nullptr };
    std::shared_ptr<WPUniformRuntimeInput> m_uniform_input;
    RenderSceneSnapshot                    m_render_scene;
    Option<Box<rg::RenderGraph>>           m_rg;
    float                                  m_speed { 1.0f };
    FillMode                               m_fillmode { FillMode::ASPECTCROP };
    bool                                   m_stopped { false };
    AudioResponseDemandCallback            m_audio_response_demand_callback;
    bool                                   m_audio_response_enabled { true };
    std::array<float, 64>                  m_audio_left {};
    std::array<float, 64>                  m_audio_right {};
    std::chrono::steady_clock::time_point  m_audio_received {};
    bool                                   m_audio_primed { false };

    std::atomic<std::array<float, 2>> m_mouse_pos { std::array { 0.5f, 0.5f } };
    std::atomic<uint32_t>             m_buttons_down { 0 };
    std::atomic<uint32_t>             m_buttons_pressed { 0 };
    std::atomic<uint32_t>             m_buttons_released { 0 };
    std::atomic<bool>                 m_cursor_in_window { false };

    std::optional<RenderSender>   m_tx;
    std::optional<RenderReceiver> m_rx;
    std::thread                   m_thread;
    std::optional<MainSender>     m_main_tx;

    // Strong ref kept here, weak copy captured by the swapchain callback.
    std::shared_ptr<RenderSender> m_swapchain_tx;
};

auto SceneRenderController::sender() const -> RenderSender {
    if (! m_tx) rstd::panic { "render mailbox is stopped" };
    return *m_tx;
}

void SceneRenderController::post(RenderMsg msg) {
    if (m_tx) (void)m_tx->send(std::move(msg));
}

void SceneRenderController::start() {
    if (m_thread.joinable()) return;
    if (! m_rx) rstd::panic { "render mailbox cannot be restarted" };

    RenderReceiver rx(std::move(*m_rx));
    m_rx.reset();
    m_thread = std::thread([this, rx = std::move(rx)]() mutable {
        rstd_info("render loop started");
        while (true) {
            auto received = rx.recv();
            if (received.is_err()) break;

            auto message  = std::move(received).unwrap();
            bool shutdown = false;
            std::visit(
                [this, &shutdown](auto&& value) {
                    using Message = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::is_same_v<Message, RenderShutdown>) {
                        frame_timer.Stop();
                        frame_timer.SetCallback([] {
                        });
                        m_swapchain_tx.reset();
                        shutdown = true;
                    } else {
                        on(std::move(value));
                    }
                },
                std::move(message.v));
            if (shutdown) break;
        }
        rstd_info("render loop stopped");
    });
}

void SceneRenderController::stop() {
    if (! m_thread.joinable()) {
        frame_timer.Stop();
        frame_timer.SetCallback([] {
        });
        m_swapchain_tx.reset();
        m_tx.reset();
        m_rx.reset();
        m_main_tx.reset();
        return;
    }
    if (std::this_thread::get_id() == m_thread.get_id()) {
        rstd::panic { "SceneRenderController destroyed from render thread" };
    }

    post(RenderMsg { RenderShutdown {} });
    m_thread.join();
    m_tx.reset();
    m_main_tx.reset();
}

// ---- SceneRenderController message handlers ---------------------------------

void SceneRenderController::on(RenderStop&& m) {
    m_stopped = m.stop;
    if (m.stop)
        frame_timer.Stop();
    else
        frame_timer.Run();
}

void SceneRenderController::on(RenderDraw&&) {
    frame_timer.FrameBegin();
    if (m_rg.is_some()) {
        {
            auto pos                 = m_mouse_pos.load();
            m_scene->pointerPosition = rstd::array<float, 2> { pos[0], pos[1] };
            if (m_uniform_input) m_uniform_input->SetPointerInput(pos[0], pos[1]);
        }
        // Drive any per-Scene scenescripts before particle emission.
        // Scripts mutate SceneNode transforms (scale/origin/angles) so
        // they need to run before the matrix-derivation in the
        // uniform evaluation runs inside drawFrame after scripts have updated Scene state.
        // The runtime is a no-op when no ScriptScene is installed.
        {
            owe::script::FrameInputs fi;
            fi.frametime =
                static_cast<float>(m_scene->Runtime().Frame().delta.to_primitive() * m_speed);
            fi.runtime  = static_cast<float>(m_scene->Runtime().Frame().elapsed.to_primitive());
            fi.canvas_w = static_cast<float>(m_scene->ortho[0]);
            fi.canvas_h = static_cast<float>(m_scene->ortho[1]);
            fi.screen_w = fi.canvas_w;
            fi.screen_h = fi.canvas_h;
            {
                auto pos    = m_mouse_pos.load();
                fi.cursor_x = pos[0];
                fi.cursor_y = pos[1];
            }
            fi.cursor_in_window             = cursorInWindow();
            fi.mouse_buttons_down           = buttonsDown();
            fi.mouse_buttons_pressed        = consumePressed();
            fi.mouse_buttons_released       = consumeReleased();
            constexpr auto kAudioStaleAfter = std::chrono::milliseconds(250);
            const bool     stale =
                ! m_audio_primed ||
                std::chrono::steady_clock::now() - m_audio_received > kAudioStaleAfter;
            if (! stale) {
                fi.audio_left  = m_audio_left;
                fi.audio_right = m_audio_right;
                for (std::size_t i = 0; i < fi.audio_average.size(); ++i) {
                    fi.audio_average[i] = (fi.audio_left[i] + fi.audio_right[i]) * 0.5f;
                }
            }
            if (m_uniform_input) {
                m_uniform_input->SetAudioSpectrum(
                    rstd::slice<float>::from_raw_parts(fi.audio_left.data(),
                                                       usize(fi.audio_left.size())),
                    rstd::slice<float>::from_raw_parts(fi.audio_right.data(),
                                                       usize(fi.audio_right.size())));
            }
            m_scene->TickNodeFieldAnimations();
            owe::script::TickSceneScripts(*m_scene, fi);
            m_scene->TickCameraPaths();
            m_scene->TickMaterialShaderAnimations();
            m_scene->TickTransformUpdaters();
            if (m_scene->ConsumeRenderGraphDirty()) {
                rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
            }
        }
        m_scene->paritileSys->Emitt();
        refreshPreparedMeshDirtyEvents();
        refreshPreparedMaterialDirtyEvents();

        /* Advance video textures (no-op if none) before drawFrame so
         * the new RGBA frame is sampled by the same render pass. */
        m_render->pumpVideoTextures(frame_timer.TargetFrameTime() * m_speed);

        /* Upload any glyph rects the actuators added this tick. Runs after
         * TickSceneScripts (which calls FontFace::Populate) and before
         * drawFrame so newly-rasterised glyphs are visible the same frame. */
        m_render->pumpFontAtlases(*m_scene);

        m_render->drawFrame(*m_scene);

        m_scene->PassFrameTime(frame_timer.TargetFrameTime() * m_speed);

        if (! m_scene->first_frame_ok) {
            m_scene->first_frame_ok = true;
            if (m_main_tx) (void)m_main_tx->send(MainMsg { MainFirstFrame {} });
        }
    }
    frame_timer.FrameEnd();
}

void SceneRenderController::on(RenderSetFillMode&& m) {
    m_fillmode = m.mode;
    if (m_scene && renderInited()) {
        m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    }
}

void SceneRenderController::rebuildRenderGraph(vulkan::RenderGraphResourceRetention retention,
                                               bool                                 evict_meshes) {
    if (! m_scene || ! renderInited()) return;
    if (m_rg.is_some()) m_render->clearLastRenderGraph(retention);
    if (evict_meshes) m_render->evictUnusedMeshes();
    m_render->configureRenderTargets(*m_scene);
    m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
    m_rg           = Some(sceneToRenderGraph(*m_scene, m_render_scene));

    if (m_main.isGenGraphviz()) (*m_rg)->ToGraphviz("graph.dot");
    m_render->compileRenderGraph(*m_scene, **m_rg, m_render_scene);
    m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    consumeDirtyEventsCoveredByGraphRebuild();
    (void)m_scene->ConsumeRenderGraphDirty();
}

void SceneRenderController::consumeDirtyEventsCoveredByGraphRebuild() {
    if (! m_scene) return;
    (void)m_scene->ConsumePreparedMaterialDirtyEvents();
    (void)m_scene->ConsumePreparedMeshDirtyEvents();
}

void SceneRenderController::refreshPreparedMeshDirtyEvents() {
    if (! m_scene || ! renderInited() || m_rg.is_none()) return;
    auto events = m_scene->ConsumePreparedMeshDirtyEvents();
    if (events.empty()) return;

    bool requires_graph_rebuild = std::any_of(events.begin(), events.end(), [](const auto& event) {
        return (event.flags & SceneMeshDirtyLayout) != 0;
    });
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }

    m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
    for (const auto& event : events) {
        if ((event.flags & SceneMeshDirtyData) == 0) continue;
        m_render->refreshPreparedMesh(
            *m_scene,
            m_render_scene,
            event.mesh,
            vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Resources));
    }
}

void SceneRenderController::refreshPreparedMaterialDirtyEvents() {
    if (! m_scene || ! renderInited() || m_rg.is_none()) return;
    auto events = m_scene->ConsumePreparedMaterialDirtyEvents();
    if (events.empty()) return;

    bool requires_graph_rebuild = std::any_of(events.begin(), events.end(), [](const auto& event) {
        return (event.flags & SceneMaterialDirtyGraph) != 0;
    });
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }

    m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
    for (const auto& event : events) {
        auto flags = MaterialDirtyToPassInvalidationFlags(event.flags);
        if (flags == vulkan::PassInvalidationNone) continue;
        m_render->refreshPreparedMaterial(*m_scene, m_render_scene, event.material, flags);
    }
}

void SceneRenderController::on(RenderSetScene&& m) {
    if (m_scene && m_scene->audioResponseDemand) {
        m_scene->audioResponseDemand->SetCallback({});
    }
    m_scene         = std::move(m.scene);
    m_uniform_input = std::move(m.uniform_input);
    m_audio_primed  = false;
    m_audio_left.fill(0.0f);
    m_audio_right.fill(0.0f);
    if (m_scene && m_scene->audioResponseDemand) {
        m_scene->audioResponseDemand->SetEnabled(m_audio_response_enabled);
        m_scene->audioResponseDemand->SetCallback(m_audio_response_demand_callback);
    }
    rebuildRenderGraph(vulkan::RenderGraphResourceRetention::ReleaseSceneTextures, true);
}

void SceneRenderController::on(RenderSetSpeed&& m) { m_speed = m.speed; }

void SceneRenderController::on(RenderSetUserProperty&& m) {
    if (! m_scene) return;
    std::string key                      = CanonicalUserPropertyKey(m.key);
    const bool  has_shader_combo_binding = m_scene->shader_combo_user_index.contains(key);
    owe::script::SetSceneUserProperty(*m_scene, key, m.property);
    ApplyUserPropertyToClear(*m_scene, key, m.property);
    ApplyUserPropertyToShaderUniforms(*m_scene, key, m.property);
    auto texture_materials = ApplyUserPropertyToMaterialTextures(*m_scene, key, m.property);
    bool shader_combo_requires_graph = ApplyUserPropertyToShaderCombos(*m_scene, key, m.property);
    ApplyUserPropertyToImageColor(*m_scene, key, m.property);
    ApplyUserPropertyToImageAlpha(*m_scene, key, m.property);
    m_scene->ApplyUserTextBindings(key, m.property);
    ApplyUserPropertyToParticles(*m_scene, key, m.property);
    ApplyUserPropertyToSoundVolume(*m_scene, key, m.property);
    m_scene->ApplyUserPropertyBindings(key, m.property);
    ApplyUserPropertyToCameraPath(*m_scene, key, m.property);
    bool requires_graph_rebuild = ApplyUserPropertyToNodeVisibility(*m_scene, key, m.property);
    requires_graph_rebuild =
        m_scene->ApplyUserImageEffectVisibilityBindings(key, m.property) || requires_graph_rebuild;
    requires_graph_rebuild = requires_graph_rebuild || shader_combo_requires_graph;

    if (! texture_materials.empty() && renderInited() && m_rg.is_some() &&
        ! requires_graph_rebuild) {
        m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
        if (! m_render->refreshPreparedMaterialTextures(
                *m_scene, m_render_scene, texture_materials)) {
            requires_graph_rebuild = true;
        }
    }
    if (has_shader_combo_binding && m_main_tx) {
        auto diagnostics = CollectUserPropertyDiagnostics(*m_scene, key);
        (void)m_main_tx->send(
            MainMsg { MainUserPropertyDiagnostics { .diagnostics = std::move(diagnostics) } });
    }
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }
    if (renderInited() && m_rg.is_some()) refreshPreparedMaterialDirtyEvents();
}

void SceneRenderController::on(RenderSetMediaStatus&& m) {
    if (! m_scene) return;

    owe::script::SetSceneMediaStatus(*m_scene, ToScriptMediaStatus(m.status));

    std::vector<SceneMaterialId> texture_materials;
    for (auto material : ApplyUserPropertyToMaterialTextures(
             *m_scene, "$mediaThumbnail", RuntimeTextureProperty(m.status.art_url))) {
        PushUniqueMaterialId(texture_materials, material);
    }
    for (auto material :
         ApplyUserPropertyToMaterialTextures(*m_scene,
                                             "$mediaPreviousThumbnail",
                                             RuntimeTextureProperty(m.status.previous_art_url))) {
        PushUniqueMaterialId(texture_materials, material);
    }

    bool requires_graph_rebuild = false;
    if (! texture_materials.empty() && renderInited() && m_rg.is_some()) {
        m_render_scene = ExtractRenderSceneSnapshot(*m_scene);
        if (! m_render->refreshPreparedMaterialTextures(
                *m_scene, m_render_scene, texture_materials)) {
            requires_graph_rebuild = true;
        }
    }
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }
    if (renderInited() && m_rg.is_some()) refreshPreparedMaterialDirtyEvents();
}

void SceneRenderController::on(RenderSetAudioResponseDemandCallback&& m) {
    m_audio_response_demand_callback = std::move(m.callback);
    if (m_scene && m_scene->audioResponseDemand) {
        m_scene->audioResponseDemand->SetCallback(m_audio_response_demand_callback);
    }
}

void SceneRenderController::on(RenderSetAudioResponseEnabled&& m) {
    m_audio_response_enabled = m.enabled;
    if (m_scene && m_scene->audioResponseDemand) {
        m_scene->audioResponseDemand->SetEnabled(m.enabled);
    }
}

void SceneRenderController::on(RenderSetAudioSpectrum&& m) {
    auto sanitize = [](float value) {
        if (! std::isfinite(value)) return 0.0f;
        return std::clamp(value, 0.0f, 1.0f);
    };
    for (std::size_t i = 0; i < m_audio_left.size(); ++i) {
        m_audio_left[i]  = sanitize(m.left[i]);
        m_audio_right[i] = sanitize(m.right[i]);
    }
    m_audio_received = m.received;
    m_audio_primed   = true;
}

void SceneRenderController::on(RenderInit&& m) {
    m_render->init(std::move(*m.info));

    // Subscribe to ExSwapchain ready/extent/format changes. The
    // callback runs on the render thread (sync for Local, from
    // drainPendingDirective for Bridge); we just relay it as a
    // RenderSwapchainReady message back to ourselves so the actual
    // handling happens through the normal loop path. Format reaches
    // VulkanRender via ExSwapchain::format() directly; no need to
    // round-trip it through this message.
    if (auto* sw = m_render->exSwapchain()) {
        if (m_tx) {
            m_swapchain_tx                   = std::make_shared<RenderSender>(*m_tx);
            std::weak_ptr<RenderSender> weak = m_swapchain_tx;
            sw->setOnReadyChanged([weak](const ExSwapchainReadyEvent& e) {
                if (auto tx = weak.lock()) {
                    (void)tx->send(
                        RenderMsg { RenderSwapchainReady { e.ready, e.width, e.height } });
                }
            });
        }
    }

    // inited, callback to load scene
    if (m_main_tx) (void)m_main_tx->send(MainMsg { MainLoadScene {} });
}

void SceneRenderController::on(RenderSwapchainReady&& m) {
    if (! m.ready) {
        frame_timer.Stop();
        return;
    }
    bool extent_changed = m_render->onSwapchainReady(m.width, m.height);
    if (extent_changed && m_scene && m_rg.is_some()) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
    }
    if (m_stopped)
        frame_timer.Stop();
    else
        frame_timer.Run();
}

void SceneRenderController::on(RenderRequestPreparedPassDiagnostics&& m) {
    if (! m_main_tx) return;
    auto diagnostics = m_render->preparedPassDiagnostics();
    (void)m_main_tx->send(MainMsg { MainPreparedPassDiagnostics {
        .cb          = std::move(m.cb),
        .diagnostics = std::move(diagnostics),
    } });
}

auto SceneRuntimeController::sender() const -> MainSender {
    if (! m_main_tx) rstd::panic { "main mailbox is stopped" };
    return *m_main_tx;
}

void SceneRuntimeController::post(MainMsg msg) {
    if (m_main_tx) (void)m_main_tx->send(std::move(msg));
}

void SceneRuntimeController::post(RenderMsg msg) { m_render_controller->post(std::move(msg)); }

void SceneRuntimeController::startMainLoop() {
    if (m_main_thread.joinable()) return;
    if (! m_main_rx) rstd::panic { "main mailbox cannot be restarted" };

    MainReceiver rx(std::move(*m_main_rx));
    m_main_rx.reset();
    m_main_thread = std::thread([this, rx = std::move(rx)]() mutable {
        rstd_info("main loop started");
        while (true) {
            auto received = rx.recv();
            if (received.is_err()) break;

            auto message  = std::move(received).unwrap();
            bool shutdown = false;
            std::visit(
                [this, &shutdown](auto&& value) {
                    using Message = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::is_same_v<Message, MainShutdown>) {
                        shutdown = true;
                    } else {
                        on(std::move(value));
                    }
                },
                std::move(message.v));
            if (shutdown) break;
        }
        rstd_info("main loop stopped");
    });
}

void SceneRuntimeController::stopMainLoop() {
    if (! m_main_thread.joinable()) {
        m_main_tx.reset();
        m_main_rx.reset();
        return;
    }
    if (std::this_thread::get_id() == m_main_thread.get_id()) {
        rstd::panic { "SceneRuntimeController destroyed from main thread" };
    }

    post(MainMsg { MainShutdown {} });
    m_main_thread.join();
    m_main_tx.reset();
}

// ---- SceneRuntimeController message handlers --------------------------------

void SceneRuntimeController::on(MainLoadScene&&) {
    if (m_render_controller->renderInited()) {
        loadScene();
    }
}

void SceneRuntimeController::on(MainConfigure&& m) {
    m_config          = std::move(m.config);
    m_user_properties = NormalizeUserProperties(m_config.user_properties);
    on(MainSetFps { m_config.fps });
    on(MainSetVolume { m_config.volume });
    on(MainSetVolumeScale { 1.0f });
    on(MainSetMuted { m_config.muted });
    on(MainSetFillMode { m_config.fill_mode });
    on(MainSetSpeed { m_config.speed });
    on(MainLoadScene {});
}

void SceneRuntimeController::on(MainSetFps&& m) {
    m_config.fps = m.fps;
    if (m.fps >= 5) {
        m_render_controller->frame_timer.SetRequiredFps(u16(static_cast<rstd::uint16_t>(m.fps)));
    }
}

void SceneRuntimeController::on(MainSetVolume&& m) {
    m_config.volume = m.volume;
    m_sound_manager->set_volume(m.volume);
}

void SceneRuntimeController::on(MainSetVolumeScale&& m) {
    m_sound_manager->set_volume_scale(m.scale, m.fade_ms);
}

void SceneRuntimeController::on(MainSetMuted&& m) {
    m_config.muted = m.muted;
    m_sound_manager->set_muted(m.muted);
}

void SceneRuntimeController::on(MainSetFillMode&& m) {
    m_config.fill_mode = m.mode;
    m_render_controller->post(RenderMsg { RenderSetFillMode { m.mode } });
}

void SceneRuntimeController::on(MainSetSpeed&& m) {
    m_config.speed = m.speed;
    m_render_controller->post(RenderMsg { RenderSetSpeed { m.speed } });
}

void SceneRuntimeController::on(MainSetUserProperty&& m) {
    const std::string property = CanonicalUserPropertyKey(m.key);
    auto              current  = m_user_properties.get(rstd::cppstd::as_str(property));
    Json              prop     = current.is_some() ? MergeUserPropertyDescriptor(**current, m.value)
                                                   : MakeUserPropertyDescriptor(std::move(m.value));
    m_config.user_properties.insert(::alloc::string::String::make(rstd::cppstd::as_str(property)),
                                    prop.clone());
    m_user_properties.insert(::alloc::string::String::make(rstd::cppstd::as_str(property)),
                             prop.clone());
    m_render_controller->post(RenderMsg { RenderSetUserProperty { property, std::move(prop) } });
}

void SceneRuntimeController::on(MainSetFirstFrameCallback&& m) {
    m_first_frame_callback = std::move(m.cb);
}

void SceneRuntimeController::on(MainSetUserPropertyDiagnosticCallback&& m) {
    m_user_property_diagnostic_cb = std::move(m.cb);
}

void SceneRuntimeController::on(MainUserPropertyDiagnostics&& m) {
    if (m_user_property_diagnostic_cb) m_user_property_diagnostic_cb(std::move(m.diagnostics));
}

void SceneRuntimeController::on(MainPreparedPassDiagnostics&& m) {
    if (m.cb) m.cb(std::move(m.diagnostics));
}

void SceneRuntimeController::on(MainStop&& m) {
    const uint64_t generation = ++m_audio_pause_generation;
    if (m.stop) {
        if (m.scale_audio) m_sound_manager->set_volume_scale(0.0f, m.fade_ms);
        if (m.fade_ms == 0 || ! m.scale_audio) {
            m_sound_manager->pause();
        } else {
            auto     tx    = sender();
            uint32_t delay = m.fade_ms;
            std::thread([tx = std::move(tx), generation, delay]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                (void)tx.send(MainMsg { MainPauseAudio { generation } });
            }).detach();
        }
    } else {
        m_sound_manager->play();
        if (m.scale_audio) m_sound_manager->set_volume_scale(1.0f, m.fade_ms);
    }
    m_render_controller->post(RenderMsg { RenderStop { m.stop } });
}

void SceneRuntimeController::on(MainPauseAudio&& m) {
    if (m.generation == m_audio_pause_generation) m_sound_manager->pause();
}

void SceneRuntimeController::on(MainFirstFrame&&) {
    if (m_first_frame_callback) m_first_frame_callback();
}

void SceneRuntimeController::loadScene() {
    if (m_config.source_pkg_path.empty() || m_config.assets_dir.empty()) return;

    rstd_info("loading scene: {}", m_config.source_pkg_path);

    if (! m_sound_manager->is_inited()) {
        m_sound_manager->init();
        m_sound_manager->play();
    } else {
        m_sound_manager->unmount_all();
    }

    std::shared_ptr<Scene> scene { nullptr };

    // mount assets dir
    std::unique_ptr<fs::VFS> pVfs = std::make_unique<fs::VFS>();
    auto&                    vfs  = *pVfs;
    if (! vfs.is_mounted("assets")) {
        auto assets = fs::make_physical_fs(fs::ToPath(m_config.assets_dir));
        if (assets.is_err() ||
            vfs.mount("/assets", rstd::move(assets).unwrap_unchecked(), "assets").is_err()) {
            rstd_error("Mount assets dir failed");
            return;
        }
    }
    std::filesystem::path pkgPath_fs { m_config.source_pkg_path };
    pkgPath_fs.replace_extension("pkg");
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgEntry = pkgPath_fs.filename().replace_extension("json").native();
    std::string pkgDir   = pkgPath_fs.parent_path().native();
    std::string scene_id = pkgPath_fs.parent_path().filename().native();
    MergeProjectUserProperties(pkgPath_fs.parent_path(), m_user_properties);

    // load pkgfile. Read pkg version stamp before move-mounting so we can
    // pass it to the scene parser; on fallback (loose dir) we have no
    // version info and use kSceneVersionUnknown.
    wpscene::SceneVersion pkg_v       = wpscene::kSceneVersionUnknown;
    auto                  wfs         = fs::WPPkgFs::open(fs::ToPath(pkgPath));
    bool                  pkg_mounted = false;
    if (wfs.is_ok()) {
        auto stamp  = wfs->pkg_version_stamp();
        pkg_v       = wpscene::ParsePkgVersionStamp(std::string_view(
            reinterpret_cast<const char*>(stamp.data()), stamp.size().to_primitive()));
        pkg_mounted = vfs.mount("/assets", wfs->mount_handle()).is_ok();
    }
    if (! pkg_mounted) {
        rstd_info("load pkg file {} failed, fallback to use dir", pkgPath);
        pkg_v = wpscene::kSceneVersionUnknown;
        // load pkg dir
        auto loose = fs::make_physical_fs(fs::ToPath(pkgDir));
        if (loose.is_err() || vfs.mount("/assets", rstd::move(loose).unwrap_unchecked()).is_err()) {
            rstd_error("can't load pkg directory: {}", pkgDir);
            return;
        }
    }
    if (! m_config.cache_dir.empty()) {
        auto cache = fs::make_physical_fs(fs::ToPath(m_config.cache_dir), true);
        if (cache.is_err() ||
            vfs.mount("/cache", rstd::move(cache).unwrap_unchecked(), "cache").is_err()) {
            rstd_error("can't load cache folder: {}", m_config.cache_dir);
        } else {
            rstd_info("cache folder: {}", m_config.cache_dir);
        }
    }

    {
        const std::string base { "/assets/" };
        auto              scene_doc = m_config.scene_document;
        if (! scene_doc) {
            auto loaded = wpscene::LoadSceneDocumentFromVfs(vfs, base + pkgEntry, pkg_v);
            if (loaded) scene_doc = std::make_shared<wpscene::SceneDocument>(std::move(*loaded));
        }
        if (! scene_doc) {
            rstd_error("Not supported scene type");
            return;
        }
        // Hand the (already-merged project.json defaults + any host-supplied
        // overrides) user-property map to the parser so visible-binding
        // pruning sees the user's saved values, not the scene.json defaults.
        m_scene_parser.SetUserProperties(rstd::Some(
            rstd::ref<rstd::json::Map>::from_raw_parts(rstd::addressof(m_user_properties))));
        scene = m_scene_parser.Parse(scene_id, *scene_doc, vfs, *m_sound_manager);
        m_scene_parser.SetUserProperties(rstd::None());
        m_user_properties.iter().for_each([&](auto entry) {
            auto [entry_key, entry_value] = entry;
            auto        key               = rstd::cppstd::to_string(entry_key->as_str());
            const auto& prop              = *entry_value;
            ApplyUserPropertyToClear(*scene, key, prop);
            owe::script::SetSceneUserProperty(*scene, key, prop);
        });
        if (! m_config.cache_dir.empty() && scene) {
            std::filesystem::path ls_dir =
                std::filesystem::path(m_config.cache_dir) / "script_localstorage";
            std::error_code ec;
            std::filesystem::create_directories(ls_dir, ec);
            std::string ls_file = (ls_dir / (scene_id + ".json")).native();
            owe::script::SetScenePersistence(*scene, std::move(ls_file));
        }
        scene->vfs.reset(pVfs.release());

        // Surface the parsed clear color before the scene is shipped
        // off to the render thread; downstream callers (the daemon
        // host) need the value to feed `set_config.clear_*`.
        if (m_clear_color_cb) {
            const auto& c = scene->clearColor;
            m_clear_color_cb(c[usize()], c[usize(1)], c[usize(2)]);
        }
    }

    auto rtx = m_render_controller->sender();
    (void)rtx.send(RenderMsg { RenderSetScene { scene, m_scene_parser.RuntimeInput() } });
    // First-frame default push: now that the render thread owns the scene,
    // replay every collected user property (project.json defaults + any
    // mutations the host already pushed during scene load) so the shader
    // cbuffer matches what the host UI displays.
    m_user_properties.iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto        key               = rstd::cppstd::to_string(entry_key->as_str());
        const auto& prop              = *entry_value;
        (void)rtx.send(RenderMsg { RenderSetUserProperty { rstd::move(key), prop.clone() } });
    });
    // draw first frame
    (void)rtx.send(RenderMsg { RenderDraw {} });
}

bool SceneRuntimeController::init() {
    if (m_inited) return true;

    // Wire render handler senders before starting the loops; otherwise an
    // early RenderInit could fire before they're set.
    m_render_controller->setMainSender(sender());

    startMainLoop();
    m_render_controller->start();

    {
        auto& frameTimer = m_render_controller->frame_timer;
        auto  rtx        = m_render_controller->sender();
        frameTimer.SetCallback([rtx]() mutable {
            (void)rtx.send(RenderMsg { RenderDraw {} });
        });
        frameTimer.SetRequiredFps(u16(15));
        frameTimer.Run();
    }

    m_inited = true;
    return true;
}

SceneRuntimeController::SceneRuntimeController()
    : m_sound_manager(std::make_unique<wavsen::audio::SoundManager>()) {
    auto [tx, rx] = rstd::sync::mpsc::channel<MainMsg>();
    m_main_tx.emplace(std::move(tx));
    m_main_rx.emplace(std::move(rx));
    m_render_controller = std::make_unique<SceneRenderController>(*this);
}

SceneRuntimeController::~SceneRuntimeController() {
    // Stop main before render so no main handler can enqueue more render work.
    if (m_render_controller) m_render_controller->frame_timer.Stop();
    stopMainLoop();
    if (m_render_controller) m_render_controller->stop();
}

} // namespace owe

SceneWallpaper::SceneWallpaper(): m_runtime(std::make_unique<SceneRuntimeController>()) {}

SceneWallpaper::~SceneWallpaper() = default;

bool SceneWallpaper::inited() const { return m_runtime->inited(); }

bool SceneWallpaper::init() { return m_runtime->init(); }

void SceneWallpaper::initVulkan(RenderInitInfo info) {
    m_offscreen = info.offscreen;
    auto sp     = std::make_shared<RenderInitInfo>(std::move(info));
    m_runtime->post(RenderMsg { RenderInit { std::move(sp) } });
}

void SceneWallpaper::play() { m_runtime->post(MainMsg { MainStop { false } }); }
void SceneWallpaper::play(uint32_t fade_ms) {
    m_runtime->post(MainMsg { MainStop { false, fade_ms, true } });
}
void SceneWallpaper::pause() { m_runtime->post(MainMsg { MainStop { true } }); }
void SceneWallpaper::pause(uint32_t fade_ms) {
    m_runtime->post(MainMsg { MainStop { true, fade_ms, true } });
}
void SceneWallpaper::requestFrame() { m_runtime->post(RenderMsg { RenderDraw {} }); }

void SceneWallpaper::mouseInput(double x, double y) {
    m_runtime->renderController()->setMousePos(x, y);
}

void SceneWallpaper::mouseButton(int button, bool down) {
    m_runtime->renderController()->setMouseButton(button, down);
}

void SceneWallpaper::mouseEnter(bool in_window) {
    m_runtime->renderController()->setMouseInWindow(in_window);
}

void SceneWallpaper::configure(SceneWallpaperConfig config) {
    m_runtime->post(MainMsg { MainConfigure { std::move(config) } });
}

void SceneWallpaper::setFps(uint32_t fps) { m_runtime->post(MainMsg { MainSetFps { fps } }); }

void SceneWallpaper::setVolume(float volume) {
    m_runtime->post(MainMsg { MainSetVolume { volume } });
}

void SceneWallpaper::setVolumeScale(float scale) { setVolumeScale(scale, 0); }

void SceneWallpaper::setVolumeScale(float scale, uint32_t fade_ms) {
    m_runtime->post(MainMsg { MainSetVolumeScale { scale, fade_ms } });
}

void SceneWallpaper::setMuted(bool muted) { m_runtime->post(MainMsg { MainSetMuted { muted } }); }

void SceneWallpaper::setFillMode(FillMode mode) {
    m_runtime->post(MainMsg { MainSetFillMode { mode } });
}

void SceneWallpaper::setSpeed(float speed) { m_runtime->post(MainMsg { MainSetSpeed { speed } }); }

void SceneWallpaper::setMediaStatus(MediaStatus status) {
    m_runtime->post(RenderMsg { RenderSetMediaStatus { std::move(status) } });
}

void SceneWallpaper::setAudioResponseDemandCallback(AudioResponseDemandCallback callback) {
    m_runtime->post(RenderMsg { RenderSetAudioResponseDemandCallback { std::move(callback) } });
}

void SceneWallpaper::setAudioResponseEnabled(bool enabled) {
    m_runtime->post(RenderMsg { RenderSetAudioResponseEnabled { enabled } });
}

void SceneWallpaper::setAudioSpectrum(const std::array<float, 64>& left,
                                      const std::array<float, 64>& right) {
    m_runtime->post(RenderMsg { RenderSetAudioSpectrum {
        .left     = left,
        .right    = right,
        .received = std::chrono::steady_clock::now(),
    } });
}

void SceneWallpaper::setUserPropertyRaw(std::string_view name, std::string value) {
    m_runtime->post(MainMsg { MainSetUserProperty { std::string(name), RawUserProperty(value) } });
}

void SceneWallpaper::setUserPropertyJson(std::string_view name, Json value) {
    m_runtime->post(MainMsg { MainSetUserProperty { std::string(name), std::move(value) } });
}

void SceneWallpaper::setOnClearColor(ClearColorCallback cb) {
    m_runtime->setOnClearColor(std::move(cb));
}

void SceneWallpaper::setOnFirstFrame(FirstFrameCallback cb) {
    m_runtime->post(MainMsg { MainSetFirstFrameCallback { std::move(cb) } });
}

void SceneWallpaper::setOnUserPropertyDiagnostics(UserPropertyDiagnosticCallback cb) {
    m_runtime->post(MainMsg { MainSetUserPropertyDiagnosticCallback { std::move(cb) } });
}

void SceneWallpaper::requestPreparedPassDiagnostics(RenderPassDiagnosticCallback cb) {
    m_runtime->post(RenderMsg { RenderRequestPreparedPassDiagnostics { std::move(cb) } });
}

int SceneWallpaper::takeLastFrameSyncFd() {
    return m_runtime->renderController()->takeLastFrameSyncFd();
}

ExSwapchain* SceneWallpaper::exSwapchain() const {
    return m_runtime->renderController()->exSwapchain();
}

bool SceneWallpaper::getDrmRenderNode(uint32_t& out_major, uint32_t& out_minor) const {
    return m_runtime->renderController()->getDrmRenderNode(out_major, out_minor);
}

bool SceneWallpaper::waitVulkanInited(uint32_t timeout_ms) {
    using clock   = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    auto rh       = m_runtime->renderController();
    while (clock::now() < deadline) {
        if (rh->renderInited()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return rh->renderInited();
}

VkInstance SceneWallpaper::vkInstance() const {
    return m_runtime->renderController()->render()->vkInstance();
}
VkPhysicalDevice SceneWallpaper::vkPhysicalDevice() const {
    return m_runtime->renderController()->render()->vkPhysicalDevice();
}
VkDevice SceneWallpaper::vkDevice() const {
    return m_runtime->renderController()->render()->vkDevice();
}
VkQueue SceneWallpaper::vkGraphicsQueue() const {
    return m_runtime->renderController()->render()->vkGraphicsQueue();
}
uint32_t SceneWallpaper::vkGraphicsQueueFamily() const {
    return m_runtime->renderController()->render()->vkGraphicsQueueFamily();
}
void SceneWallpaper::deviceUuid(uint8_t out[16]) const {
    m_runtime->renderController()->render()->deviceUuid(out);
}
void SceneWallpaper::driverUuid(uint8_t out[16]) const {
    m_runtime->renderController()->render()->driverUuid(out);
}
