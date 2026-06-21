module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import nlohmann.json;
import rstd.log;
import rstd.cppstd;

using namespace owe::wpscene;

bool EffectCommand::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "command", command);
    owe::GetJsonValue(json, "target", target);
    owe::GetJsonValue(json, "source", source);
    return true;
}

bool ObjectInstance::FromJson(const nlohmann::json& json) {
    present = true;
    owe::GetJsonValue(json, "id", id, false);
    if (json.contains("combos") && json.at("combos").is_object()) {
        for (const auto& jC : json.at("combos").items()) {
            std::int32_t v { 0 };
            try {
                v = jC.value().get<std::int32_t>();
            } catch (...) {
                continue;
            }
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

bool EffectFbo::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "format", format);

    owe::GetJsonValue(json, "scale", scale);
    owe::GetJsonValue(json, "fit", fit, false);
    if (scale == 0) {
        rstd_error("fbo scale can't be 0");
        scale = 1;
    }
    return true;
}

// Define and initialize the static property
const std::unordered_set<std::string> ImageEffect::BLACKLISTED_WORKSHOP_EFFECTS = {
    "2799421411" // Audio Responsive Oscilloscope   --  causes vulcan deadlock
};

bool ImageEffect::IsEffectBlacklisted(const std::string& filePath) {
    std::filesystem::path path(filePath);
    // Check if the path has a parent path
    if (path.has_parent_path()) {
        path = path.parent_path();
        if (path.has_parent_path()) {
            std::string effectId   = path.parent_path().filename().string();
            std::string parentPath = path.parent_path().string();
            return ImageEffect::BLACKLISTED_WORKSHOP_EFFECTS.find(effectId) !=
                   ImageEffect::BLACKLISTED_WORKSHOP_EFFECTS.end();
        }
    }
    return false;
}

bool ImageEffect::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ImageEffect::FromJson(const nlohmann::json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    std::string filePath;
    owe::GetJsonValue(json, "file", filePath);
    owe::GetJsonValue(json, "visible", visible, false);
    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "username", username, false);
    if (this->IsEffectBlacklisted(filePath)) {
        // hide blacklisted effects
        visible = false;
    }
    owe::GetJsonValue(json, "id", id, false);
    nlohmann::json jEffect;
    if (! owe::ParseJson(fs::GetFileContent(vfs, "/assets/" + filePath), jEffect)) return false;
    if (! FromFileJson(jEffect, vfs)) return false;

    if (json.contains("passes")) {
        const auto& jPasses = json.at("passes");
        if (jPasses.size() > passes.size()) {
            rstd_error("passes is not injective");
            return false;
        }
        int32_t i = 0;
        for (const auto& jP : jPasses) {
            MaterialPass pass;
            pass.FromJson(jP);
            passes[i++].Update(pass);
        }
    }
    return true;
}

bool ImageEffect::FromFileJson(const nlohmann::json& json, fs::VFS& vfs) {
    owe::GetJsonValue(json, "version", version, false);
    owe::GetJsonValue(json, "name", name);
    if (json.contains("fbos")) {
        for (auto& jF : json.at("fbos")) {
            EffectFbo fbo;
            fbo.FromJson(jF);
            fbos.push_back(std::move(fbo));
        }
    }
    if (json.contains("passes")) {
        const auto& jEPasses = json.at("passes");
        bool        compose { false };
        for (const auto& jP : jEPasses) {
            if (! jP.contains("material")) {
                if (jP.contains("command")) {
                    EffectCommand cmd;
                    cmd.FromJson(jP);
                    cmd.afterpos = passes.size();
                    commands.push_back(cmd);
                    continue;
                }
                rstd_error("no material in effect pass");
                return false;
            }
            std::string matPath;
            owe::GetJsonValue(jP, "material", matPath);
            nlohmann::json jMat;
            if (! owe::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) return false;
            Material material;
            material.FromJson(jMat);
            materials.push_back(std::move(material));
            MaterialPass pass;
            pass.FromJson(jP);
            passes.push_back(std::move(pass));
            if (jP.contains("compose")) owe::GetJsonValue(jP, "compose", compose);
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

bool ImageObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ImageObject::FromJson(const nlohmann::json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    owe::GetJsonValue(json, "image", image);
    owe::GetJsonValue(json, "visible", visible, false);
    ReadVisibleUserBinding(json, visible_user);
    visible_user_key = visible_user.name;
    owe::GetJsonValue(json, "alignment", alignment, false);
    nlohmann::json jImage;
    if (! owe::ParseJson(fs::GetFileContent(vfs, "/assets/" + image), jImage)) {
        rstd_error("Can't load image json: {}", image);
        return false;
    }
    owe::GetJsonValue(jImage, "fullscreen", fullscreen, false);
    owe::GetJsonValue(jImage, "passthrough", config.passthrough, false);
    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "id", id, false);
    owe::GetJsonValue(json, "colorBlendMode", colorBlendMode, false);
    if (! fullscreen) {
        owe::GetJsonValue(json, "origin", origin);
        owe::GetJsonValue(json, "angles", angles);
        owe::GetJsonValue(json, "scale", scale);
        owe::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);
        if (jImage.contains("width")) {
            int32_t w, h;
            owe::GetJsonValue(jImage, "width", w);
            owe::GetJsonValue(jImage, "height", h);
            size = { (float)w, (float)h };
        } else if (json.contains("size")) {
            owe::GetJsonValue(json, "size", size);
        } else {
            size = { origin.at(0) * 2, origin.at(1) * 2 };
        }
    }
    owe::GetJsonValue(jImage, "nopadding", nopadding, false);
    owe::GetJsonValue(json, "color", color, false);
    ReadUserValueBinding(json, "color", color_user);
    color_user_key = color_user.name;
    owe::GetJsonValue(json, "alpha", alpha, false);
    owe::GetJsonValue(json, "brightness", brightness, false);

    owe::GetJsonValue(jImage, "puppet", puppet, false);
    const bool explicit_no_copy_background = json.contains("copybackground") &&
                                             json.at("copybackground").is_boolean() &&
                                             ! json.at("copybackground").get<bool>();

    if (jImage.contains("material")) {
        std::string matPath;
        owe::GetJsonValue(jImage, "material", matPath);
        nlohmann::json jMat;
        if (! owe::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) {
            rstd_error("Can't load material json: {}", matPath);
            return false;
        }
        material.FromJson(jMat);
        if (image == "models/util/composelayer.json" && explicit_no_copy_background) {
            material.combos["CLEARALPHA"] = 1;
        }
    } else {
        rstd_info("image object no material");
        return false;
    }
    if (json.contains("effects")) {
        for (const auto& jE : json.at("effects")) {
            ImageEffect wpeff;
            wpeff.FromJson(jE, vfs);
            effects.push_back(std::move(wpeff));
        }
    }
    ReadPuppetAnimationLayers(json, puppet_layers);
    if (json.contains("config")) {
        const auto& jConf = json.at("config");
        owe::GetJsonValue(jConf, "passthrough", config.passthrough, false);
    }

    owe::GetJsonValue(json, "locktransforms", locktransforms, false);
    owe::GetJsonValue(json, "muteineditor", muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    owe::GetJsonValue(json, "parent", parent, false);
    owe::GetJsonValue(json, "attachment", attachment, false);
    owe::GetJsonValue(json, "perspective", perspective, false);
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
    if (json.contains("instance") && json.at("instance").is_object()) {
        instance.FromJson(json.at("instance"));
    }
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
