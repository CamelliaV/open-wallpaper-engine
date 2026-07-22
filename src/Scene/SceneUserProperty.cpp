module;

#include <rstd/macro.hpp>

module wescene.scene_user_property;

import eigen;
import rstd;
import rstd.cppstd;
import rstd.log;
import wescene.fs;
import wescene.pkg.parse;
import wescene.script;
import wescene.spec_names;

using namespace rstd::prelude;

namespace owe
{
namespace
{

constexpr std::string_view kSchemeColorKey          = "schemecolor";
constexpr std::string_view kWaywallenSchemeColorKey = "waywallen.scheme_color";

bool ParseFloatList(std::string_view source, std::vector<float>& out) {
    out.clear();
    std::size_t offset = 0;
    while (offset < source.size()) {
        while (offset < source.size() && (source[offset] == ' ' || source[offset] == '\t'))
            ++offset;
        if (offset >= source.size()) break;
        std::size_t start = offset;
        while (offset < source.size() && source[offset] != ' ' && source[offset] != '\t') ++offset;
        try {
            out.push_back(std::stof(std::string(source.substr(start, offset - start))));
        } catch (...) {
            return false;
        }
    }
    return ! out.empty();
}

struct UserPropertyCoerceResult {
    bool        ok { false };
    ShaderValue value;
    const char* skip_reason { nullptr };
};

UserPropertyCoerceResult CoerceUserPropertyValue(const Json& property) {
    UserPropertyCoerceResult result;

    std::string type;
    if (auto member = property.get("type"); member.is_some()) {
        auto string = (*member)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }

    if (type == "combo") {
        result.skip_reason = "shader graph mutation is not a uniform update";
        return result;
    }
    if (type == "texture" || type == "replacetexture" || type == "file" || type == "textinput") {
        result.skip_reason = "non-uniform property type";
        return result;
    }

    auto        value = property.get("value");
    const Json& raw   = value.is_some() ? **value : property;
    if (type == "color") {
        std::vector<float> values;
        auto               string = raw.as_str();
        if (string.is_some() && ParseFloatList(rstd::cppstd::as_string_view(*string), values) &&
            values.size() >= 3) {
            result.ok    = true;
            result.value = ShaderValue(std::span<const float>(values.data(), values.size()));
            return result;
        }
        result.skip_reason = "color value not a 'r g b[ a]' float string";
        return result;
    }

    if (raw.is_boolean()) {
        result.ok    = true;
        result.value = ShaderValue(*raw.as_bool() ? 1.0f : 0.0f);
        return result;
    }
    if (raw.is_number()) {
        auto number = raw.as_f64();
        if (number.is_some()) {
            const double native = number->to_primitive();
            result.ok           = native >= std::numeric_limits<float>::lowest() &&
                                  native <= std::numeric_limits<float>::max();
            if (result.ok) result.value = ShaderValue(static_cast<float>(native));
        }
        return result;
    }
    if (raw.is_string()) {
        std::vector<float> values;
        if (ParseFloatList(rstd::cppstd::as_string_view(*raw.as_str()), values)) {
            result.ok    = true;
            result.value = values.size() == 1
                               ? ShaderValue(values.front())
                               : ShaderValue(std::span<const float>(values.data(), values.size()));
            return result;
        }
        result.skip_reason = "string value isn't parseable as float list";
        return result;
    }
    result.skip_reason = "unsupported JSON value shape";
    return result;
}

bool IsShaderGraphUserProperty(const Json& property) {
    auto type = property.get("type");
    if (type.is_none()) return false;
    auto string = (*type)->as_str();
    return string.is_some() && rstd::cppstd::as_string_view(*string) == "combo";
}

void ApplyClear(Scene& scene, const std::string& key, const Json& property) {
    if (scene.clearColorUserKey.empty() ||
        CanonicalSceneUserPropertyKey(scene.clearColorUserKey) != key)
        return;
    auto coerced = CoerceUserPropertyValue(property);
    if (! coerced.ok || coerced.value.size() < usize(3)) return;
    auto clamp01 = [](float value) {
        return std::clamp(value, 0.0f, 1.0f);
    };
    scene.clearColor = { clamp01(coerced.value[usize()]),
                         clamp01(coerced.value[usize(1)]),
                         clamp01(coerced.value[usize(2)]) };
}

void ApplyShaderUniforms(Scene& scene, const std::string& key, const Json& property) {
    auto it = scene.shader_user_var_index.find(key);
    if (it == scene.shader_user_var_index.end()) return;
    if (IsShaderGraphUserProperty(property)) {
        rstd_warn("user property '{}' skipped: shader graph mutation is not a uniform update", key);
        return;
    }

    auto coerced = CoerceUserPropertyValue(property);
    if (! coerced.ok) {
        rstd_warn("user property '{}' skipped: {}",
                  key,
                  coerced.skip_reason ? coerced.skip_reason : "unknown");
        return;
    }
    for (auto& [material, uniform_name] : it->second) {
        if (material) scene.SetMaterialShaderValue(*material, uniform_name, coerced.value);
    }
}

Option<std::string> ResolveTextureProperty(const Json& property) {
    if (property.is_string()) return Some(rstd::cppstd::to_string(*property.as_str()));
    if (! property.is_object()) return None();

    std::string type;
    if (auto member = property.get("type"); member.is_some()) {
        auto string = (*member)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }
    if (! type.empty() && type != "scenetexture" && type != "texture" && type != "replacetexture")
        return None();
    auto value = property.get("value");
    if (value.is_none()) return None();
    auto string = (*value)->as_str();
    return string.is_some() ? Some(rstd::cppstd::to_string(*string)) : None();
}

bool SameMaterialId(SceneMaterialId lhs, SceneMaterialId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

void PushUniqueMaterial(std::vector<SceneMaterialId>& materials, SceneMaterialId id) {
    auto it = std::find_if(materials.begin(), materials.end(), [id](auto existing) {
        return SameMaterialId(existing, id);
    });
    if (it == materials.end()) materials.push_back(id);
}

std::vector<SceneMaterialId> ApplyTextureProperty(Scene& scene, const std::string& key,
                                                  const Json& property) {
    std::vector<SceneMaterialId> changed;
    auto                         it = scene.material_texture_user_index.find(key);
    if (it == scene.material_texture_user_index.end()) return changed;

    auto texture = ResolveTextureProperty(property);
    if (texture.is_none()) return changed;
    for (const auto& binding : it->second) {
        if (! binding.material) continue;
        std::string next     = texture->empty() ? binding.fallback : *texture;
        auto        mutation = scene.SetMaterialTextureSlot(*binding.material, binding.slot, next);
        if (mutation.changed && mutation.material.is_some()) {
            PushUniqueMaterial(changed, *mutation.material);
        }
    }
    return changed;
}

Option<std::string> ResolveShaderComboValue(const Json&                          property,
                                            const Scene::ShaderComboUserBinding& binding) {
    auto        member = property.get("value");
    const auto& value  = member.is_some() ? **member : property;
    if (value.is_null()) return Some(std::string(binding.fallback));
    if (value.is_boolean()) return Some(std::string(*value.as_bool() ? "1" : "0"));
    if (value.is_number()) {
        auto number = value.as_f64();
        if (number.is_some()) {
            const double native = number->to_primitive();
            if (native >= std::numeric_limits<int>::min() &&
                native <= std::numeric_limits<int>::max())
                return Some(std::to_string(static_cast<int>(native)));
        }
        return None();
    }
    if (! value.is_string()) return None();

    auto text = rstd::cppstd::to_string(*value.as_str());
    if (text.empty()) return Some(std::string(binding.fallback));
    if (auto it = binding.options.find(text); it != binding.options.end())
        return Some(std::string(it->second));
    if (text == "true") return Some(std::string("1"));
    if (text == "false") return Some(std::string("0"));
    try {
        std::size_t parsed = 0;
        int         number = std::stoi(text, &parsed);
        if (parsed == text.size()) return Some(std::to_string(number));
    } catch (...) {
    }
    return None();
}

void RecordShaderComboDiagnostic(Scene& scene, std::string key,
                                 SceneUserPropertyDiagnosticCode code, std::string material,
                                 std::string combo, std::string message) {
    scene.AddUserPropertyDiagnostic(SceneUserPropertyDiagnostic { .key      = rstd::move(key),
                                                                  .code     = code,
                                                                  .material = rstd::move(material),
                                                                  .combo    = rstd::move(combo),
                                                                  .message = rstd::move(message) });
}

bool ApplyShaderCombos(Scene& scene, const std::string& key, const Json& property) {
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

    bool graph_changed = false;
    for (const auto& binding : it->second) {
        if (! binding.material) continue;
        auto next = ResolveShaderComboValue(property, binding);
        if (next.is_none()) {
            rstd_warn(
                "user property '{}' skipped: combo '{}' value is unsupported", key, binding.combo);
            RecordShaderComboDiagnostic(
                scene,
                key,
                SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue,
                binding.material->name,
                binding.combo,
                "shader combo value is unsupported");
            continue;
        }
        auto& material = *binding.material;
        if (! material.customShader.variant) {
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
            current != current_variant.resolved_combos.end() && current->second == *next)
            continue;

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
                                                           .shader  = rstd::move(compiled.shader),
                                                           .variant = rstd::move(compiled.variant),
                                                       });
        graph_changed |= mutation.changed && (material.DirtyFlags() & SceneMaterialDirtyGraph) != 0;
    }
    return graph_changed;
}

float CurrentImageAlpha(SceneNode* node) {
    if (! node) return 1.0f;
    return node->IsAlphaOverridden() ? node->EffectiveAlpha() : node->BaseAlpha();
}

Eigen::Vector3f CurrentImageColor(SceneNode* node) {
    if (! node) return { 1.0f, 1.0f, 1.0f };
    return node->IsColorOverridden() ? node->Color() : node->BaseColor();
}

bool MaterialHasUniform(const SceneMaterial& material, std::string_view uniform_name) {
    const std::string name(uniform_name);
    if (material.customShader.constValues.contains(name)) return true;
    if (material.customShader.shader &&
        material.customShader.shader->default_uniforms.contains(name))
        return true;
    return material.customShader.variant &&
           material.customShader.variant->default_uniforms.contains(name);
}

void ApplyImageColor(Scene& scene, const std::string& key, const Json& property) {
    auto it = scene.image_color_user_index.find(key);
    if (it == scene.image_color_user_index.end()) return;
    auto coerced = CoerceUserPropertyValue(property);
    if (! coerced.ok || coerced.value.size() < usize(3)) return;

    Eigen::Vector3f color { coerced.value[usize()],
                            coerced.value[usize(1)],
                            coerced.value[usize(2)] };
    for (const auto& binding : it->second) {
        if (binding.node) binding.node->SetColor(color);
        std::array<float, 3> color3 { color.x(), color.y(), color.z() };
        for (auto* material : binding.materials) {
            if (! material) continue;
            const bool  has_user_alpha = MaterialHasUniform(*material, G_USERALPHA);
            const float alpha = has_user_alpha && binding.node ? binding.node->BaseAlpha()
                                                               : CurrentImageAlpha(binding.node);
            std::array<float, 4> color4 { color.x(), color.y(), color.z(), alpha };
            if (MaterialHasUniform(*material, G_COLOR4))
                scene.SetMaterialShaderValue(*material, G_COLOR4, color4);
            if (MaterialHasUniform(*material, G_COLOR))
                scene.SetMaterialShaderValue(*material, G_COLOR, color3);
        }
    }
}

void ApplyImageAlpha(Scene& scene, const std::string& key, const Json& property) {
    auto it = scene.image_alpha_user_index.find(key);
    if (it == scene.image_alpha_user_index.end()) return;
    auto coerced = CoerceUserPropertyValue(property);
    if (! coerced.ok || coerced.value.size() < usize(1)) return;

    const float alpha = std::clamp(coerced.value[usize()], 0.0f, 1.0f);
    for (const auto& binding : it->second) {
        if (binding.node) binding.node->SetUserAlpha(alpha);
        auto                 color = CurrentImageColor(binding.node);
        std::array<float, 4> color4 { color.x(), color.y(), color.z(), alpha };
        for (auto* material : binding.materials) {
            if (! material) continue;
            const bool has_user_alpha = MaterialHasUniform(*material, G_USERALPHA);
            if (has_user_alpha) scene.SetMaterialShaderValue(*material, G_USERALPHA, alpha);
            if (MaterialHasUniform(*material, G_ALPHA))
                scene.SetMaterialShaderValue(*material, G_ALPHA, alpha);
            if (! has_user_alpha && MaterialHasUniform(*material, G_COLOR4))
                scene.SetMaterialShaderValue(*material, G_COLOR4, color4);
        }
    }
}

void ApplyParticles(Scene& scene, const std::string& key, const Json& property) {
    auto it = scene.particle_user_var_index.find(key);
    if (it == scene.particle_user_var_index.end()) return;
    auto coerced = CoerceUserPropertyValue(property);
    if (! coerced.ok) return;

    auto write_scalar = [&](float& destination) {
        if (coerced.value.size() >= usize(1)) destination = coerced.value[usize()];
    };
    auto write_vec3 = [&](std::array<float, 3>& destination, float scale) {
        if (coerced.value.size() < usize(3)) return;
        destination = { coerced.value[usize()] * scale,
                        coerced.value[usize(1)] * scale,
                        coerced.value[usize(2)] * scale };
    };

    for (auto& binding : it->second) {
        if (! binding.state) continue;
        auto*       state = static_cast<wpscene::ParticleInstanceoverride*>(binding.state.get());
        const auto& field = binding.field;
        if (field == "alpha")
            write_scalar(state->alpha);
        else if (field == "size")
            write_scalar(state->size);
        else if (field == "lifetime")
            write_scalar(state->lifetime);
        else if (field == "rate")
            write_scalar(state->rate);
        else if (field == "speed")
            write_scalar(state->speed);
        else if (field == "count")
            write_scalar(state->count);
        else if (field == "brightness")
            write_scalar(state->brightness);
        else if (field == "color") {
            // Particle initialization normalizes this authored 0..255 color.
            write_vec3(state->color, 255.0f);
            state->overColor = true;
        } else if (field == "colorn") {
            write_vec3(state->colorn, 1.0f);
            state->overColorn = true;
        } else if (field.starts_with("controlpoint") && ! field.starts_with("controlpointangle")) {
            try {
                int index = std::stoi(field.substr(std::string_view("controlpoint").size()));
                if (index >= 0 && index < 8) write_vec3(state->controlpoint[index], 1.0f);
            } catch (...) {
            }
        } else if (field.starts_with("controlpointangle")) {
            try {
                int index = std::stoi(field.substr(std::string_view("controlpointangle").size()));
                if (index >= 0 && index < 8) write_vec3(state->controlpointangle[index], 1.0f);
            } catch (...) {
            }
        }
    }
}

void ApplySoundVolume(Scene& scene, const std::string& key, const Json& property) {
    auto it = scene.sound_volume_user_index.find(key);
    if (it == scene.sound_volume_user_index.end()) return;
    auto coerced = CoerceUserPropertyValue(property);
    if (! coerced.ok || coerced.value.size() < usize(1)) return;
    const float volume = std::clamp(coerced.value[usize()], 0.0f, 1.0f);
    for (auto& control : it->second) {
        if (control) control->SetVolume(volume);
    }
}

} // namespace

std::string CanonicalSceneUserPropertyKey(std::string_view key) {
    return key == kWaywallenSchemeColorKey ? std::string(kSchemeColorKey) : std::string(key);
}

SceneUserPropertyMutation SceneUserPropertyApplier::Apply(Scene& scene, std::string_view raw_key,
                                                          const Json& property) {
    SceneUserPropertyMutation mutation;
    std::string               key = CanonicalSceneUserPropertyKey(raw_key);
    mutation.diagnostics_changed  = scene.shader_combo_user_index.contains(key);

    script::SetSceneUserProperty(scene, key, property);
    ApplyClear(scene, key, property);
    ApplyShaderUniforms(scene, key, property);
    mutation.texture_materials = ApplyTextureProperty(scene, key, property);
    mutation.graph_changed     = ApplyShaderCombos(scene, key, property);
    ApplyImageColor(scene, key, property);
    ApplyImageAlpha(scene, key, property);
    scene.ApplyUserTextBindings(key, property);
    ApplyParticles(scene, key, property);
    ApplySoundVolume(scene, key, property);
    scene.ApplyUserPropertyBindings(key, property);
    scene.ApplyUserCameraPathVisibilityBindings(key, property);
    mutation.graph_changed =
        scene.ApplyUserNodeVisibilityBindings(key, property) || mutation.graph_changed;
    mutation.graph_changed =
        scene.ApplyUserImageEffectVisibilityBindings(key, property) || mutation.graph_changed;
    return mutation;
}

SceneUserPropertyMutation SceneUserPropertyApplier::ApplyAll(Scene&                 scene,
                                                             const rstd::json::Map& properties) {
    SceneUserPropertyMutation result;
    properties.iter().for_each([&](auto entry) {
        auto [key, property] = entry;
        auto mutation        = Apply(scene, rstd::cppstd::as_string_view(key->as_str()), *property);
        result.graph_changed |= mutation.graph_changed;
        result.diagnostics_changed |= mutation.diagnostics_changed;
        for (auto material : mutation.texture_materials) {
            PushUniqueMaterial(result.texture_materials, material);
        }
    });
    return result;
}

std::vector<SceneMaterialId> SceneUserPropertyApplier::ApplyTexture(Scene&           scene,
                                                                    std::string_view raw_key,
                                                                    const Json&      property) {
    return ApplyTextureProperty(scene, CanonicalSceneUserPropertyKey(raw_key), property);
}

std::vector<SceneUserPropertyDiagnostic>
CollectSceneUserPropertyDiagnostics(const Scene& scene, std::string_view raw_key) {
    std::vector<SceneUserPropertyDiagnostic> out;
    const std::string                        key = CanonicalSceneUserPropertyKey(raw_key);
    for (const auto& diagnostic : scene.UserPropertyDiagnostics()) {
        if (diagnostic.key == key) out.push_back(diagnostic);
    }
    return out;
}

} // namespace owe
