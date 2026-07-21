module;

#include <rstd/macro.hpp>

module wescene.vulkan_render;
import wescene.spec_names;
import wescene.core;
import rstd.log;
import rstd.cppstd;
import eigen;
import wescene.vulkan;
import wescene.scene;

import wescene.rgraph;

using namespace owe;
using namespace rstd::prelude;

namespace
{
auto StdString(const String& value) -> std::string {
    return rstd::cppstd::to_string(value.as_str());
}

auto CloneTextureDesc(const rg::TextureDesc& desc) -> rg::TextureDesc {
    return rg::TextureDesc {
        .name = desc.name.clone(),
        .key  = desc.key.clone(),
        .kind = desc.kind,
        .request =
            desc.request.is_some() ? Some(desc.request->clone()) : None<resource::TextureRequest>(),
    };
}
} // namespace

namespace owe::rg
{

void doCopy(RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc, TextureNodeRef in,
            TextureNodeRef out) {
    builder.read(in);
    builder.write(out);

    auto in_state  = builder.textureState(in);
    auto out_state = builder.textureState(out);
    rstd_assert(in_state.is_some() && out_state.is_some());
    if (in_state.is_none() || out_state.is_none()) return;
    desc.src     = StdString(in_state->desc.key);
    desc.dst     = StdString(out_state->desc.key);
    desc.src_use = Some(in_state->use);
    desc.dst_use = Some(out_state->use);
}
} // namespace owe::rg

struct ExtraInfo;

struct LinkTextureConsumer {
    rg::NodeHandle   node;
    rg::PassHandle   pass;
    WallpaperLayerId source_layer;
    u32              texture_index;
};

static rg::TextureDesc MakeTextureDescBase(std::string_view key) {
    return rg::TextureDesc {
        .name = String::make(rstd::cppstd::as_str(key)),
        .key  = String::make(rstd::cppstd::as_str(key)),
        .kind = IsSpecTex(key) ? rg::TextureKind::Temp : rg::TextureKind::Imported,
    };
}

struct GraphTextureOutput {
    rg::TextureNodeRef            ref;
    vulkan::TextureBindingRequest binding;
    rg::TextureDesc               desc;
};

class GraphLinkFinalizer {
public:
    void setLinkedLayerIds(const Set<i32>* linked_ids) { m_linked_ids = linked_ids; }
    void recordSource(WallpaperLayerId source_layer, GraphTextureOutput output) {
        if (m_linked_ids == nullptr || m_linked_ids->count(source_layer.value) != 0) {
            m_source_outputs[source_layer.value] = std::move(output);
        }
    }
    void addConsumer(const rg::PassNode& pass, WallpaperLayerId source_layer, u32 texture_index) {
        m_consumers.push_back(LinkTextureConsumer {
            .node          = pass.handle,
            .pass          = pass.pass,
            .source_layer  = source_layer,
            .texture_index = texture_index,
        });
    }
    void apply(ExtraInfo& extra);

private:
    const Set<i32>*                  m_linked_ids { nullptr };
    Map<i32, GraphTextureOutput>     m_source_outputs;
    std::vector<LinkTextureConsumer> m_consumers;
};

struct ExtraInfo {
    rg::RenderGraph*           rgraph { nullptr };
    Scene*                     scene { nullptr };
    Set<std::string>           depth_initialized_outputs {};
    Option<rg::TextureNodeRef> mip_framebuffer_history;
    const RenderSceneSnapshot* render_scene { nullptr };
    GraphLinkFinalizer         link_finalizer;
};

static Option<vulkan::TextureRequest> BuildGraphTextureRequest(ExtraInfo&       extra,
                                                               std::string_view key) {
    if (key.empty()) return None();
    if (! IsSpecTex(key)) {
        Option<RenderTextureDescId> texture;
        if (extra.render_scene != nullptr) texture = extra.render_scene->textureDescId(key);
        return Some(vulkan::MakeImportedTextureRequest(key, texture));
    }

    if (extra.render_scene != nullptr) {
        if (auto desc_id = extra.render_scene->renderTargetDescId(key)) {
            if (auto* desc = extra.render_scene->renderTargetDesc(*desc_id)) {
                return Some(vulkan::MakeRenderTargetTextureRequest(key, desc->desc));
            }
        }
    }

    if (extra.scene != nullptr) {
        auto it = extra.scene->renderTargets.find(std::string(key));
        if (it != extra.scene->renderTargets.end()) {
            return Some(vulkan::MakeRenderTargetTextureRequest(key, it->second));
        }
    }

    return None();
}

static rg::TextureDesc MakeTextureDesc(ExtraInfo& extra, std::string_view key) {
    auto desc    = MakeTextureDescBase(key);
    desc.request = BuildGraphTextureRequest(extra, key);
    return desc;
}

static void FillCopyTextureRequests(ExtraInfo& extra, vulkan::CopyPass::Desc& desc) {
    desc.src_request = BuildGraphTextureRequest(extra, desc.src);
    desc.dst_request = BuildGraphTextureRequest(extra, desc.dst);
}

static GraphTextureOutput CaptureTextureOutput(ExtraInfo& extra, rg::TextureNodeRef ref) {
    auto state = extra.rgraph->textureState(ref);
    rstd_assert(state.is_some());
    if (state.is_none()) return {};
    auto key = StdString(state->desc.key);
    return GraphTextureOutput {
        .ref = ref,
        .binding =
            vulkan::TextureBindingRequest {
                .name    = String::make(rstd::cppstd::as_str(key)),
                .use     = Some(state->use),
                .request = BuildGraphTextureRequest(extra, key),
            },
        .desc = rstd::move(state->desc),
    };
}

static void AddCopyPass(ExtraInfo& extra, rg::TextureDesc in, rg::TextureDesc out) {
    extra.rgraph->addPass<vulkan::CopyPass>(
        "copy",
        rg::PassNode::Type::Copy,
        [in = std::move(in), out = std::move(out), &extra](rg::RenderGraphBuilder& builder,
                                                           vulkan::CopyPass::Desc& desc) {
            auto in_node  = builder.createTexture(in);
            auto out_node = builder.createTexture(out, true);
            rg::doCopy(builder, desc, in_node, out_node);
            FillCopyTextureRequests(extra, desc);
        });
}

static rg::TextureNodeRef AddCopyPass(ExtraInfo& extra, rg::TextureNodeRef in,
                                      Option<rg::TextureDesc> out_desc = None()) {
    rg::TextureNodeRef copy {};
    extra.rgraph->addPass<vulkan::CopyPass>(
        "copy",
        rg::PassNode::Type::Copy,
        [&copy, in, out_desc = std::move(out_desc), &extra](rg::RenderGraphBuilder& builder,
                                                            vulkan::CopyPass::Desc& pdesc) {
            auto state = builder.textureState(in);
            rstd_assert(state.is_some());
            if (state.is_none()) return;
            auto desc =
                out_desc.is_some() ? CloneTextureDesc(*out_desc) : CloneTextureDesc(state->desc);
            if (out_desc.is_none()) {
                auto suffix = rstd::format("_{}_copy", state->version);
                desc.key.push_str(suffix.as_str());
                desc.name.push_str(suffix.as_str());
            }
            copy = builder.createTexture(desc, true);
            rg::doCopy(builder, pdesc, in, copy);
            FillCopyTextureRequests(extra, pdesc);
        });
    return copy;
}

void GraphLinkFinalizer::apply(ExtraInfo& extra) {
    for (auto& consumer : m_consumers) {
        auto output_it = m_source_outputs.find(consumer.source_layer.value);
        if (output_it == m_source_outputs.end()) {
            rstd_error("link tex {} not found", consumer.source_layer.value);
            continue;
        }

        auto rgpass = extra.rgraph->getPass(consumer.pass);
        if (rgpass.is_none()) {
            rstd_error("link tex {} pass not found", consumer.source_layer.value);
            continue;
        }
        auto& pass = static_cast<vulkan::VulkanPass&>(*rgpass);

        const auto&        stored = output_it->second;
        GraphTextureOutput input {
            .ref     = stored.ref,
            .binding = stored.binding.clone(),
            .desc    = CloneTextureDesc(stored.desc),
        };
        auto link_key =
            GenLinkTex(static_cast<std::ptrdiff_t>(consumer.source_layer.value.to_primitive()));
        if (input.binding.name != rstd::cppstd::as_str(link_key)) {
            auto copy_desc        = CloneTextureDesc(input.desc);
            copy_desc.key         = String::make(rstd::cppstd::as_str(link_key));
            copy_desc.name        = copy_desc.key.clone();
            input.desc            = CloneTextureDesc(copy_desc);
            input.ref             = AddCopyPass(extra, input.ref, Some(rstd::move(copy_desc)));
            input.binding.name    = String::make(rstd::cppstd::as_str(link_key));
            input.binding.request = BuildGraphTextureRequest(extra, link_key);
            auto copied_state     = extra.rgraph->textureState(input.ref);
            if (copied_state.is_some()) input.binding.use = Some(copied_state->use);
        }

        if (! extra.rgraph->readTexture(consumer.node, input.ref)) {
            rstd_error("link tex {} read failed", consumer.source_layer.value);
            continue;
        }
        if (! pass.setTextureBinding(consumer.texture_index, std::move(input.binding))) {
            rstd_error("link tex {} binding failed", consumer.source_layer.value);
        }
    }
}

static rg::TextureNodeRef AddMipFramebufferHistory(ExtraInfo&              extra,
                                                   rg::RenderGraphBuilder& builder) {
    if (extra.mip_framebuffer_history.is_some()) {
        return *extra.mip_framebuffer_history;
    }

    auto history_desc = MakeTextureDesc(extra, WE_MIP_MAPPED_FRAME_BUFFER);
    history_desc.kind = rg::TextureKind::Temp;
    auto history      = builder.createTexture(history_desc);
    builder.markVirtualWrite(history);
    extra.mip_framebuffer_history = Some<rg::TextureNodeRef>(history);
    return history;
}

static void StoreMipFramebufferHistory(ExtraInfo& extra) {
    if (extra.mip_framebuffer_history.is_none()) return;

    auto history_desc = MakeTextureDesc(extra, WE_MIP_MAPPED_FRAME_BUFFER);
    history_desc.kind = rg::TextureKind::Temp;
    AddCopyPass(extra, MakeTextureDesc(extra, SpecTex_Default), rstd::move(history_desc));
}

static SceneImageEffectLayer* ToGraphPass(SceneNode* node, std::string_view output,
                                          Option<WallpaperLayerId> source_layer, ExtraInfo& extra,
                                          bool defer_effect = false);

static void LoadGraphEffects(SceneImageEffectLayer* effs, Option<WallpaperLayerId> source_layer,
                             ExtraInfo& extra) {
    auto& scene = *extra.scene;

    effs->ResolveEffect(scene.default_effect_mesh, "effect");

    for (auto* eff : effs->ResolvedEffects()) {
        if (eff == nullptr) continue;
        auto cmdItor = eff->commands.begin();
        auto cmdEnd  = eff->commands.end();
        int  nodePos = 0;
        for (auto& n : eff->nodes) {
            if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos.to_primitive()) {
                AddCopyPass(extra,
                            MakeTextureDesc(extra, cmdItor->src),
                            MakeTextureDesc(extra, cmdItor->dst));
                cmdItor++;
            }
            auto& name = n.output;
            ToGraphPass(n.sceneNode.as_ptr(), name, source_layer, extra);
            nodePos++;
        }
    }
}

static SceneImageEffectLayer* ToGraphPass(SceneNode* node, std::string_view output,
                                          Option<WallpaperLayerId> source_layer, ExtraInfo& extra,
                                          bool defer_effect) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    if (node->Mesh() == nullptr) return nullptr;
    auto* mesh = node->Mesh();
    if (mesh->Submeshes().empty()) return nullptr;
    const auto& slots = mesh->MaterialSlots();

    SceneImageEffectLayer* imgeff = nullptr;
    if (! node->Camera().empty()) {
        auto& cam = scene.cameras.at(node->Camera());
        if (cam->HasImgEffect()) {
            auto* effect = cam->GetImgEffect().get();
            if (effect->RequiresIntermediateTarget()) {
                imgeff = effect;
                output = imgeff->FirstTarget();
            }
        }
    }
    if (imgeff != nullptr) {
        for (auto& prefill : imgeff->PrefillNodes()) {
            std::string_view prefill_output =
                prefill.output.empty() ? output : std::string_view(prefill.output);
            ToGraphPass(prefill.sceneNode.as_ptr(), prefill_output, source_layer, extra);
        }
    }

    for (std::size_t smi = 0; smi < mesh->Submeshes().size(); smi++) {
        const auto& submesh       = mesh->Submeshes()[smi];
        const auto  material_slot = static_cast<std::size_t>(submesh.material_slot);
        if (material_slot >= slots.size() || ! slots[material_slot]) continue;
        SceneMaterial* material = slots[material_slot].get();
        std::string    passName = material->name;
        // Per-submesh output override (clipping-mask submeshes write into a
        // shared RT that the main puppet pass samples via g_Texture8).
        std::string_view pass_output =
            submesh.output_override.empty() ? output : std::string_view(submesh.output_override);

        rgraph.addPass<vulkan::CustomShaderPass>(
            rstd::cppstd::as_str(passName),
            rg::PassNode::Type::CustomShader,
            [material, node, smi, pass_output, source_layer, &scene, &extra](
                rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
                const auto& pass       = builder.workPassNode();
                pdesc.node             = Some(rstd::mut_ref<SceneNode>::from_raw_parts(node));
                pdesc.submesh_index    = u32(static_cast<rstd::uint32_t>(smi));
                pdesc.graph_pass_index = pass.pass.index;
                if (auto node_id = scene.ResourceIndex().nodeId(*node)) {
                    if (auto draw_item = scene.ResourceIndex().drawItemFor(
                            *node_id, u32(static_cast<rstd::uint32_t>(smi)))) {
                        pdesc.draw_item = *draw_item;
                        if (extra.render_scene != nullptr) {
                            if (auto render_item = extra.render_scene->renderItemFor(*draw_item)) {
                                pdesc.render_item = *render_item;
                            }
                        }
                    }
                }
                pdesc.output = std::string(pass_output);
                for (std::size_t i = 0; i < material->textures.size(); i++) {
                    const auto&                url = material->textures[i];
                    Option<rg::TextureNodeRef> input;
                    if (url.empty()) {
                        pdesc.texture_bindings.emplace_back();
                        continue;
                    } else if (IsSpecLinkTex(url)) {
                        auto id = ParseLinkTex(url);
                        extra.link_finalizer.addConsumer(
                            pass,
                            WallpaperLayerId { .value = rstd::as_cast<i32>(id) },
                            u32(static_cast<rstd::uint32_t>(i)));
                        pdesc.texture_bindings.emplace_back();
                        continue;
                    } else {
                        auto desc = MakeTextureDesc(extra, url);
                        if (sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER)) {
                            input = Some(AddMipFramebufferHistory(extra, builder));
                        } else {
                            input = Some(builder.createTexture(desc));
                        }
                        if (IsSpecTex(url) && ! sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER)) {
                            builder.markVirtualWrite(*input);
                        }
                    }

                    if (url == pass_output) {
                        builder.markSelfWrite(*input);
                        input = Some(AddCopyPass(extra, *input));
                    }
                    builder.read(*input);
                    auto sampled_state = builder.textureState(*input);
                    rstd_assert(sampled_state.is_some());
                    if (sampled_state.is_none()) {
                        pdesc.texture_bindings.emplace_back();
                        continue;
                    }
                    auto sampled_key = StdString(sampled_state->desc.key);
                    pdesc.texture_bindings.emplace_back(vulkan::TextureBindingRequest {
                        .name    = String::make(rstd::cppstd::as_str(sampled_key)),
                        .use     = Some(sampled_state->use),
                        .request = BuildGraphTextureRequest(extra, sampled_key),
                    });
                }

                std::string pass_output_s(pass_output);
                auto        output_node =
                    builder.createTexture(MakeTextureDesc(extra, pass_output_s), true);
                auto output_state = builder.textureState(output_node);
                rstd_assert(output_state.is_some());
                if (output_state.is_none()) return;
                const auto& output_rt          = scene.renderTargets.at(pass_output_s);
                const bool  first_output_write = output_state->version == usize();
                pdesc.output_use               = Some(output_state->use);
                pdesc.output_request           = BuildGraphTextureRequest(extra, pass_output_s);
                pdesc.samples                  = vulkan::TextureSampleCount(output_rt.sample_count);
                if (pdesc.samples != VK_SAMPLE_COUNT_1_BIT) {
                    auto twin_name = vulkan::MsaaTwinName(pass_output_s, pdesc.samples);
                    pdesc.output_msaa_request =
                        Some(vulkan::MakeMsaaTextureRequest(twin_name, output_rt, pdesc.samples));
                    auto msaa_node = builder.createTexture(
                        rg::TextureDesc {
                            .name    = String::make(rstd::cppstd::as_str(twin_name)),
                            .key     = String::make(rstd::cppstd::as_str(twin_name)),
                            .kind    = rg::TextureKind::Temp,
                            .request = Some(pdesc.output_msaa_request->clone()),
                        },
                        true);
                    auto msaa_state = builder.textureState(msaa_node);
                    if (msaa_state) pdesc.output_msaa_use = Some(msaa_state->use);
                }
                pdesc.transparent_clear = first_output_write && output_rt.clear_on_first_write;
                pdesc.clear_output =
                    (first_output_write && output_rt.bind.screen) || pdesc.transparent_clear;
                pdesc.preserve_output =
                    output_state->version > usize() && output_rt.preserve_on_write;
                const bool uses_depth =
                    output_rt.withDepth && vulkan::UsesDepthAttachment(*material);
                pdesc.has_depth_attachment = uses_depth;
                if (uses_depth) {
                    auto depth_name = pass_output_s + "::depth";
                    pdesc.depth_request =
                        Some(vulkan::MakeDepthTextureRequest(depth_name, output_rt));
                    auto depth_node = builder.createTexture(
                        rg::TextureDesc {
                            .name    = String::make(rstd::cppstd::as_str(depth_name)),
                            .key     = String::make(rstd::cppstd::as_str(depth_name)),
                            .kind    = rg::TextureKind::Temp,
                            .request = Some(pdesc.depth_request->clone()),
                        },
                        true);
                    auto depth_state = builder.textureState(depth_node);
                    if (depth_state) pdesc.depth_use = Some(depth_state->use);
                }
                pdesc.clear_depth =
                    uses_depth && (pdesc.clear_output || output_rt.force_clear ||
                                   extra.depth_initialized_outputs.count(pass_output_s) == 0);
                if (uses_depth) {
                    extra.depth_initialized_outputs.insert(pass_output_s);
                } else if (pdesc.clear_output || output_rt.force_clear) {
                    extra.depth_initialized_outputs.erase(pass_output_s);
                }
                builder.write(output_node);
                if (source_layer.is_some()) {
                    extra.link_finalizer.recordSource(*source_layer,
                                                      CaptureTextureOutput(extra, output_node));
                }
                if (IsSpecLinkTex(pass_output)) {
                    extra.link_finalizer.recordSource(
                        WallpaperLayerId { .value = rstd::as_cast<i32>(ParseLinkTex(pass_output)) },
                        CaptureTextureOutput(extra, output_node));
                }
            });
    }

    if (! defer_effect && imgeff != nullptr && imgeff->HasRenderEffects())
        LoadGraphEffects(imgeff, source_layer, extra);
    return imgeff;
}

// Bottom-up collect: identify SceneNode subtrees whose every node can be
// elided without losing a link source. Visibility-hidden ancestors also hide
// anonymous/generated descendants such as particle children, so the skip set
// is keyed by node pointer instead of WE layer id.
static bool CollectEmitSkipSubtrees(SceneNode* node, Scene& scene, const Set<i32>& linked_ids,
                                    Set<const SceneNode*>& out_skip,
                                    bool                   visibility_hidden_ancestor = false) {
    const i32  nid         = node->ID();
    const auto link_source = scene.ResolveLayerLinkSource(*node);
    const i32  layer_id    = link_source.is_some() ? link_source->value : nid;
    const bool linked      = link_source.is_some() && linked_ids.count(link_source->value) != 0;
    const bool visibility_hidden_self =
        layer_id >= i32() && scene.visibility_elidable_layer_ids.count(layer_id) != 0 && ! linked;
    const bool visibility_hidden = visibility_hidden_ancestor || visibility_hidden_self;

    bool all_children_skippable = true;
    for (auto& c : node->GetChildren()) {
        if (! CollectEmitSkipSubtrees(c.as_ptr(), scene, linked_ids, out_skip, visibility_hidden))
            all_children_skippable = false;
    }
    const bool self_skippable =
        ! linked &&
        (visibility_hidden || (layer_id >= i32() && scene.elidable_layer_ids.count(layer_id) != 0));
    if (self_skippable && all_children_skippable) {
        out_skip.insert(node);
        return true;
    }
    return false;
}

static bool ShouldSkipNoRuntimeEffect(SceneNode* node, Scene& scene) {
    if (node == nullptr || node->Camera().empty()) return false;
    auto camera_it = scene.cameras.find(node->Camera());
    if (camera_it == scene.cameras.end() || ! camera_it->second->HasImgEffect()) return false;
    const auto& effect_layer = camera_it->second->GetImgEffect();
    return effect_layer && effect_layer->SkipWhenNoRuntimeEffect() &&
           effect_layer->EffectCount() > usize() && ! effect_layer->HasRuntimeVisibleEffect();
}

static void ConfigureNestedOutput(SceneNode* node, std::string_view output,
                                  std::string_view inherited_camera, Scene& scene) {
    if (! inherited_camera.empty() && node->Camera().empty()) {
        node->SetCamera(std::string(inherited_camera));
    }
    if (node->Camera().empty()) return;

    auto camera_it = scene.cameras.find(node->Camera());
    if (camera_it == scene.cameras.end() || ! camera_it->second->HasImgEffect()) return;
    auto& effect_layer = camera_it->second->GetImgEffect();
    if (! effect_layer) return;
    if (output != SpecTex_Default && effect_layer->FinalTarget() == SpecTex_Default) {
        effect_layer->SetFinalTarget(std::string(output));
    }
    if (effect_layer->FinalTarget() == output) {
        effect_layer->SetFinalCamera(std::string(inherited_camera));
    }
}

static void EmitSceneNode(SceneNode* node, std::string_view inherited_output,
                          std::string_view inherited_camera, ExtraInfo& extra,
                          const Set<const SceneNode*>& emit_skip_subtrees,
                          const Set<i32>&              linked_ids) {
    if (node == nullptr || emit_skip_subtrees.count(node) != 0) return;

    auto&            scene       = *extra.scene;
    const i32        nid         = node->ID();
    const auto       link_source = scene.ResolveLayerLinkSource(*node);
    const i32        layer_id    = link_source.is_some() ? link_source->value : nid;
    const bool       elidable    = scene.elidable_layer_ids.count(layer_id) != 0;
    const bool       linked = link_source.is_some() && linked_ids.count(link_source->value) != 0;
    bool             emit   = true;
    std::string      link_output;
    std::string_view node_output = inherited_output;

    if (! linked && ShouldSkipNoRuntimeEffect(node, scene)) emit = false;
    if (elidable) {
        if (! linked) {
            emit = false;
        } else {
            auto* source_record = extra.render_scene->linkSource(*link_source);
            if (source_record == nullptr) {
                rstd_error("link render target for layer {} not found in snapshot", layer_id);
                emit = false;
            } else {
                link_output = source_record->render_target_key;
                node_output = link_output;
                if (! node->Camera().empty()) {
                    auto camera_it = scene.cameras.find(node->Camera());
                    if (camera_it != scene.cameras.end() && camera_it->second->HasImgEffect()) {
                        camera_it->second->GetImgEffect()->SetFinalTarget(link_output);
                        camera_it->second->GetImgEffect()->SetFinalLocal(true);
                    }
                }
            }
        }
    }

    auto group_camera = scene.RenderGroupCamera(WallpaperLayerId { .value = nid });
    if (emit && group_camera) {
        ConfigureNestedOutput(node, node_output, inherited_camera, scene);
        auto* effect_layer = ToGraphPass(node, node_output, link_source, extra, true);
        if (effect_layer == nullptr) {
            rstd_error("render group layer {} has no effect target", nid);
        }
        const std::string_view child_output =
            effect_layer == nullptr ? node_output : std::string_view(effect_layer->FirstTarget());
        for (auto& child : node->GetChildren()) {
            EmitSceneNode(
                child.as_ptr(), child_output, *group_camera, extra, emit_skip_subtrees, linked_ids);
        }
        if (effect_layer != nullptr && effect_layer->HasRenderEffects()) {
            LoadGraphEffects(effect_layer, link_source, extra);
        }
        return;
    }

    if (emit) {
        ConfigureNestedOutput(node, node_output, inherited_camera, scene);
        ToGraphPass(node, node_output, link_source, extra);
    }
    for (auto& child : node->GetChildren()) {
        EmitSceneNode(child.as_ptr(),
                      inherited_output,
                      inherited_camera,
                      extra,
                      emit_skip_subtrees,
                      linked_ids);
    }
}

Box<rg::RenderGraph> owe::sceneToRenderGraph(Scene&                     scene,
                                             const RenderSceneSnapshot& render_scene) {
    auto      rgraph = Box<rg::RenderGraph>::make();
    ExtraInfo extra { .rgraph = rgraph.get(), .scene = &scene, .render_scene = &render_scene };

    // The snapshot owns link-consumer discovery; graph build only consumes the
    // resulting source ids.
    const auto& linked_ids = render_scene.LinkedLayerIds();
    extra.link_finalizer.setLinkedLayerIds(&linked_ids);

    // Skip subtrees the parser tagged as elidable (user-hidden, or no-effect
    // identity passthrough layers) when nothing in the subtree links anything.
    // Most corpora have ~25x more elidable layers than link-referenced ones;
    // the skip set lets the emit walk short-circuit without mutating the tree.
    Set<const SceneNode*> emit_skip_subtrees;
    CollectEmitSkipSubtrees(scene.sceneGraph.as_ptr(), scene, linked_ids, emit_skip_subtrees);

    EmitSceneNode(
        scene.sceneGraph.as_ptr(), SpecTex_Default, {}, extra, emit_skip_subtrees, linked_ids);

    // Emit global post-process passes after the main scene-graph traversal.
    // Each step is either a CustomShaderPass (built on the synthetic node's
    // mesh+material) or a CopyPass (RT-to-RT blit).
    for (auto& pp : scene.post_processes) {
        for (auto& step : pp->steps) {
            if (auto* sp = std::get_if<ScenePostProcessPass>(&step)) {
                std::string_view target =
                    sp->output.empty() ? SpecTex_Default : std::string_view(sp->output);
                ToGraphPass(
                    sp->node.as_ptr(), target, scene.ResolveLayerLinkSource(*sp->node), extra);
            } else if (auto* cp = std::get_if<ScenePostProcessCopy>(&step)) {
                AddCopyPass(
                    extra, MakeTextureDesc(extra, cp->src), MakeTextureDesc(extra, cp->dst));
            }
        }
    }

    extra.link_finalizer.apply(extra);
    StoreMipFramebufferHistory(extra);

    scene.RebuildResourceIndex();
    return rgraph;
}

Box<rg::RenderGraph> owe::sceneToRenderGraph(Scene& scene) {
    auto render_scene = ExtractRenderSceneSnapshot(scene);
    return sceneToRenderGraph(scene, render_scene);
}
