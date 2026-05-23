module;

#include <rstd/macro.hpp>


module wescene.parse;
import nlohmann.json;
import rstd.log;
import rstd.cppstd;

using namespace owe::wpscene;

bool WPMaterialPassBindItem::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "index", index);
    return true;
}

void WPMaterialPass::Update(const WPMaterialPass& p) {
    int32_t i = -1;
    for(const auto& el:p.textures) {
        i++;
        if(p.textures.size() > textures.size())
            textures.resize(p.textures.size());
        if(!el.empty()) {
            textures[i] = el;
        }
    }
    for(const auto& el:p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for(const auto& el:p.constantshadervalues_user) {
        constantshadervalues_user[el.first] = el.second;
    }
    for(const auto& el:p.combos) {
        combos[el.first] = el.second;
    }
}

void WPMaterial::MergePass(const WPMaterialPass& p) {
    int32_t i = -1;
    for(const auto& el:p.textures) {
        i++;
        if(p.textures.size() > textures.size())
            textures.resize(p.textures.size());
        if(!el.empty()) {
            textures[i] = el;
        }
    }
    for(const auto& el:p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for(const auto& el:p.constantshadervalues_user) {
        constantshadervalues_user[el.first] = el.second;
    }
    for(const auto& el:p.combos) {
        combos[el.first] = el.second;
    }
}

bool WPMaterialPass::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "id", id, false);
    if(json.contains("textures")) {
        for(const auto& jT:json.at("textures")) {
            std::string tex;
            if(!jT.is_null())
                owe::GetJsonValue(jT, tex);
            textures.push_back(tex);
        }
    }
    if(json.contains("usertextures") && json.at("usertextures").is_array()) {
        for(const auto& jU : json.at("usertextures")) {
            usertextures.push_back(jU);
        }
    }
    if(json.contains("constantshadervalues")) {
        for(const auto& jC:json.at("constantshadervalues").items()) {
            std::string name;
            std::vector<float> value;
            owe::GetJsonValue(jC.key(), name);
            owe::GetJsonValue(jC.value(), value);
            constantshadervalues[name] = value;
            // Capture scene.json instance-level user binding so the parser can
            // bridge effect-internal keys ("Opacity") to wallpaper-level
            // project.json keys ("luzopacidad").
            if (jC.value().is_object() && jC.value().contains("user") &&
                jC.value().at("user").is_string()) {
                constantshadervalues_user[name] = jC.value().at("user").get<std::string>();
            }
        }
    }
    if(json.contains("combos")) {
        for(const auto& jC:json.at("combos").items()) {
            std::string name;
            int32_t value;
            owe::GetJsonValue(jC.key(), name);
            owe::GetJsonValue(jC.value(), value);
            combos[name] = value;
        }
    }
    owe::GetJsonValue(json, "target", target, false);
    if(json.contains("bind")) {
        for(const auto& jB:json.at("bind")) {
            WPMaterialPassBindItem bindItem;
            bindItem.FromJson(jB);
            bind.push_back(bindItem);
        }
    }
    return true;
}

bool WPMaterial::FromJson(const nlohmann::json& json) {
    return FromJson(json, kSceneVersionUnknown);
}

bool WPMaterial::FromJson(const nlohmann::json& json, SceneVersion /*v*/) {
    if(!json.contains("passes") || json.at("passes").size() == 0) {
        rstd_error("material no data");
        return false;
    }
    const auto jContent = json.at("passes").at(0);
    if(!jContent.contains("shader")) {
        rstd_error("material no shader");
        return false;
    }
	owe::GetJsonValue(jContent, "blending", blending);
	owe::GetJsonValue(jContent, "cullmode", cullmode);
	owe::GetJsonValue(jContent, "depthtest", depthtest);
	owe::GetJsonValue(jContent, "depthwrite", depthwrite);
	owe::GetJsonValue(jContent, "shader", shader);
    if(jContent.contains("textures")) {
        for(const auto& jT:jContent.at("textures")) {
            std::string tex;
            if(!jT.is_null())
                owe::GetJsonValue(jT, tex);
            textures.push_back(tex);
        }
    }
    if(jContent.contains("constantshadervalues")) {
        for(const auto& jC:jContent.at("constantshadervalues").items()) {
            std::string name;
            std::vector<float> value;
            owe::GetJsonValue(jC.key(), name);
            owe::GetJsonValue(jC.value(), value);
            constantshadervalues[name] = value;
            if (jC.value().is_object() && jC.value().contains("user") &&
                jC.value().at("user").is_string()) {
                constantshadervalues_user[name] = jC.value().at("user").get<std::string>();
            }
        }
    }
    if(jContent.contains("combos")) {
        for(const auto& jC:jContent.at("combos").items()) {
            std::string name;
            int32_t value;
            owe::GetJsonValue(jC.key(), name);
            owe::GetJsonValue(jC.value(), value);
            combos[name] = value;
        }
    }
    return true;
}
