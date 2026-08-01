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
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

template<typename T>
struct CopyableArcHold {
    Arc<T> value;

    explicit CopyableArcHold(Arc<T> owner): value(rstd::move(owner)) {}
    CopyableArcHold(const CopyableArcHold& other): value(other.value.clone()) {}
    CopyableArcHold(CopyableArcHold&&) noexcept            = default;
    CopyableArcHold& operator=(CopyableArcHold&&) noexcept = default;
    CopyableArcHold& operator=(const CopyableArcHold&)     = delete;
};

bool RegisterUniformNodeSources(Scene& scene, const Arc<UniformSceneState>& uniform_state,
                                const Arc<UniformCameraResolver>& camera_resolver,
                                const Arc<SceneNode>& node, const UniformNodeConfigDraft& config) {
    auto node_id = scene.ResourceIndex().nodeId(*node);
    if (node_id.is_none()) return false;

    auto state = Arc<UniformNodeState>::make(node.clone(), camera_resolver.clone());
    state->propagated_parallax_depth      = config.propagated_parallax_depth;
    state->propagate_parallax_to_children = config.propagate_parallax_to_children;
    state->use_camera_eye_position        = config.use_camera_eye_position;
    state->eye_position_override          = config.eye_position_override;
    state->vertices_in_world_space        = config.vertices_in_world_space;
    state->effect_projection_size         = config.effect_projection_size;
    if (config.effect_projection_node.is_some())
        state->effect_projection_node = Some((*config.effect_projection_node).clone());
    uniform_state->SetNodeState(*node_id, state.clone());

    auto       registrar = dyn<UniformSourceRegistrar>::from_ref(scene);
    auto       writer    = dyn<UniformAttachmentWriter>::from_ref(scene);
    const auto transform = registrar->Register(Box<dyn<UniformSource>>::make(
        TransformUniformSource { uniform_state.clone(), rstd::move(state) }));
    const auto color =
        registrar->Register(Box<dyn<UniformSource>>::make(ColorUniformSource { node.clone() }));
    const auto texture =
        registrar->Register(Box<dyn<UniformSource>>::make(TextureUniformSource {}));
    (void)writer->AttachNode(*node_id, transform, 0);
    (void)writer->AttachNode(*node_id, color, 0);
    (void)writer->AttachNode(*node_id, texture, 0);
    return true;
}

bool RegisterParticleTrailUniformSource(Scene& scene, const Arc<SceneNode>& node,
                                        const Arc<ParticleTrailUniformState>& state) {
    auto node_id = scene.ResourceIndex().nodeId(*node);
    if (node_id.is_none()) return false;
    auto       registrar = dyn<UniformSourceRegistrar>::from_ref(scene);
    auto       writer    = dyn<UniformAttachmentWriter>::from_ref(scene);
    const auto source    = registrar->Register(
        Box<dyn<UniformSource>>::make(ParticleTrailUniformSource { state.clone() }));
    (void)writer->AttachNode(*node_id, source, 10);
    return true;
}

void FinalizeUniformSources(SceneParseContext& context) {
    auto& scene = *context.scene;
    scene.RebuildResourceIndex();

    auto active_camera = scene.ActiveCameraHandle();
    if (active_camera.is_none()) return;
    auto camera_for = [&](const SceneNode& node) -> Option<Arc<SceneCamera>> {
        if (! node.Camera().empty()) {
            return scene.CameraHandle(rstd::cppstd::as_str(node.Camera()).unwrap());
        }
        if (node.Perspective()) {
            return scene.CameraHandle("global_perspective"_str);
        }
        return Some((*active_camera).clone());
    };
    auto camera_resolver = Arc<UniformCameraResolver>::make((*active_camera).clone());
    auto camera_names    = scene.CameraNames();
    camera_resolver->Reserve(camera_names.len());
    for (usize index {}; index < camera_names.len(); ++index) {
        const auto& name   = camera_names[index];
        auto        camera = scene.CameraHandle(name.as_str());
        if (camera.is_some()) camera_resolver->Add(name.clone(), rstd::move(*camera));
    }

    auto ortho = scene.Ortho();
    context.uniform_state->SetOrtho(static_cast<float>(ortho[usize()].to_primitive()),
                                    static_cast<float>(ortho[usize(1)].to_primitive()));
    scene.Runtime().RegisterSystem(UniformRuntimeSystem { context.uniform_state.clone() });

    auto registrar = dyn<UniformSourceRegistrar>::from_ref(scene);
    auto writer    = dyn<UniformAttachmentWriter>::from_ref(scene);

    const auto frame_source = registrar->Register(
        Box<dyn<UniformSource>>::make(FrameUniformSource { context.uniform_state.clone() }));
    const auto audio_source = registrar->Register(
        Box<dyn<UniformSource>>::make(AudioUniformSource { context.uniform_state.clone() }));
    (void)writer->AttachGlobal(frame_source, 0);
    (void)writer->AttachGlobal(audio_source, 0);

    Vec<ref<SceneLight>> lights;
    auto                 owned_lights = scene.Lights();
    lights.reserve(owned_lights.len());
    for (usize index {}; index < owned_lights.len(); ++index)
        lights.push(owned_lights[index].as_ref());
    const auto light_source = registrar->Register(
        Box<dyn<UniformSource>>::make(LightUniformSource { rstd::move(lights) }));
    (void)writer->AttachGlobal(light_source, 0);

    for (auto& draft : context.text_uniform_configs) {
        auto node_id = scene.ResourceIndex().nodeId(*draft.node);
        if (node_id.is_none()) continue;
        auto state               = std::make_shared<text::TextUniformState>(draft.node.clone());
        state->camera            = camera_for(*draft.node);
        state->active_camera     = Some((*active_camera).clone());
        state->effect_projection = draft.effect_projection;
        const auto source        = registrar->Register(
            Box<dyn<UniformSource>>::make(text::TextUniformSource { rstd::move(state) }));
        (void)writer->AttachNode(*node_id, source, 0);
    }

    for (auto& entry : context.uniform_configs) {
        (void)RegisterUniformNodeSources(
            scene, context.uniform_state, camera_resolver, entry.node, entry.config);
    }

    for (auto& draft : context.particle_trail_uniform_configs) {
        (void)RegisterParticleTrailUniformSource(scene, draft.node, draft.uniform_state);
    }

    HashMap<PuppetLayer*, UniformSourceId> puppet_sources;
    context.puppet_layers->by_node.iter().for_each([&](auto entry) {
        auto [node_ref, layer_ref] = entry;
        auto*       node           = *node_ref;
        const auto& layer          = *layer_ref;
        if (node == nullptr) return;
        auto node_id = scene.ResourceIndex().nodeId(*node);
        if (node_id.is_none()) return;
        auto* key    = layer.as_ptr().as_raw_ptr();
        auto  source = puppet_sources.get(key);
        if (source.is_none()) {
            auto registered = registrar->Register(
                Box<dyn<UniformSource>>::make(PuppetUniformSource { layer.clone() }));
            (void)puppet_sources.insert(key, registered);
            (void)writer->AttachNode(*node_id, registered, 10);
        } else {
            (void)writer->AttachNode(*node_id, **source, 10);
        }
    });

    if (! context.dynamic_image_prototypes.is_empty() ||
        ! context.dynamic_particle_prototypes.is_empty()) {
        auto scripts = scene.ExtensionMut<script::ScriptScene>();
        if (scripts.is_some()) {
            auto image_prototypes    = rstd::move(context.dynamic_image_prototypes);
            auto particle_prototypes = rstd::move(context.dynamic_particle_prototypes);
            auto particle_runtime    = context.particle_runtime.is_some()
                                           ? Some((*context.particle_runtime).clone())
                                           : None<Arc<ParticleRuntime>>();
            auto scene_ptr           = rstd::addressof(scene);
            (**scripts).runtime().SetLayerFactory(script::JsRuntime::LayerFactory::make(
                [scene_ptr,
                 image_prototypes     = rstd::move(image_prototypes),
                 particle_prototypes  = rstd::move(particle_prototypes),
                 particle_runtime     = rstd::move(particle_runtime),
                 shader_cache         = context.shader_cache.clone(),
                 shader_environment   = context.shader_environment,
                 global_base_uniforms = context.global_base_uniforms,
                 ortho_w              = context.ortho_w,
                 ortho_h              = context.ortho_h,
                 uniform_state        = context.uniform_state.clone(),
                 camera_resolver      = camera_resolver.clone()](
                    SceneNode* owner, ref<str> asset) mutable -> Option<Arc<SceneNode>> {
                    SceneNode* parent = owner && owner->Parent()
                                            ? owner->Parent()
                                            : scene_ptr->RootMut().as_raw_ptr();

                    if (auto prototype = image_prototypes.get(asset); prototype.is_some()) {
                        auto node =
                            CloneRegisteredNode(*scene_ptr, (**prototype).node.deref(), asset);
                        scene_ptr->AttachRuntimeNode(*parent, node.clone());
                        if (! RegisterUniformNodeSources(*scene_ptr,
                                                         uniform_state,
                                                         camera_resolver,
                                                         node,
                                                         (**prototype).uniform_config)) {
                            rstd_error("registered image asset '{}' has no runtime resource id",
                                       asset);
                            return None();
                        }
                        return Some(rstd::move(node));
                    }

                    auto prototype = particle_prototypes.get(asset);
                    if (prototype.is_none() || particle_runtime.is_none()) return None();
                    auto vfs = scene_ptr->ExtensionMut<fs::VFS>();
                    if (vfs.is_none()) {
                        rstd_error("registered particle asset '{}' has no runtime VFS", asset);
                        return None();
                    }

                    auto particle    = (**prototype).Clone();
                    particle.id      = -1;
                    particle.name    = rstd::cppstd::to_string(asset);
                    particle.origin  = { 0.0f, 0.0f, 0.0f };
                    particle.scale   = { 1.0f, 1.0f, 1.0f };
                    particle.angles  = { 0.0f, 0.0f, 0.0f };
                    particle.parent  = 0;
                    particle.visible = true;
                    ParticleObjectParseServices particle_services {
                        .scene                = scene_ptr,
                        .vfs                  = (*vfs).as_raw_ptr(),
                        .shader_cache         = shader_cache.clone(),
                        .shader_environment   = shader_environment,
                        .global_base_uniforms = global_base_uniforms,
                        .particle_runtime     = (*particle_runtime).clone(),
                        .ortho_w              = ortho_w,
                        .ortho_h              = ortho_h,
                    };
                    auto parsed = BuildParticleObject(particle_services, particle);
                    if (parsed.root.is_none()) return None();
                    auto node = rstd::move(*parsed.root);
                    scene_ptr->RegisterNode(*node);
                    scene_ptr->AttachRuntimeNode(*parent, node.clone());
                    for (auto& entry : parsed.uniform_configs) {
                        (void)RegisterUniformNodeSources(
                            *scene_ptr, uniform_state, camera_resolver, entry.node, entry.config);
                    }
                    for (auto& draft : parsed.trail_uniform_configs) {
                        (void)RegisterParticleTrailUniformSource(
                            *scene_ptr, draft.node, draft.uniform_state);
                    }
                    return Some(rstd::move(node));
                }));
        }
    }
}

Box<Scene> FinalizeScene(SceneParseContext& context) {
    // Attach once after every registered node has been created in JSON
    // declaration order (node_id_order) but not yet inserted into the scene
    // graph. Walk that order and AppendChild to parent (or root). Result:
    // child lists at every depth match scene.json declaration order, which
    // is what WE treats as z-order.
    int attached = 0, missing_parent = 0;
    for (auto id : context.node_id_order) {
        auto found = context.node_id_map.get_mut(id);
        if (found.is_none() || (**found).node.is_none()) continue;
        auto&                             ref         = **found;
        SceneNode*                        parent_node = context.scene->RootMut().as_raw_ptr();
        const SceneParseContext::NodeRef* parent_ref  = nullptr;
        if (ref.parent_id != 0) {
            auto parent = context.node_id_map.get(static_cast<std::int32_t>(ref.parent_id));
            if (parent.is_none() || (**parent).node.is_none()) {
                missing_parent++;
                continue;
            }
            parent_node = (*(**parent).node).as_ptr();
            parent_ref  = &**parent;
        }
        // Named MDAT anchors provide the child's local frame in the parent
        // puppet's bind space.
        if (! ref.attachment.is_empty() && parent_ref && parent_ref->puppet.is_some()) {
            const auto& puppet           = **parent_ref->puppet;
            auto        attachment_index = puppet.attachmentIndex(ref.attachment.as_str());
            if (attachment_index.is_some()) {
                auto apply_bind_offset = [&]() {
                    auto anchor = puppet.attachmentBindTransform(*attachment_index);
                    if (anchor.is_none()) return;
                    if (ref.apply_attachment_offset.is_some()) {
                        (*ref.apply_attachment_offset)->operator()(anchor->translation());
                    } else {
                        (*ref.node)->SetLocalFrame(anchor->matrix().cast<double>() *
                                                   (*ref.node)->LocalFrame());
                    }
                };
                if (ref.apply_attachment_offset.is_none() && parent_ref->puppet_layer.is_some()) {
                    SceneNode* node       = (*ref.node).as_ptr();
                    auto       layer      = CopyableArcHold((*parent_ref->puppet_layer).clone());
                    auto       local_base = node->LocalFrame();
                    auto       update     = [node,
                                             layer,
                                             attachment_index = *attachment_index,
                                             local_base       = rstd::move(local_base)](f64 time) {
                        auto anchor =
                            layer.value->attachmentTransform(attachment_index, time.to_primitive());
                        if (anchor.is_none()) return;
                        node->SetLocalFrame(anchor->matrix().cast<double>() * local_base);
                    };
                    update(context.scene->Runtime().Frame().elapsed);
                    context.scene->RegisterTransformUpdater(
                        Box<dyn<FnMut<void(f64)>>>::make(rstd::move(update)));
                } else {
                    apply_bind_offset();
                }
            }
        }
        for (auto& before_node : ref.ordered_before_nodes) {
            parent_node->AppendChild(before_node.clone());
        }
        parent_node->AppendChild((*ref.node).clone());
        attached++;

        // Attach this layer's fanout clones (audio bars) right after it, so
        // all bars sit at the template's z-position in the parent child list.
        if (auto clones = context.layer_clones.get_mut(id); clones.is_some()) {
            for (auto& clone : **clones) {
                parent_node->AppendChild(rstd::move(clone));
                attached++;
            }
        }
    }
    rstd_info("attach: {}/{} nodes ({} missing parents)",
              attached,
              context.node_id_map.len(),
              missing_parent);

    // If any object during the visit installed a script binding, hand the
    // ScriptScene off to the Scene now. The renderer ticks it once per
    // frame via owe::script::TickSceneScripts. Empty ScriptScenes are
    // skipped so image-only pkgs don't pay any runtime cost.
    if (context.script_scene.is_some() && ! (*context.script_scene)->empty()) {
        // Hand the scene root to the JS runtime so `thisScene.getLayer(name)`
        // can resolve against the live graph. The renderer also ticks the
        // ScriptScene once per frame via owe::script::TickSceneScripts.
        auto& runtime = (*context.script_scene)->runtime();
        for (auto id : context.node_id_order) {
            auto node   = context.node_id_map.get(id);
            auto config = context.initial_layer_configs.get(id);
            if (node.is_none() || (**node).node.is_none() || config.is_none()) continue;
            runtime.RegisterInitialLayerConfig((*(**node).node).as_ptr(), (**config).clone());
        }
        runtime.SetScene(context.scene.get());
        runtime.SetLayerFactory(script::JsRuntime::LayerFactory::make(
            [&context](SceneNode* owner, ref<str> asset) -> Option<Arc<SceneNode>> {
                auto node = InstantiateRegisteredAsset(context, owner, asset);
                if (node.is_none())
                    rstd_error("registered layer asset '{}' is unsupported or unavailable", asset);
                return node;
            }));
        runtime.SetLayerConfigFactory(script::JsRuntime::LayerConfigFactory::make(
            [&context](SceneNode* owner, Json config) -> Option<Arc<SceneNode>> {
                auto node = InstantiateLayerConfiguration(context, owner, config);
                if (node.is_none()) rstd_error("layer configuration is unsupported or unavailable");
                return node;
            }));
        runtime.SetSceneRoot(context.scene->RootMut().as_raw_ptr());
        runtime.ClearLayerFactory();
        runtime.ClearLayerConfigFactory();
        owe::script::InstallScriptScene(*context.scene,
                                        context.script_scene.take().unwrap_unchecked());
    }
    if (context.particle_runtime.is_some()) {
        context.scene->Runtime().RegisterSystem(
            ParticleRuntimeSystem { (*context.particle_runtime).clone() },
            SceneRuntimeSchedule::BeforeRender);
    }
    context.shader_cache->ReleaseTransientEntries();
    context.scene->InstallExtension(Box<Arc<ShaderCache>>::make(context.shader_cache.clone()));
    FinalizeUniformSources(context);
    return context.scene.Take();
}

} // namespace owe
