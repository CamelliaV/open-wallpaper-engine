module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import rstd.log;
import rstd.cppstd;
import wescene.json;

using namespace owe::wpscene;

namespace
{

void LoadUserShaderValues(const owe::Json&                              json,
                          std::unordered_map<std::string, std::string>& out) {
    auto values = json.get("usershadervalues");
    if (values.is_none()) return;
    auto object = (*values)->as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto text                     = entry_value->as_str();
        if (text.is_some())
            out[rstd::cppstd::to_string(entry_key->as_str())] = rstd::cppstd::to_string(*text);
    });
}

void MergeUserTextures(const rstd::json::Array& src, rstd::json::Array& dst) {
    while (src.len() > dst.len()) dst.push(owe::Json::Null());
    for (rstd::usize i = rstd::usize(); i < src.len(); ++i) {
        if (! src[i].is_null()) dst[i] = src[i].clone();
    }
}

void LoadConstantShaderValue(std::string name, const owe::Json& json,
                             std::unordered_map<std::string, std::vector<float>>& constant_values,
                             std::unordered_map<std::string, std::string>&        user_values,
                             std::unordered_map<std::string, AnimCurve>&          animations) {
    std::vector<float> value;
    owe::GetJsonValue(json, value);
    constant_values[name] = std::move(value);
    if (! json.is_object()) return;

    if (auto user = json.get("user"); user.is_some()) {
        auto string = (*user)->as_str();
        if (string.is_some()) user_values[name] = rstd::cppstd::to_string(*string);
    }
    if (auto animation = json.get("animation"); animation.is_some()) {
        AnimCurve curve;
        if (ParseAnimCurve(**animation, curve)) {
            animations[name] = std::move(curve);
        }
    }
}

} // namespace

auto owe::wpscene::Material::clone() const -> Material {
    Material clone;
    clone.blending                  = blending;
    clone.cullmode                  = cullmode;
    clone.shader                    = shader;
    clone.depthtest                 = depthtest;
    clone.depthwrite                = depthwrite;
    clone.textures                  = textures;
    clone.combos                    = combos;
    clone.constantshadervalues      = constantshadervalues;
    clone.constantshadervalues_user = constantshadervalues_user;
    clone.user_shader_values        = user_shader_values;
    clone.use_puppet                = use_puppet;
    MergeUserTextures(usertextures, clone.usertextures);
    for (const auto& [name, curve] : constantshadervalues_animations)
        clone.constantshadervalues_animations[name] = curve.clone();
    return clone;
}

bool MaterialPassBindItem::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "index", index);
    return true;
}

void MaterialPass::Update(const MaterialPass& p) {
    int32_t i = -1;
    for (const auto& el : p.textures) {
        i++;
        if (p.textures.size() > textures.size()) textures.resize(p.textures.size());
        if (! el.empty()) {
            textures[i] = el;
        }
    }
    for (const auto& el : p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervalues_user) {
        constantshadervalues_user[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervalues_animations) {
        constantshadervalues_animations[el.first] = el.second.clone();
    }
    for (const auto& el : p.user_shader_values) {
        user_shader_values[el.first] = el.second;
    }
    MergeUserTextures(p.usertextures, usertextures);
    for (const auto& el : p.combos) {
        combos[el.first] = el.second;
    }
}

void Material::MergePass(const MaterialPass& p) {
    int32_t i = -1;
    for (const auto& el : p.textures) {
        i++;
        if (p.textures.size() > textures.size()) textures.resize(p.textures.size());
        if (! el.empty()) {
            textures[i] = el;
        }
    }
    for (const auto& el : p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervalues_user) {
        constantshadervalues_user[el.first] = el.second;
    }
    for (const auto& el : p.constantshadervalues_animations) {
        constantshadervalues_animations[el.first] = el.second.clone();
    }
    for (const auto& el : p.user_shader_values) {
        user_shader_values[el.first] = el.second;
    }
    MergeUserTextures(p.usertextures, usertextures);
    for (const auto& el : p.combos) {
        combos[el.first] = el.second;
    }
}

bool MaterialPass::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "id", id, false);
    if (auto values = json.get("textures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jT : **array) {
                std::string tex;
                if (! jT.is_null()) owe::GetJsonValue(jT, tex);
                textures.push_back(std::move(tex));
            }
        }
    }
    if (auto values = json.get("usertextures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& jU : **array) usertextures.push(jU.clone());
    }
    if (auto values = json.get("constantshadervalues"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                LoadConstantShaderValue(rstd::cppstd::to_string(entry_key->as_str()),
                                        *entry_value,
                                        constantshadervalues,
                                        constantshadervalues_user,
                                        constantshadervalues_animations);
            });
    }
    LoadUserShaderValues(json, user_shader_values);
    if (auto values = json.get("combos"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                std::int32_t value { 0 };
                owe::GetJsonValue(*entry_value, value);
                combos[rstd::cppstd::to_string(entry_key->as_str())] = value;
            });
    }
    owe::GetJsonValue(json, "target", target, false);
    if (auto values = json.get("bind"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jB : **array) {
                MaterialPassBindItem bindItem;
                bindItem.FromJson(jB);
                bind.push_back(bindItem);
            }
        }
    }
    return true;
}

bool Material::FromJson(const owe::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool Material::FromJson(const owe::Json& json, SceneVersion /*v*/) {
    auto passes = json.get("passes");
    if (passes.is_none()) {
        rstd_error("material no data");
        return false;
    }
    auto pass_array = (*passes)->as_array();
    if (pass_array.is_none() || (*pass_array)->is_empty()) {
        rstd_error("material no data");
        return false;
    }
    const auto& jContent = (**pass_array)[rstd::usize()];
    if (jContent.get("shader").is_none()) {
        rstd_error("material no shader");
        return false;
    }
    owe::GetJsonValue(jContent, "blending", blending);
    owe::GetJsonValue(jContent, "cullmode", cullmode);
    owe::GetJsonValue(jContent, "depthtest", depthtest);
    owe::GetJsonValue(jContent, "depthwrite", depthwrite);
    owe::GetJsonValue(jContent, "shader", shader);
    if (auto values = jContent.get("textures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jT : **array) {
                std::string tex;
                if (! jT.is_null()) owe::GetJsonValue(jT, tex);
                textures.push_back(std::move(tex));
            }
        }
    }
    if (auto values = jContent.get("usertextures"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& jU : **array) usertextures.push(jU.clone());
    }
    if (auto values = jContent.get("constantshadervalues"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                LoadConstantShaderValue(rstd::cppstd::to_string(entry_key->as_str()),
                                        *entry_value,
                                        constantshadervalues,
                                        constantshadervalues_user,
                                        constantshadervalues_animations);
            });
    }
    LoadUserShaderValues(jContent, user_shader_values);
    if (auto values = jContent.get("combos"); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                std::int32_t value { 0 };
                owe::GetJsonValue(*entry_value, value);
                combos[rstd::cppstd::to_string(entry_key->as_str())] = value;
            });
    }
    return true;
}
