module;

#include <rstd/macro.hpp>
#include "WPJson.hpp"

#include <nlohmann/json.hpp>
module wescene.parse;
import rstd.log;
import rstd.cppstd;
import cppstd;

using namespace wallpaper::wpscene;

bool WPEffectCommand::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "command", command);
    GET_JSON_NAME_VALUE(json, "target", target);
    GET_JSON_NAME_VALUE(json, "source", source);
    return true;
}

bool WPObjectInstance::FromJson(const nlohmann::json& json) {
    present = true;
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    if (json.contains("combos") && json.at("combos").is_object()) {
        for (const auto& jC : json.at("combos").items()) {
            std::int32_t v { 0 };
            try { v = jC.value().get<std::int32_t>(); }
            catch (...) { continue; }
            combos.emplace(jC.key(), v);
        }
    }
    if (json.contains("textures") && json.at("textures").is_array()) {
        for (const auto& jT : json.at("textures")) {
            if (jT.is_string()) textures.push_back(jT.get<std::string>());
        }
    }
    if (json.contains("usertextures") && json.at("usertextures").is_array()) {
        for (const auto& jU : json.at("usertextures")) {
            usertextures.push_back(jU);
        }
    }
    return true;
}

bool WPEffectFbo::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "name", name);
    GET_JSON_NAME_VALUE(json, "format", format);

    GET_JSON_NAME_VALUE(json, "scale", scale);
    if(scale == 0) { 
        rstd_error("fbo scale can't be 0");
        scale = 1;
    }
    return true;
}

// Define and initialize the static property
const std::unordered_set<std::string> WPImageEffect::BLACKLISTED_WORKSHOP_EFFECTS = 
{
    "2799421411" // Audio Responsive Oscilloscope   --  causes vulcan deadlock
};

bool WPImageEffect::IsEffectBlacklisted(const std::string& filePath) {
    
    std::filesystem::path path(filePath);
    // Check if the path has a parent path
    if (path.has_parent_path()) {
        path = path.parent_path();
        if(path.has_parent_path()) {
            std::string effectId = path.parent_path().filename().string();
            std::string parentPath = path.parent_path().string();
            return WPImageEffect::BLACKLISTED_WORKSHOP_EFFECTS.find(effectId) != WPImageEffect::BLACKLISTED_WORKSHOP_EFFECTS.end();
        }
    }
    return false;
}
    
bool WPImageEffect::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool WPImageEffect::FromJson(const nlohmann::json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    std::string filePath;
    GET_JSON_NAME_VALUE(json, "file", filePath);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "username", username);
    if(this->IsEffectBlacklisted(filePath)) {
        //hide blacklisted effects
        visible = false;
    }
	GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    nlohmann::json jEffect;
    if(!PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + filePath), jEffect))
        return false;
    if(!FromFileJson(jEffect, vfs))
        return false;

    if(json.contains("passes")) {
        const auto& jPasses = json.at("passes");
        if(jPasses.size() > passes.size()) {
            rstd_error("passes is not injective");
            return false;
        }
        int32_t i = 0;
        for(const auto& jP:jPasses) {
            WPMaterialPass pass;
            pass.FromJson(jP);
            passes[i++].Update(pass); 
        }
    }
    return true;
}

bool WPImageEffect::FromFileJson(const nlohmann::json& json, fs::VFS& vfs) {
	GET_JSON_NAME_VALUE_NOWARN(json, "version", version);
    GET_JSON_NAME_VALUE(json, "name", name);
    if(json.contains("fbos")) {
        for(auto& jF:json.at("fbos")) {
            WPEffectFbo fbo;
            fbo.FromJson(jF);
            fbos.push_back(std::move(fbo));
        }
    }
    if(json.contains("passes")) {
        const auto& jEPasses = json.at("passes");
        bool compose {false};
        for(const auto& jP:jEPasses) {
            if(!jP.contains("material")) {
                if(jP.contains("command")) {
                    WPEffectCommand cmd;
                    cmd.FromJson(jP);
                    cmd.afterpos = passes.size();
                    commands.push_back(cmd);
                    continue;
                }
                rstd_error("no material in effect pass");
                return false;
            }
            std::string matPath;
            GET_JSON_NAME_VALUE(jP, "material", matPath);
            nlohmann::json jMat;
            if(!PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + matPath), jMat))
                return false;
            WPMaterial material;
            material.FromJson(jMat);
            materials.push_back(std::move(material));
            WPMaterialPass pass;
            pass.FromJson(jP);
            passes.push_back(std::move(pass));
            if(jP.contains("compose"))
	            GET_JSON_NAME_VALUE(jP, "compose", compose);
        }
        if(compose) {
            if(passes.size() != 2) {
                rstd_error("effect compose option error");
                return false;
            }
            WPEffectFbo fbo; {fbo.name = "_rt_FullCompoBuffer1"; fbo.scale = 1;}
            fbos.push_back(fbo);
            passes.at(0).bind.push_back({ "previous", 0});
            passes.at(0).target = "_rt_FullCompoBuffer1";
            passes.at(1).bind.push_back({"_rt_FullCompoBuffer1", 0});
        }
    } else {
        rstd_error("no passes in effect file");
        return false;
    }
    return true;
}

bool WPImageObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool WPImageObject::FromJson(const nlohmann::json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    GET_JSON_NAME_VALUE(json, "image", image);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    GET_JSON_NAME_VALUE_NOWARN(json, "alignment", alignment);
    nlohmann::json jImage;
    if(!PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + image), jImage)) {
        rstd_error("Can't load image json: {}", image);
        return false;
    }
    GET_JSON_NAME_VALUE_NOWARN(jImage, "fullscreen", fullscreen);
	GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
	GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
	GET_JSON_NAME_VALUE_NOWARN(json, "colorBlendMode", colorBlendMode);
	if(!fullscreen) {
		GET_JSON_NAME_VALUE(json, "origin", origin);	
		GET_JSON_NAME_VALUE(json, "angles", angles);	
		GET_JSON_NAME_VALUE(json, "scale", scale);	
		GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
		if(jImage.contains("width")) {
			int32_t w,h;
			GET_JSON_NAME_VALUE(jImage, "width", w);	
			GET_JSON_NAME_VALUE(jImage, "height", h);	
			size = {(float)w, (float)h};
		} else if(json.contains("size")) {
			GET_JSON_NAME_VALUE(json, "size", size);	
		} else {
			size = {origin.at(0)*2, origin.at(1)*2};
		}
    }
    GET_JSON_NAME_VALUE_NOWARN(jImage, "nopadding", nopadding);
    GET_JSON_NAME_VALUE_NOWARN(json, "color", color);
    GET_JSON_NAME_VALUE_NOWARN(json, "alpha", alpha);
    GET_JSON_NAME_VALUE_NOWARN(json, "brightness", brightness);

	GET_JSON_NAME_VALUE_NOWARN(jImage, "puppet", puppet);	
    if(jImage.contains("material")) {
        std::string matPath;
		GET_JSON_NAME_VALUE(jImage, "material", matPath);	
        nlohmann::json jMat;
        if(!PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) {
            rstd_error("Can't load material json: {}", matPath);
            return false;
        }
        material.FromJson(jMat);
    } else {
        rstd_info("image object no material");
        return false;
    }
    if(json.contains("effects")) {
        for(const auto& jE:json.at("effects")) {
            WPImageEffect wpeff;
            wpeff.FromJson(jE, vfs);
            effects.push_back(std::move(wpeff));
        }
    }
    if(json.contains("animationlayers")) {
        for(const auto& jLayer:json.at("animationlayers")) {
             WPPuppetLayer::AnimationLayer layer;
             GET_JSON_NAME_VALUE(jLayer, "animation", layer.id);
             GET_JSON_NAME_VALUE(jLayer, "blend", layer.blend);
             GET_JSON_NAME_VALUE(jLayer, "rate", layer.rate);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "visible", layer.visible);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "id", layer.layer_id);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "name", layer.name);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "additive", layer.additive);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "blendin", layer.blendin);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "blendout", layer.blendout);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "blendtime", layer.blendtime);
             puppet_layers.push_back(layer);
        }
    }
    if(json.contains("config")) {
        const auto& jConf = json.at("config");
        GET_JSON_NAME_VALUE_NOWARN(jConf, "passthrough", config.passthrough);
    }

    GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
    GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
    GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
    GET_JSON_NAME_VALUE_NOWARN(json, "perspective", perspective);
    GET_JSON_NAME_VALUE_NOWARN(json, "copybackground", copybackground);
    GET_JSON_NAME_VALUE_NOWARN(json, "solid", solid);
    GET_JSON_NAME_VALUE_NOWARN(json, "opaquebackground", opaquebackground);
    GET_JSON_NAME_VALUE_NOWARN(json, "clampuvs", clampuvs);
    GET_JSON_NAME_VALUE_NOWARN(json, "castshadow", castshadow);
    GET_JSON_NAME_VALUE_NOWARN(json, "disablepropagation", disablepropagation);
    GET_JSON_NAME_VALUE_NOWARN(json, "depthtest", depthtest);
    GET_JSON_NAME_VALUE_NOWARN(json, "backgroundcolor", backgroundcolor);
    GET_JSON_NAME_VALUE_NOWARN(json, "backgroundbrightness", backgroundbrightness);
    GET_JSON_NAME_VALUE_NOWARN(json, "dependencies", dependencies);
    if (json.contains("instance") && json.at("instance").is_object()) {
        instance.FromJson(json.at("instance"));
    }
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
