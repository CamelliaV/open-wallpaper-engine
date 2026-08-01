module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import :scene_context;
import eigen;
import wescene.spec_names;
import wescene.load_bench;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

float ParticleTextureRatio(const SceneMaterial& material) {
    auto it = material.customShader.constValues.find(WE_GLTEX_RESOLUTION_NAMES[usize()]);
    if (it == material.customShader.constValues.end()) return 1.0f;
    const auto& r = it->second;
    if (r.size() < usize(2) || r[usize(0)] == 0.0f) return 1.0f;
    return r[usize(1)] / r[usize(0)];
}

struct ParticleRenderDesc {
    bool rope { false };
    bool rope_trail { false };
    bool trail { false };
};

ParticleRenderDesc DescribeParticleRender(const wpscene::ParticleRender& render) {
    ParticleRenderDesc desc;
    desc.rope       = render.name == "rope";
    desc.rope_trail = render.name == "ropetrail";
    desc.trail      = send_with(render.name, "trail");
    return desc;
}

ParticleAnimationMode ToAnimMode(const std::string& str) {
    if (str == "randomframe")
        return ParticleAnimationMode::RANDOMONE;
    else if (str == "sequence")
        return ParticleAnimationMode::SEQUENCE;
    else {
        return ParticleAnimationMode::SEQUENCE;
    }
}

void ApplyParticleOverride(wpscene::ParticleInstanceoverride& state, ref<str> field,
                           slice<float> values) {
    auto write_scalar = [&](float& destination) {
        if (values.len() >= usize(1)) destination = values[usize()];
    };
    auto write_vec3 = [&](std::array<float, 3>& destination, float scale) -> bool {
        if (values.len() < usize(3)) return false;
        destination = { values[usize()] * scale,
                        values[usize(1)] * scale,
                        values[usize(2)] * scale };
        return true;
    };

    auto parse_index = [&](ref<str> prefix) -> Option<usize> {
        auto suffix = field.strip_prefix(prefix);
        if (suffix.is_none()) return None();
        auto parsed = rstd::from_str<usize>(*suffix);
        if (parsed.is_err()) return None();
        auto index = rstd::move(parsed).unwrap_unchecked();
        return index < usize(8) ? Some(index) : None();
    };

    if (field == "alpha"_str)
        write_scalar(state.alpha);
    else if (field == "size"_str)
        write_scalar(state.size);
    else if (field == "lifetime"_str)
        write_scalar(state.lifetime);
    else if (field == "rate"_str)
        write_scalar(state.rate);
    else if (field == "speed"_str)
        write_scalar(state.speed);
    else if (field == "count"_str)
        write_scalar(state.count);
    else if (field == "brightness"_str)
        write_scalar(state.brightness);
    else if (field == "color"_str) {
        write_vec3(state.color, 255.0f);
        state.overColor = true;
    } else if (field == "colorn"_str) {
        write_vec3(state.colorn, 1.0f);
        state.overColorn = true;
    } else if (auto index = parse_index("controlpointangle"_str); index.is_some()) {
        write_vec3(state.controlpointangle[index->to_primitive()], 1.0f);
    } else if (auto index = parse_index("controlpoint"_str); index.is_some()) {
        std::array<float, 3> point {};
        if (write_vec3(point, 1.0f)) state.controlpoint[index->to_primitive()] = point;
    }
}

Vec<float> ReadParticleOverride(const wpscene::ParticleInstanceoverride& state, ref<str> field) {
    auto scalar = [](float value) {
        Vec<float> out;
        out.push(float(value));
        return out;
    };
    auto vec3 = [](const std::array<float, 3>& value, float scale) {
        Vec<float> out;
        out.push(value[0] * scale);
        out.push(value[1] * scale);
        out.push(value[2] * scale);
        return out;
    };
    if (field == "alpha"_str) return scalar(state.alpha);
    if (field == "size"_str) return scalar(state.size);
    if (field == "lifetime"_str) return scalar(state.lifetime);
    if (field == "rate"_str) return scalar(state.rate);
    if (field == "speed"_str) return scalar(state.speed);
    if (field == "count"_str) return scalar(state.count);
    if (field == "brightness"_str) return scalar(state.brightness);
    if (field == "color"_str) return vec3(state.color, 1.0f / 255.0f);
    if (field == "colorn"_str) return vec3(state.colorn, 1.0f);
    return {};
}

struct ParticleOverrideControl {
    Arc<wpscene::ParticleInstanceoverride> state;
    String                                 field;

    void Apply(slice<float> values) { ApplyParticleOverride(*state, field.as_str(), values); }
};

struct ParticleNodeControl {
    Arc<wpscene::ParticleInstanceoverride> state;
    Arc<ParticlePlaybackState>             playback;

    Vec<float> Get(ref<str> field) const { return ReadParticleOverride(*state, field); }
    void       Apply(ref<str> field, slice<float> values) {
        ApplyParticleOverride(*state, field, values);
    }
    void Play() {
        playback->playing.store(true, rstd::sync::atomic::Ordering::Release);
        playback->reset_sequence.fetch_add(u32(1), rstd::sync::atomic::Ordering::AcqRel);
    }
    void Stop() {
        playback->playing.store(false, rstd::sync::atomic::Ordering::Release);
        playback->reset_sequence.fetch_add(u32(1), rstd::sync::atomic::Ordering::AcqRel);
    }
    void Pause() { playback->playing.store(false, rstd::sync::atomic::Ordering::Release); }
    bool IsPlaying() const { return playback->playing.load(rstd::sync::atomic::Ordering::Acquire); }
};

void LoadControlPoint(ParticleSubSystem& system, const wpscene::Particle& particle,
                      Arc<wpscene::ParticleInstanceoverride> instance_override) {
    auto points = system.ControlpointsMut();
    auto count  = rstd::cmp::min(points.len(), usize(particle.controlpoints.size()));
    for (usize index {}; index < count; ++index) {
        auto source_index         = index.to_primitive();
        points[index].base_offset = Eigen::Vector3d {
            array_cast<double>(particle.controlpoints[source_index].offset).data()
        };
        points[index].offset     = points[index].base_offset;
        points[index].link_mouse = particle.controlpoints[source_index]
                                       .flags[wpscene::ParticleControlpoint::FlagEnum::link_mouse];
        points[index].worldspace = particle.controlpoints[source_index]
                                       .flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
    }
    system.SetInstanceOverride(instance_override.clone());
    if (! instance_override->field_bindings) return;
    for (usize index {}; index < points.len(); ++index) {
        auto field = std::string("controlpointangle") + std::to_string(index.to_primitive());
        auto curve = instance_override->field_bindings->animations.find(field);
        if (curve != instance_override->field_bindings->animations.end())
            system.SetControlpointAngleCurve(index, ToSceneAnimationCurve(curve->second));
    }
}
void LoadInitializer(ParticleSubSystem& system, const wpscene::Particle& particle,
                     Arc<wpscene::ParticleInstanceoverride> over_state) {
    u32 implicit_sequence_count { 2 };
    for (const auto& emitter : particle.emitters) {
        if (emitter.max_emit_per_period > u32()) {
            implicit_sequence_count = emitter.max_emit_per_period;
            break;
        }
    }
    for (const auto& initializer : particle.initializers) {
        auto instruction = ParticleParser::GenInitializer(initializer, implicit_sequence_count);
        auto count       = instruction.SequenceCount();
        if (count.is_some()) system.SetRopeSequenceCount(*count);
        system.AddInitializer(rstd::move(instruction));
    }
    if (over_state->enabled) {
        system.AddInitializer(ParticleParser::GenOverride(rstd::move(over_state)));
    }
}
void LoadOperator(ParticleSubSystem& system, const wpscene::Particle& particle,
                  Arc<wpscene::ParticleInstanceoverride> over_state) {
    usize index {};
    for (const auto& operation : particle.operators) {
        system.AddOperator(
            ParticleParser::GenOperator(operation, over_state.clone(), system, index++));
    }
}
void LoadEmitter(ParticleSubSystem& system, const wpscene::Particle& particle, float count) {
    usize emitter_index {};
    for (const auto& em : particle.emitters) {
        auto newEm = em;
        newEm.rate *= count;
        system.AddEmitter(ParticleParser::GenEmitter(newEm, system, emitter_index++));
    }
}

ParticleSubSystem::SpawnType ParseSpawnType(std::string_view str) {
    using ST = ParticleSubSystem::SpawnType;
    ST type { ST::STATIC };
    if (str == "eventfollow") {
        type = ST::EVENT_FOLLOW;
    } else if (str == "eventspawn") {
        type = ST::EVENT_SPAWN;
    } else if (str == "eventdeath") {
        type = ST::EVENT_DEATH;
    }
    return type;
};

struct ParticleChildPtr {
    wpscene::ParticleChild*            child { nullptr };
    SceneNode*                         node_parent { nullptr };
    ParticleSubSystem*                 particle_parent { nullptr };
    Option<Arc<ParticlePlaybackState>> playback;
    bool                               inherit_instance_override { false };

    // Effective world scale at node_parent. Particle child origins are
    // pre-divided by this so the shader's MVP scale recovers the authored
    // parent-relative world-pixel offset.
    Eigen::Vector3f world_scale { 1.f, 1.f, 1.f };
};

wpscene::ParticleInstanceoverride ParticleOverrideForNode(const wpscene::ParticleObject& obj,
                                                          bool                           is_child,
                                                          bool inherit_instance_override) {
    if (! is_child) return obj.instanceoverride;

    wpscene::ParticleInstanceoverride out;
    if (! inherit_instance_override) return out;
    const auto& parent = obj.instanceoverride;
    out.enabled        = parent.enabled;
    out.alpha          = parent.alpha;
    out.overColor      = parent.overColor;
    out.overColorn     = parent.overColorn;
    out.color          = parent.color;
    out.colorn         = parent.colorn;
    for (std::string_view field : { "alpha", "color", "colorn" }) {
        if (auto it = parent.bindings.find(std::string(field)); it != parent.bindings.end()) {
            out.bindings.emplace(it->first, it->second);
        }
    }
    return out;
}

void SetParticleUniformConfig(ParticleObjectParseOutput& output, const Arc<SceneNode>& node,
                              UniformNodeConfigDraft config) {
    config.configured = true;
    for (auto& entry : output.uniform_configs) {
        if (entry.node.as_ptr() != node.as_ptr()) continue;
        entry.config = rstd::move(config);
        return;
    }
    output.uniform_configs.push(SceneUniformConfigDraft {
        .node   = node.clone(),
        .config = rstd::move(config),
    });
}

void BuildParticleObjectNode(ParticleObjectParseServices& services,
                             ParticleObjectParseOutput& output, wpscene::ParticleObject& wppartobj,
                             ParticleChildPtr child_ptr = {}) {
    struct ChildData {
        ChildData() = default;
        ChildData(const wpscene::ParticleChild& o)
            : type(o.type),
              maxcount(o.maxcount),
              controlpointstartindex(o.controlpointstartindex),
              probability(o.probability) {}
        std::string type { "static" };
        i32         maxcount { 20 };
        Option<i32> controlpointstartindex;
        float       probability { 1.0f };
    };

    wpscene::Particle*     p_particle_obj { nullptr };
    Option<Arc<SceneNode>> spNodeOpt;
    ChildData              child_data;

    bool is_child = child_ptr.child != nullptr;
    if (is_child) {
        p_particle_obj = &(child_ptr.child->obj);
        // ParticleChild::origin is a WE world-pixel offset from the parent
        // particle. SceneNode hierarchy composes T(local) * S(parent) so
        // the local translation gets multiplied by parent scale at render
        // time; pre-divide so the world translation matches the JSON.
        Vector3f corigin(child_ptr.child->origin.data());
        for (int i = 0; i < 3; ++i) {
            float s = child_ptr.world_scale[i];
            if (std::abs(s) > 1e-6f) corigin[i] /= s;
        }
        spNodeOpt  = Some(Arc<SceneNode>::make(corigin,
                                               Vector3f(child_ptr.child->scale.data()),
                                               Vector3f(child_ptr.child->angles.data()),
                                               child_ptr.child->name));
        child_data = ChildData(*child_ptr.child);

    } else {
        p_particle_obj = &wppartobj.particleObj;
        spNodeOpt      = Some(Arc<SceneNode>::make(Vector3f(wppartobj.origin.data()),
                                                   Vector3f(wppartobj.scale.data()),
                                                   Vector3f(wppartobj.angles.data()),
                                                   wppartobj.name));
        auto& spNode   = *spNodeOpt;
        spNode->ID()   = i32(wppartobj.id);
        if (! wppartobj.visible) {
            spNode->SetVisible(false);
            services.scene->MarkLayerVisibilityElidable(
                WallpaperLayerId { .value = static_cast<i32>(wppartobj.id) });
        }
        if (! wppartobj.visible_user.empty())
            spNode->SetVisibleUserBinding(ToSceneUserVisibilityBinding(wppartobj.visible_user));
    }
    auto& spNode = *spNodeOpt;
    spNode->SetReflected(wppartobj.reflected);

    // Effective world scale at this SceneNode: parent's world scale times
    // this node's local scale. Propagated to child particle nodes.
    Eigen::Vector3f node_world_scale = child_ptr.world_scale.cwiseProduct(spNode->Scale());

    // The placed object's opacity/tint enters its direct child presets only. A preset's own
    // children keep their authored values instead of inheriting the scene override transitively.
    auto override_state = Arc<wpscene::ParticleInstanceoverride>::make(
        ParticleOverrideForNode(wppartobj, is_child, child_ptr.inherit_instance_override));
    auto& override       = *override_state;
    auto  playback_state = is_child && child_ptr.playback.is_some()
                               ? (*child_ptr.playback).clone()
                               : Arc<ParticlePlaybackState>::make();

    auto& particle_obj = *p_particle_obj;
    auto& vfs          = *services.vfs;

    auto wppartRenderer    = particle_obj.renderers.at(0);
    auto render_desc       = DescribeParticleRender(wppartRenderer);
    bool render_rope       = render_desc.rope;
    bool render_rope_trail = render_desc.rope_trail;
    bool rope_shader       = render_rope || render_rope_trail;
    bool hastrail          = render_desc.trail;

    if (rope_shader) particle_obj.material.shader = "genericropeparticle";

    // wppartobj.origin[1] = context.ortho_h - wppartobj.origin[1];

    if (particle_obj.flags[wpscene::Particle::FlagEnum::perspective]) {
        spNode->SetCamera("global_perspective");
    }

    SceneMaterial          material;
    UniformNodeConfigDraft svData;

    if (! is_child) {
        svData.parallax_depth = { wppartobj.parallaxDepth[0], wppartobj.parallaxDepth[1] };
        svData.propagated_parallax_depth = { wppartobj.parallaxDepth[0],
                                             wppartobj.parallaxDepth[1] };
    }
    svData.use_camera_eye_position = particle_obj.flags[wpscene::Particle::FlagEnum::perspective];
    svData.vertices_in_world_space = particle_obj.flags[wpscene::Particle::FlagEnum::wordspace];

    ShaderInfo shaderInfo;
    shaderInfo.baseConstSvs = services.global_base_uniforms;
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ORIENTATIONUP)] =
        std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ORIENTATIONRIGHT)] =
        std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ORIENTATIONFORWARD)] =
        std::array { 0.0f, 0.0f, 1.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_VIEWUP)]    = std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_VIEWRIGHT)] = std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_EYEPOSITION)] = std::array {
        static_cast<float>(services.ortho_w) / 2.0f,
        static_cast<float>(services.ortho_h) / 2.0f,
        1000.0f,
    };

    std::uint32_t maxcount = particle_obj.maxcount;
    maxcount               = std::min(maxcount, 20000u);

    Option<Arc<ParticleTrailUniformState>> trail_uniform_state;
    if (hastrail) {
        double          in_SegmentUVTimeOffset = 0.0;
        double          in_SegmentMaxCount     = maxcount - 1.0;
        array<float, 4> render_var {
            (float)wppartRenderer.length,
            (float)wppartRenderer.maxlength,
            (float)in_SegmentUVTimeOffset,
            (float)in_SegmentMaxCount,
        };
        shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_RENDERVAR0)] = render_var;
        if (render_rope_trail) {
            trail_uniform_state = Some(Arc<ParticleTrailUniformState>::make(
                ParticleTrailUniformState { .render_var = render_var }));
        }
        shaderInfo.combos[rstd::cppstd::to_string(WE_CB_TRAILRENDERER)] = "1";
        if (! render_rope_trail)
            shaderInfo.combos[rstd::cppstd::to_string(WE_CB_THICK_FORMAT)] = "1";
    }
    if (rope_shader) {
        std::int32_t subdiv = static_cast<std::int32_t>(std::round(wppartRenderer.subdivision));
        if (subdiv < 0) subdiv = 0;
        shaderInfo.combos["TRAILSUBDIVISION"] = std::to_string(subdiv);
    }

    auto animationmode = ToAnimMode(particle_obj.animationmode);
    if (animationmode == ParticleAnimationMode::SEQUENCE &&
        ! particle_obj.flags[wpscene::Particle::FlagEnum::spritenoframeblending]) {
        shaderInfo.combos["SPRITESHEETBLEND"] = "1";
    }

    bool mat_ok = false;
    try {
        auto material_result = BuildMaterial(vfs,
                                             *services.shader_cache,
                                             services.shader_environment,
                                             particle_obj.material,
                                             *services.scene,
                                             rstd::move(shaderInfo),
                                             GeometryStageRequirement::Required);
        if (material_result.is_ok()) {
            auto material_build = rstd::move(material_result).unwrap_unchecked();
            material            = rstd::move(material_build.material);
            shaderInfo          = rstd::move(material_build.shader_info);
            mat_ok              = true;
        }
    } catch (const std::exception& e) {
        rstd_error("load particleobj '{}' material exception: {}", wppartobj.name, e.what());
    }
    if (! mat_ok) {
        rstd_error("load particleobj '{}' material faild", wppartobj.name);
        return;
    }
    LoadConstvalue(material, particle_obj.material, shaderInfo);
    auto  spMesh             = std::make_shared<SceneMesh>(true);
    auto& mesh               = *spMesh;
    auto  sequencemultiplier = particle_obj.sequencemultiplier;
    bool  hasSprite          = material.hasSprite;
    (void)hasSprite;

    bool          thick_format = material.hasSprite || (hastrail && ! render_rope_trail);
    std::uint32_t trail_length = 0;
    if (render_rope_trail) {
        std::int32_t segments = wppartRenderer.segments.to_primitive();
        segments              = std::clamp(segments, 1, 256);
        trail_length          = static_cast<std::uint32_t>(segments);
    }
    ParticleFollowAnchor follow_anchor;
    if (hastrail && ! render_rope_trail) {
        follow_anchor.trail_renderer = true;
        follow_anchor.length         = wppartRenderer.length;
        follow_anchor.max_length     = wppartRenderer.maxlength;
        follow_anchor.texture_ratio  = ParticleTextureRatio(material);
    }

    auto spawn_type = ParseSpawnType(child_data.type);
    if (is_child && spawn_type == ParticleSubSystem::SpawnType::STATIC &&
        child_data.controlpointstartindex.is_some()) {
        spawn_type = ParticleSubSystem::SpawnType::STATIC_CONTROLPOINT;
    }
    auto max_instance_count = u32(
        static_cast<std::uint32_t>(std::max(child_data.maxcount.to_primitive(), std::int32_t(0))));
    auto particleSub = Box<ParticleSubSystem>::make(
        *services.scene,
        spMesh,
        u32(maxcount),
        f64(override.rate),
        max_instance_count,
        f64(child_data.probability),
        spawn_type,
        ParticleAnimationSpec {
            .mode                = animationmode,
            .sequence_multiplier = sequencemultiplier,
        },
        follow_anchor,
        u32(trail_length),
        f64(render_rope_trail ? static_cast<double>(wppartRenderer.length) : 0.0),
        f64(static_cast<double>(particle_obj.starttime)),
        particle_obj.flags[wpscene::Particle::FlagEnum::wordspace],
        trail_uniform_state.is_some() ? Some((*trail_uniform_state).clone()) : None());

    {
        auto mesh_capacity = particleSub->MaxParticleCapacity();
        if (mesh_capacity.is_none()) {
            rstd_error("particle mesh capacity overflow for '{}'", spNode->Name());
            return;
        }
        auto mesh_maxcount = mesh_capacity->to_primitive();
        if (rope_shader) {
            if (render_rope_trail) {
                auto capacity = mesh_capacity->checked_mul(u32(trail_length));
                if (capacity.is_none()) {
                    rstd_error("particle rope capacity overflow for '{}'", spNode->Name());
                    return;
                }
                mesh_maxcount = capacity->to_primitive();
            }
            SetRopeParticleMesh(mesh, particle_obj, mesh_maxcount, thick_format, render_rope_trail);
        } else {
            SetParticleMesh(mesh, mesh_maxcount, thick_format);
        }
    }

    particleSub->SetOwnerNode(spNode.as_ptr());
    particleSub->SetPlaybackState(playback_state.clone());
    if (child_data.controlpointstartindex.is_some())
        particleSub->SetParentControlpointStartIndex(*child_data.controlpointstartindex);
    LoadEmitter(*particleSub, particle_obj, override.count);
    LoadInitializer(*particleSub, particle_obj, override_state.clone());
    LoadOperator(*particleSub, particle_obj, override_state.clone());
    LoadControlPoint(*particleSub, particle_obj, override_state.clone());
    particleSub->Finalize();

    // Register every {user:"<key>", value:...} binding on instanceoverride
    // so RenderSetUserProperty can mutate the shared state at runtime.
    for (const auto& [field, key] : override.bindings) {
        services.scene->RegisterParticleOverrideBinding(
            String::make(as_str(key).unwrap()),
            Arc<dyn<SceneParticleOverrideControl>>::make(ParticleOverrideControl {
                .state = override_state.clone(),
                .field = String::make(as_str(field).unwrap()),
            }));
    }

    mesh.AddMaterial(std::move(material));
    RegisterMaterialBindings(*services.scene, *mesh.Material(), particle_obj.material, shaderInfo);
    if (services.construction_context != nullptr) {
        WireMaterialShaderValueScripts(*services.construction_context,
                                       spNode,
                                       mesh.MaterialSlots().back(),
                                       particle_obj.material,
                                       shaderInfo);
    }
    spNode->AddMesh(spMesh);
    SetParticleUniformConfig(output, spNode, rstd::move(svData));
    if (trail_uniform_state.is_some()) {
        output.trail_uniform_configs.push(ParticleTrailUniformConfigDraft {
            .node          = spNode.clone(),
            .uniform_state = rstd::move(*trail_uniform_state),
        });
    }

    for (auto& child : particle_obj.children) {
        BuildParticleObjectNode(services,
                                output,
                                wppartobj,
                                {
                                    .child                     = &child,
                                    .node_parent               = spNode.as_ptr(),
                                    .particle_parent           = particleSub.get(),
                                    .playback                  = Some(playback_state.clone()),
                                    .inherit_instance_override = ! is_child,
                                    .world_scale               = node_world_scale,
                                });
    }

    if (is_child)
        child_ptr.particle_parent->AddChild(std::move(particleSub));
    else
        services.particle_runtime->Add(rstd::move(particleSub));

    if (! is_child) {
        spNode->SetParticleControl(Arc<dyn<SceneParticleControl>>::make(ParticleNodeControl {
            .state    = override_state.clone(),
            .playback = playback_state.clone(),
        }));
        AssignNodeFieldAnimations(*spNode.as_ptr(), wppartobj.field_bindings);
    }
    if (services.construction_context != nullptr)
        WireFieldScripts(*services.construction_context, spNode, wppartobj.field_bindings);
    if (is_child)
        child_ptr.node_parent->AppendChild(spNode.clone());
    else
        output.root = Some(spNode.clone());
}

auto BuildParticleObjectImpl(ParticleObjectParseServices& services,
                             wpscene::ParticleObject&     particle) -> ParticleObjectParseOutput {
    ParticleObjectParseOutput output;
    BuildParticleObjectNode(services, output, particle);
    return output;
}

void ParseParticleObjImpl(SceneParseContext& context, wpscene::ParticleObject& particle) {
    if (context.particle_runtime.is_none()) return;
    if (! particle.particle.empty() && ! context.dynamic_particle_prototypes.contains_key(
                                           rstd::cppstd::as_str(particle.particle).unwrap())) {
        (void)context.dynamic_particle_prototypes.insert(
            String::make(rstd::cppstd::as_str(particle.particle).unwrap()), particle.Clone());
    }

    ParticleObjectParseServices services {
        .scene                = context.scene.get(),
        .vfs                  = context.vfs,
        .shader_cache         = context.shader_cache.clone(),
        .shader_environment   = context.shader_environment,
        .global_base_uniforms = context.global_base_uniforms,
        .particle_runtime     = (*context.particle_runtime).clone(),
        .ortho_w              = context.ortho_w,
        .ortho_h              = context.ortho_h,
        .construction_context = &context,
    };
    auto output = BuildParticleObject(services, particle);
    if (output.root.is_none()) return;
    for (auto& draft : output.uniform_configs) context.uniform_configs.push(rstd::move(draft));
    for (auto& draft : output.trail_uniform_configs)
        context.particle_trail_uniform_configs.push(rstd::move(draft));
    RegisterNodeRef(context,
                    particle.id,
                    SceneParseContext::NodeRef {
                        particle.parent,
                        Some(rstd::move(*output.root)),
                        None(),
                        String::make(rstd::cppstd::as_str(particle.attachment).unwrap()),
                    });
}

void ParseParticleObj(SceneParseContext& context, wpscene::ParticleObject& particle) {
    ParseParticleObjImpl(context, particle);
}

auto BuildParticleObject(ParticleObjectParseServices& services, wpscene::ParticleObject& particle)
    -> ParticleObjectParseOutput {
    return BuildParticleObjectImpl(services, particle);
}

} // namespace owe
