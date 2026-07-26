module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.json;

using namespace owe::wpscene;
using namespace rstd::literals;

namespace
{

auto LoadJsonFile(owe::fs::VFS& vfs, const std::string& path) -> std::optional<owe::Json> {
    auto parsed = owe::ReadJsonFile(vfs, path);
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err_unchecked();
        rstd_error("Can't load json {}: {}", path, error.message.as_str());
        return std::nullopt;
    }
    return rstd::move(parsed).unwrap_unchecked();
}

float NormalizeLayerAlpha(float alpha) {
    // Older WE scene JSON stores layer alpha as 0..100 percent.
    if (alpha > 1.0f) alpha /= 100.0f;
    return std::clamp(alpha, 0.0f, 1.0f);
}

constexpr std::string_view kFoliageSwayEffect = "effects/foliagesway/effect.json";
constexpr SceneVersion     kNormalizedFoliageSwayStrengthVersion = 9;

auto LoadImageAssetJson(ImageObject& object, owe::fs::VFS& vfs, SceneVersion version,
                        bool explicit_no_copy_background) -> std::optional<owe::Json> {
    auto json = LoadJsonFile(vfs, "/assets/" + object.image);
    if (! json) return std::nullopt;

    owe::GetJsonValue(*json, "fullscreen", object.fullscreen, false);
    owe::GetJsonValue(*json, "passthrough", object.config.passthrough, false);
    owe::GetJsonValue(*json, "nopadding", object.nopadding, false);
    owe::GetJsonValue(*json, "solidlayer", object.solid_layer, false);
    owe::GetJsonValue(*json, "puppet", object.puppet, false);

    std::string material_path;
    if (! owe::GetJsonValue(*json, "material", material_path, false)) {
        rstd_info("image object no material");
        return std::nullopt;
    }
    auto material_json = LoadJsonFile(vfs, "/assets/" + material_path);
    if (! material_json) return std::nullopt;
    object.material.FromJson(*material_json, version);
    if (object.composite_layer && explicit_no_copy_background)
        object.material.combos["CLEARALPHA"] = 1;
    return json;
}

void ScaleAnimCurve(AnimCurve& curve, float scale) {
    auto scale_axis = [scale](std::vector<AnimKeyframe>& keys) {
        for (auto& key : keys) {
            key.value *= scale;
            key.front.y *= scale;
            key.back.y *= scale;
        }
    };
    scale_axis(curve.c0);
    scale_axis(curve.c1);
    scale_axis(curve.c2);
}

void NormalizeLegacyFoliageSwayStrength(MaterialPass& pass) {
    constexpr float scale = 0.01f;
    auto            value = pass.constantshadervalues.find("strength");
    if (value != pass.constantshadervalues.end()) {
        for (float& component : value->second) component *= scale;
    }
    auto animation = pass.constantshadervalues_animations.find("strength");
    if (animation != pass.constantshadervalues_animations.end())
        ScaleAnimCurve(animation->second, scale);
}

} // namespace

bool EffectCommand::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "command", command);
    owe::GetJsonValue(json, "target", target);
    owe::GetJsonValue(json, "source", source);
    return true;
}

bool ObjectInstance::FromJson(const owe::Json& json) {
    present = true;
    owe::GetJsonValue(json, "id", id, false);
    if (auto values = json.get("combos"_str); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                std::int32_t value { 0 };
                if (owe::GetJsonValue(*entry_value, value))
                    combos.emplace(rstd::cppstd::to_string(entry_key->as_str()), value);
            });
    }
    if (auto values = json.get("textures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& value : **array) {
                auto texture = value.as_str();
                if (texture.is_some()) textures.push_back(rstd::cppstd::to_string(*texture));
            }
        }
    }
    if (auto values = json.get("usertextures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& value : **array) usertextures.push(value.clone());
        }
    }
    return true;
}

bool EffectFbo::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "format", format);
    owe::GetJsonValue(json, "scale", scale);
    owe::GetJsonValue(json, "fit", fit, false);
    owe::GetJsonValue(json, "unique", unique, false);
    if (scale == 0) {
        rstd_error("fbo scale can't be 0");
        scale = 1;
    }
    return true;
}

bool ImageEffect::FromJson(const owe::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ImageEffect::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion v) {
    std::string filePath;
    owe::GetJsonValue(json, "file", filePath);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;
    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "username", username, false);
    owe::GetJsonValue(json, "id", id, false);
    auto jEffect = LoadJsonFile(vfs, "/assets/" + filePath);
    if (! jEffect) return false;
    if (! FromFileJson(*jEffect, vfs)) return false;

    if (auto injected_passes = json.get("passes"_str); injected_passes.is_some()) {
        auto array = (*injected_passes)->as_array();
        if (array.is_none()) return true;
        if ((*array)->len().to_primitive() > passes.size()) {
            rstd_error("passes is not injective");
            return false;
        }
        int32_t i = 0;
        for (const auto& jP : **array) {
            MaterialPass pass;
            pass.FromJson(jP);
            if (filePath == kFoliageSwayEffect && v != kSceneVersionUnknown &&
                v < kNormalizedFoliageSwayStrengthVersion)
                NormalizeLegacyFoliageSwayStrength(pass);
            passes[i++].Update(pass);
        }
    }
    return true;
}

bool ImageEffect::FromFileJson(const owe::Json& json, fs::VFS& vfs) {
    owe::GetJsonValue(json, "version", version, false);
    owe::GetJsonValue(json, "name", name);
    if (auto values = json.get("fbos"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jF : **array) {
                EffectFbo fbo;
                fbo.FromJson(jF);
                fbos.push_back(std::move(fbo));
            }
        }
    }
    if (auto effect_passes = json.get("passes"_str); effect_passes.is_some()) {
        auto array = (*effect_passes)->as_array();
        if (array.is_none()) {
            rstd_error("passes in effect file is not an array");
            return false;
        }
        bool compose { false };
        for (const auto& jP : **array) {
            if (jP.get("material"_str).is_none()) {
                if (jP.get("command"_str).is_some()) {
                    EffectCommand cmd;
                    cmd.FromJson(jP);
                    cmd.afterpos = i32(static_cast<rstd::int32_t>(passes.size()));
                    commands.push_back(cmd);
                    continue;
                }
                rstd_error("no material in effect pass");
                return false;
            }
            std::string matPath;
            owe::GetJsonValue(jP, "material", matPath);
            auto jMat = LoadJsonFile(vfs, "/assets/" + matPath);
            if (! jMat) return false;
            Material material;
            material.FromJson(*jMat);
            materials.push_back(std::move(material));
            MaterialPass pass;
            pass.FromJson(jP);
            passes.push_back(std::move(pass));
            if (jP.get("compose"_str).is_some()) owe::GetJsonValue(jP, "compose", compose);
        }
        if (compose) {
            if (passes.size() != 2) {
                rstd_error("effect compose option error");
                return false;
            }
            EffectFbo fbo;
            {
                fbo.name  = "_rt_FullCompoBuffer1";
                fbo.scale = 1;
            }
            fbos.push_back(fbo);
            passes.at(0).bind.push_back({ "previous", 0 });
            passes.at(0).target = "_rt_FullCompoBuffer1";
            passes.at(1).bind.push_back({ "_rt_FullCompoBuffer1", 0 });
        }
    } else {
        rstd_error("no passes in effect file");
        return false;
    }
    return true;
}

bool ImageObject::FromJson(const owe::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

std::optional<ImageAssetInfo> owe::wpscene::LoadImageAssetInfo(fs::VFS&         vfs,
                                                               std::string_view image) {
    auto j_image = LoadJsonFile(vfs, "/assets/" + std::string(image));
    if (! j_image) return std::nullopt;

    ImageAssetInfo info;
    owe::GetJsonValue(*j_image, "solidlayer", info.solid_layer, false);
    int32_t w = 0, h = 0;
    if (j_image->get("width"_str).is_some() && j_image->get("height"_str).is_some()) {
        owe::GetJsonValue(*j_image, "width", w, false);
        owe::GetJsonValue(*j_image, "height", h, false);
        if (w > 0 && h > 0) {
            info.size = std::array { static_cast<float>(w), static_cast<float>(h) };
            return info;
        }
    }

    std::string mat_path;
    if (! owe::GetJsonValue(*j_image, "material", mat_path, false)) return info;
    auto j_mat = LoadJsonFile(vfs, "/assets/" + mat_path);
    if (! j_mat) return info;
    Material mat;
    if (mat.FromJson(*j_mat) && ! mat.textures.empty()) info.first_texture = mat.textures.front();
    return info;
}

bool ImageObject::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion v) {
    owe::GetJsonValue(json, "image", image);
    composite_layer = image == "models/util/composelayer.json";
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;
    owe::GetJsonValue(json, "alignment", alignment, false);
    bool copy_background_value { true };
    bool explicit_no_copy_background =
        owe::GetJsonValue(json, "copybackground", copy_background_value, false) &&
        ! copy_background_value;
    auto jImage = LoadImageAssetJson(*this, vfs, v, explicit_no_copy_background);
    if (! jImage) return false;
    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "id", id, false);
    owe::GetJsonValue(json, "colorBlendMode", colorBlendMode, false);
    if (! fullscreen) {
        owe::GetJsonValue(json, "origin", origin);
        owe::GetJsonValue(json, "angles", angles);
        owe::GetJsonValue(json, "scale", scale);
        if (! owe::GetJsonValue(json, "parallaxDepth", parallaxDepth, false) && composite_layer) {
            // WE gives composite containers the regular layer depth when the field is omitted.
            parallaxDepth = { 1.0f, 1.0f };
        }
        if (jImage->get("width"_str).is_some()) {
            int32_t w = 0, h = 0;
            owe::GetJsonValue(*jImage, "width", w);
            owe::GetJsonValue(*jImage, "height", h);
            size = { (float)w, (float)h };
        } else if (json.get("size"_str).is_some()) {
            owe::GetJsonValue(json, "size", size);
        } else {
            size = { origin.at(0) * 2, origin.at(1) * 2 };
        }
    }
    owe::GetJsonValue(json, "color", color, false);
    ReadUserValueBinding(json, "color", color_user);
    color_user_key = color_user.name;
    owe::GetJsonValue(json, "alpha", alpha, false);
    alpha = NormalizeLayerAlpha(alpha);
    ReadUserValueBinding(json, "alpha", alpha_user);
    alpha_user_key = alpha_user.name;
    owe::GetJsonValue(json, "brightness", brightness, false);

    if (auto values = json.get("effects"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jE : **array) {
                ImageEffect wpeff;
                wpeff.FromJson(jE, vfs, v);
                effects.push_back(std::move(wpeff));
            }
        }
    }
    ReadPuppetAnimationLayers(json, puppet_layers);
    if (auto config_json = json.get("config"_str); config_json.is_some()) {
        owe::GetJsonValue(**config_json, "passthrough", config.passthrough, false);
    }

    owe::GetJsonValue(json, "locktransforms", locktransforms, false);
    owe::GetJsonValue(json, "muteineditor", muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    owe::GetJsonValue(json, "parent", parent, false);
    owe::GetJsonValue(json, "attachment", attachment, false);
    owe::GetJsonValue(json, "perspective", perspective, false);
    owe::GetJsonValue(json, "reflected", reflected, false);
    owe::GetJsonValue(json, "copybackground", copybackground, false);
    owe::GetJsonValue(json, "solid", solid, false);
    owe::GetJsonValue(json, "opaquebackground", opaquebackground, false);
    owe::GetJsonValue(json, "clampuvs", clampuvs, false);
    owe::GetJsonValue(json, "castshadow", castshadow, false);
    owe::GetJsonValue(json, "disablepropagation", disablepropagation, false);
    owe::GetJsonValue(json, "depthtest", depthtest, false);
    owe::GetJsonValue(json, "backgroundcolor", backgroundcolor, false);
    owe::GetJsonValue(json, "backgroundbrightness", backgroundbrightness, false);
    owe::GetJsonValue(json, "dependencies", dependencies, false);
    if (auto instance_json = json.get("instance"_str);
        instance_json.is_some() && (*instance_json)->is_object()) {
        instance.FromJson(**instance_json);
    }
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}

bool ImageObject::FromAsset(rstd::ref<rstd::str> asset, rstd::array<float, 2> asset_size,
                            fs::VFS& vfs, SceneVersion version) {
    image           = rstd::cppstd::to_string(asset);
    name            = "__createLayer:" + image;
    size            = { asset_size[rstd::usize()], asset_size[rstd::usize(1)] };
    composite_layer = image == "models/util/composelayer.json";
    return LoadImageAssetJson(*this, vfs, version, false).has_value();
}

bool ShapeObject::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion v) {
    owe::GetJsonValue(json, "shape", shape);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;
    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "id", id, false);
    owe::GetJsonValue(json, "origin", origin);
    owe::GetJsonValue(json, "angles", angles);
    owe::GetJsonValue(json, "scale", scale);

    if (auto values = json.get("effects"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& value : **array) {
                ImageEffect effect;
                if (! effect.FromJson(value, vfs, v)) return false;
                effects.push_back(std::move(effect));
            }
        }
    }

    owe::GetJsonValue(json, "locktransforms", locktransforms, false);
    owe::GetJsonValue(json, "muteineditor", muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    owe::GetJsonValue(json, "reflected", reflected, false);
    owe::GetJsonValue(json, "castshadow", castshadow, false);
    owe::GetJsonValue(json, "disablepropagation", disablepropagation, false);
    owe::GetJsonValue(json, "parent", parent, false);
    owe::GetJsonValue(json, "attachment", attachment, false);
    owe::GetJsonValue(json, "dependencies", dependencies, false);
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
