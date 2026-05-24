module;

#include <rstd/macro.hpp>


module wescene.parse;
import nlohmann.json;
import wescene.core;
import rstd.log;
import rstd.cppstd;

using namespace owe::wpscene;

bool ParticleChild::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "type", type);

    if (name.empty()) {
        return false;
    }

    nlohmann::json jParticle;
    if (! owe::ParseJson(fs::GetFileContent(vfs, "/assets/" + name), jParticle)) return false;

    if (! obj.FromJson(jParticle, vfs)) return false;

    owe::GetJsonValue(json, "maxcount", maxcount, false);
    owe::GetJsonValue(json, "controlpointstartindex", controlpointstartindex, false);
    owe::GetJsonValue(json, "probability", probability, false);

    owe::GetJsonValue(json, "origin", origin, false);
    owe::GetJsonValue(json, "scale", scale, false);
    owe::GetJsonValue(json, "angles", angles, false);
    return true;
}

bool ParticleControlpoint::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "id", id);

    uint32_t _raw_flags { 0 };
    owe::GetJsonValue(json, "flags", _raw_flags, false);
    flags = EFlags(_raw_flags);

    owe::GetJsonValue(json, "offset", offset, false);
    return true;
};

bool ParticleRender::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "name", name);

    if (sstart_with(name, "rope")) {
        owe::GetJsonValue(json, "subdivision", subdivision, false);
    }
    if (name == "spritetrail" || name == "ropetrail") {
        owe::GetJsonValue(json, "length", length, false);
        owe::GetJsonValue(json, "maxlength", maxlength, false);
        owe::GetJsonValue(json, "segments", segments, false);
    }
    return true;
}

bool Emitter::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "id", id);

    owe::GetJsonValue(json, "speedmin", speedmin, false);
    owe::GetJsonValue(json, "speedmax", speedmax, false);
    owe::GetJsonValue(json, "instantaneous", instantaneous, false);
    owe::GetJsonValue(json, "distancemax", distancemax, false);
    owe::GetJsonValue(json, "distancemin", distancemin, false);
    owe::GetJsonValue(json, "rate", rate, false);
    owe::GetJsonValue(json, "directions", directions, false);
    owe::GetJsonValue(json, "origin", origin, false);
    owe::GetJsonValue(json, "sign", sign, false);
    owe::GetJsonValue(json, "audioprocessingmode", audioprocessingmode, false);
    owe::GetJsonValue(json, "audioamount", audioamount, false);
    owe::GetJsonValue(json, "audioexponent", audioexponent, false);
    owe::GetJsonValue(json, "audiofrequency", audiofrequency, false);
    owe::GetJsonValue(json, "audiobounds", audiobounds, false);
    owe::GetJsonValue(json, "controlpoint", controlpoint, false);
    owe::GetJsonValue(json, "duration", duration, false);

    if (controlpoint >= 8) rstd_error("wrong controlpoint {}", controlpoint);
    controlpoint = controlpoint % 8; // limited to 0-7

    uint32_t _raw_flags { 0 };
    owe::GetJsonValue(json, "flags", _raw_flags, false);
    flags = EFlags(_raw_flags);

    std::transform(sign.begin(), sign.end(), sign.begin(), [](int32_t v) {
        if (v != 0)
            return v / std::abs(v);
        else
            return 0;
    });
    return true;
}

bool ParticleInstanceoverride::FromJosn(const nlohmann::json& json) {
    enabled = true;

    // {"user":"<key>","value":...} indirection -> record the key for the
    // live user-property pipeline. The value still parses normally via
    // GetJsonValue (which already looks through the `value` wrapper).
    auto bind = [&](const char* field) {
        if (! json.contains(field)) return;
        const auto& sub = json.at(field);
        if (! sub.is_object()) return;
        auto it = sub.find("user");
        if (it != sub.end() && it->is_string()) {
            bindings[field] = it->get<std::string>();
        }
    };

    owe::GetJsonValue(json, "alpha", alpha, false);          bind("alpha");
    owe::GetJsonValue(json, "size", size, false);            bind("size");
    owe::GetJsonValue(json, "lifetime", lifetime, false);    bind("lifetime");
    owe::GetJsonValue(json, "rate", rate, false);            bind("rate");
    owe::GetJsonValue(json, "speed", speed, false);          bind("speed");
    owe::GetJsonValue(json, "count", count, false);          bind("count");
    owe::GetJsonValue(json, "brightness", brightness, false); bind("brightness");
    owe::GetJsonValue(json, "id", id, false);
    if (json.contains("color")) {
        owe::GetJsonValue(json, "color", color);
        overColor = true;
        bind("color");
    } else if (json.contains("colorn")) {
        owe::GetJsonValue(json, "colorn", colorn);
        overColorn = true;
        bind("colorn");
    }
    {
        const char* cp_keys[]    = { "controlpoint0", "controlpoint1", "controlpoint2",
                                     "controlpoint3", "controlpoint4", "controlpoint5",
                                     "controlpoint6", "controlpoint7" };
        const char* cpa_keys[]   = { "controlpointangle0", "controlpointangle1",
                                     "controlpointangle2", "controlpointangle3",
                                     "controlpointangle4", "controlpointangle5",
                                     "controlpointangle6", "controlpointangle7" };
        for (int i = 0; i < 8; ++i) {
            owe::GetJsonValue(json, cp_keys[i], controlpoint[i], false);
            bind(cp_keys[i]);
            owe::GetJsonValue(json, cpa_keys[i], controlpointangle[i], false);
            bind(cpa_keys[i]);
        }
    }
    return true;
};

bool Particle::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    if (! json.contains("emitter")) {
        rstd_error("particle no emitter");
        return false;
    }
    for (const auto& el : json.at("emitter")) {
        Emitter emi;
        emi.FromJson(el);
        emitters.push_back(emi);
    }
    if (json.contains("renderer")) {
        for (const auto& el : json.at("renderer")) {
            ParticleRender pr;
            pr.FromJson(el);
            renderers.push_back(pr);
        }
    }
    // add sprite if no renderers
    if (renderers.empty()) {
        ParticleRender pr;
        pr.name = "sprite";
        renderers.push_back(pr);
    }
    if (json.contains("initializer")) {
        for (const auto& el : json.at("initializer")) {
            initializers.push_back(el);
        }
    }
    if (json.contains("operator")) {
        for (const auto& el : json.at("operator")) {
            operators.push_back(el);
        }
    }
    if (json.contains("controlpoint")) {
        for (const auto& el : json.at("controlpoint")) {
            ParticleControlpoint pc;
            pc.FromJson(el);
            controlpoints.push_back(pc);
        }
    }

    if (json.contains("children")) {
        for (const auto& el : json.at("children")) {
            ParticleChild child;
            if (child.FromJson(el, vfs)) {
                children.push_back(child);
            }
        }
    }
    if (json.contains("material")) {
        std::string matPath;
        owe::GetJsonValue(json, "material", matPath);
        nlohmann::json jMat;
        if (! owe::ParseJson(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) return false;
        material.FromJson(jMat);
    } else {
        rstd_error("particle object no material");
        return false;
    }

    owe::GetJsonValue(json, "animationmode", animationmode, false);
    owe::GetJsonValue(json, "sequencemultiplier", sequencemultiplier, false);
    owe::GetJsonValue(json, "maxcount", maxcount);
    owe::GetJsonValue(json, "starttime", starttime);

    uint32_t rawflags { 0 };
    owe::GetJsonValue(json, "flags", rawflags, false);
    flags = EFlags(rawflags);

    return true;
}

bool WPParticleObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool WPParticleObject::FromJson(const nlohmann::json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    owe::GetJsonValue(json, "particle", particle);
    owe::GetJsonValue(json, "visible", visible, false);
    if (json.contains("visible") && json.at("visible").is_object()) {
        const auto& jv = json.at("visible");
        if (jv.contains("user") && jv.at("user").is_string())
            visible_user_key = jv.at("user").get<std::string>();
    }

    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "id", id, false);
    owe::GetJsonValue(json, "origin", origin);
    owe::GetJsonValue(json, "angles", angles);
    owe::GetJsonValue(json, "scale", scale);
    owe::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);

    if (json.contains("instanceoverride") && ! json.at("instanceoverride").is_null()) {
        instanceoverride.FromJosn(json.at("instanceoverride"));
    }

    owe::GetJsonValue(json, "locktransforms", locktransforms, false);
    owe::GetJsonValue(json, "muteineditor", muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    owe::GetJsonValue(json, "parent", parent, false);
    owe::GetJsonValue(json, "dependencies", dependencies, false);
    owe::GetJsonValue(json, "controlpoint", controlpoint, false);
    if (json.contains("instance")) instance = json.at("instance");
    if (json.contains("particlesrc")) particlesrc = json.at("particlesrc");

    AbsorbAllFieldBindings(json, field_bindings);

    nlohmann::json jParticle;
    if (! owe::ParseJson(fs::GetFileContent(vfs, "/assets/" + particle), jParticle)) return false;
    if (! particleObj.FromJson(jParticle, vfs)) return false;
    return true;
}
