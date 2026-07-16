module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import wescene.core;
import rstd.log;
import rstd.cppstd;

using namespace owe::wpscene;

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

} // namespace

bool ParticleChild::FromJson(const owe::Json& json, fs::VFS& vfs) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "type", type);

    if (name.empty()) {
        return false;
    }

    auto jParticle = LoadJsonFile(vfs, "/assets/" + name);
    if (! jParticle) return false;

    if (! obj.FromJson(*jParticle, vfs)) return false;

    owe::GetJsonValue(json, "maxcount", maxcount, false);
    owe::GetJsonValue(json, "controlpointstartindex", controlpointstartindex, false);
    owe::GetJsonValue(json, "probability", probability, false);
    owe::GetJsonValue(json, "origin", origin, false);
    owe::GetJsonValue(json, "scale", scale, false);
    owe::GetJsonValue(json, "angles", angles, false);
    return true;
}

bool ParticleControlpoint::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "id", id);

    uint32_t _raw_flags { 0 };
    owe::GetJsonValue(json, "flags", _raw_flags, false);
    flags = EFlags(_raw_flags);

    owe::GetJsonValue(json, "offset", offset, false);
    return true;
};

bool ParticleRender::FromJson(const owe::Json& json) {
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

bool Emitter::FromJson(const owe::Json& json) {
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

bool ParticleInstanceoverride::FromJosn(const owe::Json& json) {
    enabled = true;

    // {"user":"<key>","value":...} indirection -> record the key for the
    // live user-property pipeline. The value still parses normally via
    // GetJsonValue (which already looks through the `value` wrapper).
    auto bind = [&](const char* field) {
        auto sub = json.get(field);
        if (sub.is_none() || ! (*sub)->is_object()) return;
        auto user = (*sub)->get("user");
        if (user.is_none()) return;
        auto string = (*user)->as_str();
        if (string.is_some()) bindings[field] = rstd::cppstd::to_string(*string);
    };

    owe::GetJsonValue(json, "alpha", alpha, false);
    bind("alpha");
    owe::GetJsonValue(json, "size", size, false);
    bind("size");
    owe::GetJsonValue(json, "lifetime", lifetime, false);
    bind("lifetime");
    owe::GetJsonValue(json, "rate", rate, false);
    bind("rate");
    owe::GetJsonValue(json, "speed", speed, false);
    bind("speed");
    owe::GetJsonValue(json, "count", count, false);
    bind("count");
    owe::GetJsonValue(json, "brightness", brightness, false);
    bind("brightness");
    owe::GetJsonValue(json, "id", id, false);
    if (auto value = json.get("color"); value.is_some()) {
        owe::GetJsonValue(json, "color", color);
        overColor = true;
        bind("color");
    } else if (auto value = json.get("colorn"); value.is_some()) {
        owe::GetJsonValue(json, "colorn", colorn);
        overColorn = true;
        bind("colorn");
    }
    {
        const char* cp_keys[]  = { "controlpoint0", "controlpoint1", "controlpoint2",
                                   "controlpoint3", "controlpoint4", "controlpoint5",
                                   "controlpoint6", "controlpoint7" };
        const char* cpa_keys[] = { "controlpointangle0", "controlpointangle1", "controlpointangle2",
                                   "controlpointangle3", "controlpointangle4", "controlpointangle5",
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

bool Particle::FromJson(const owe::Json& json, fs::VFS& vfs) {
    auto emitter_values = json.get("emitter");
    if (emitter_values.is_none()) {
        rstd_error("particle no emitter");
        return false;
    }
    auto emitter_array = (*emitter_values)->as_array();
    if (emitter_array.is_none()) {
        rstd_error("particle emitter is not an array");
        return false;
    }
    for (const auto& el : **emitter_array) {
        Emitter emi;
        emi.FromJson(el);
        emitters.push_back(std::move(emi));
    }
    if (auto values = json.get("renderer"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleRender pr;
                pr.FromJson(el);
                renderers.push_back(std::move(pr));
            }
        }
    }
    // add sprite if no renderers
    if (renderers.empty()) {
        ParticleRender pr;
        pr.name = "sprite";
        renderers.push_back(pr);
    }
    if (auto values = json.get("initializer"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& el : **array) initializers.push(el.clone());
    }
    if (auto values = json.get("operator"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& el : **array) operators.push(el.clone());
    }
    if (auto values = json.get("controlpoint"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleControlpoint pc;
                pc.FromJson(el);
                controlpoints.push_back(std::move(pc));
            }
        }
    }

    if (auto values = json.get("children"); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleChild child;
                if (child.FromJson(el, vfs)) children.push_back(std::move(child));
            }
        }
    }
    if (json.get("material").is_some()) {
        std::string matPath;
        owe::GetJsonValue(json, "material", matPath);
        auto jMat = LoadJsonFile(vfs, "/assets/" + matPath);
        if (! jMat) return false;
        material.FromJson(*jMat);
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

bool ParticleObject::FromJson(const owe::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ParticleObject::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    owe::GetJsonValue(json, "particle", particle);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;

    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "id", id, false);
    owe::GetJsonValue(json, "origin", origin);
    owe::GetJsonValue(json, "angles", angles);
    owe::GetJsonValue(json, "scale", scale);
    owe::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);

    if (auto value = json.get("instanceoverride"); value.is_some() && ! (*value)->is_null()) {
        instanceoverride.FromJosn(**value);
    }

    owe::GetJsonValue(json, "locktransforms", locktransforms, false);
    owe::GetJsonValue(json, "muteineditor", muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    owe::GetJsonValue(json, "parent", parent, false);
    owe::GetJsonValue(json, "attachment", attachment, false);
    owe::GetJsonValue(json, "dependencies", dependencies, false);
    owe::GetJsonValue(json, "controlpoint", controlpoint, false);
    if (auto value = json.get("instance"); value.is_some()) instance = (*value)->clone();
    if (auto value = json.get("particlesrc"); value.is_some()) particlesrc = (*value)->clone();

    AbsorbAllFieldBindings(json, field_bindings);

    auto jParticle = LoadJsonFile(vfs, "/assets/" + particle);
    if (! jParticle) return false;
    if (! particleObj.FromJson(*jParticle, vfs)) return false;
    return true;
}
