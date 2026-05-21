module;

#include <rstd/macro.hpp>
#include "RenderGraph/Pass.hpp"

module wescene.vulkan_render;
import wescene.spec_texs;
import wescene.core;
import rstd.log;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

import wescene.rgraph;

using namespace owe;
namespace owe::rg
{

void doCopy(RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc, TexNode* in, TexNode* out) {
    builder.read(in);
    builder.write(out);

    desc.src = in->key();
    desc.dst = out->key();
}
void addCopyPass(RenderGraph& rgraph, TexNode* in, TexNode* out) {
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&in, &out](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc) {
            doCopy(builder, desc, in, out);
        });
}

void addCopyPass(RenderGraph& rgraph, const TexNode::Desc& in, const TexNode::Desc& out) {
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&in, &out](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc) {
            auto* in_node  = builder.createTexNode(in);
            auto* out_node = builder.createTexNode(out, true);
            doCopy(builder, desc, in_node, out_node);
        });
}

TexNode* addCopyPass(RenderGraph& rgraph, TexNode* in, TexNode::Desc* out_desc = nullptr) {
    TexNode* copy { nullptr };
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&copy, in, out_desc](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& pdesc) {
            auto desc = out_desc == nullptr ? in->genDesc() : *out_desc;
            if (out_desc == nullptr) {
                desc.key += "_" + std::to_string(in->version()) + "_copy";
                desc.name += "_" + std::to_string(in->version()) + "_copy";
            }
            copy = builder.createTexNode(desc, true);
            doCopy(builder, pdesc, in, copy);
        });
    return copy;
}

static TexNode::Desc createTexDesc(std::string path) {
    return TexNode::Desc { .name = path,
                           .key  = path,
                           .type = IsSpecTex(path) ? TexNode::TexType::Temp
                                                   : TexNode::TexType::Imported };
}
} // namespace owe::rg

static void TraverseNode(const std::function<void(SceneNode*)>& func, SceneNode* node) {
    func(node);
    for (auto& child : node->GetChildren()) TraverseNode(func, child.get());
}

static void CheckAndSetSprite(Scene& scene, vulkan::CustomShaderPass::Desc& desc,
                              std::span<const std::string> texs) {
    for (usize i = 0; i < texs.size(); i++) {
        auto& tex = texs[i];
        if (! tex.empty() && ! IsSpecTex(tex) && scene.textures.count(tex) != 0) {
            const auto& stex = scene.textures.at(tex);
            if (stex.isSprite) {
                desc.sprites_map[i] = stex.spriteAnim;
            }
        }
    }
}

struct DelayLinkInfo {
    rg::NodeID id;
    rg::NodeID link_id;
    i32        tex_index;
};

struct ExtraInfo {
    Map<size_t, rg::TexNode*>  id_link_map {};
    std::vector<DelayLinkInfo> link_info {};
    rg::RenderGraph*           rgraph { nullptr };
    Scene*                     scene { nullptr };
    bool                       use_mipmap_framebuffer { false };
};

static void ToGraphPass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    auto loadEffect = [node, &rgraph, &scene, &extra](SceneImageEffectLayer* effs) {
        effs->ResolveEffect(scene.default_effect_mesh, "effect");

        for (usize i = 0; i < effs->EffectCount(); i++) {
            auto& eff     = effs->GetEffect(i);
            auto  cmdItor = eff->commands.begin();
            auto  cmdEnd  = eff->commands.end();
            int   nodePos = 0;
            for (auto& n : eff->nodes) {
                if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos) {
                    rg::addCopyPass(
                        rgraph, rg::createTexDesc(cmdItor->src), rg::createTexDesc(cmdItor->dst));
                    cmdItor++;
                }
                auto& name = n.output;
                ToGraphPass(n.sceneNode.get(), name, node->ID(), extra);
                nodePos++;
            }
        }
    };

    if (node->Mesh() == nullptr) return;
    auto* mesh = node->Mesh();
    if (mesh->Submeshes().empty()) return;
    const auto& slots = mesh->MaterialSlots();

    SceneImageEffectLayer* imgeff = nullptr;
    if (! node->Camera().empty()) {
        auto& cam = scene.cameras.at(node->Camera());
        if (cam->HasImgEffect()) {
            imgeff = cam->GetImgEffect().get();
            output = imgeff->FirstTarget();
        }
    }

    for (uint32_t smi = 0; smi < mesh->Submeshes().size(); smi++) {
        const auto& submesh = mesh->Submeshes()[smi];
        if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) continue;
        SceneMaterial* material = slots[submesh.material_slot].get();
        std::string    passName = material->name;

        rgraph.addPass<vulkan::CustomShaderPass>(
            passName,
            rg::PassNode::Type::CustomShader,
            [material, node, smi, &output, &imgId, &rgraph, &scene, &extra](
                rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
                const auto& pass     = builder.workPassNode();
                pdesc.node           = node;
                pdesc.submesh_index  = smi;
                pdesc.output         = output;
                CheckAndSetSprite(scene, pdesc, material->textures);
                for (usize i = 0; i < material->textures.size(); i++) {
                    const auto&  url = material->textures[i];
                    rg::TexNode* input { nullptr };
                    if (url.empty()) {
                        pdesc.textures.emplace_back("");
                        continue;
                    } else if (IsSpecLinkTex(url)) {
                        auto id = ParseLinkTex(url);
                        extra.link_info.push_back(
                            DelayLinkInfo { .id = pass.ID(), .link_id = id, .tex_index = (i32)i });
                        pdesc.textures.emplace_back("");
                        continue;
                    } else {
                        rg::TexNode::Desc desc;
                        desc.key  = url;
                        desc.name = url;
                        desc.type = ! IsSpecTex(url) ? rg::TexNode::TexType::Imported
                                                     : rg::TexNode::TexType::Temp;
                        input     = builder.createTexNode(desc);
                        if (IsSpecTex(url)) builder.markVirtualWrite(input);
                        if (sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER))
                            extra.use_mipmap_framebuffer = true;
                    }

                    if (url == output) {
                        builder.markSelfWrite(input);
                        input = rg::addCopyPass(rgraph, input);
                    }
                    builder.read(input);
                    pdesc.textures.emplace_back(input->key());
                }

                rg::TexNode* output_node { nullptr };
                output_node =
                    builder.createTexNode(rg::TexNode::Desc { .name = output.data(),
                                                              .key  = output.data(),
                                                              .type = rg::TexNode::TexType::Temp },
                                          true);
                builder.write(output_node);
                if (output == SpecTex_Default) {
                    extra.id_link_map[(usize)imgId] = output_node;
                } else if (IsSpecLinkTex(output)) {
                    extra.id_link_map[(usize)ParseLinkTex(output)] = output_node;
                }
            });
    }

    // load effect
    if (imgeff != nullptr) loadEffect(imgeff);
}

// Walk the SceneNode subtree (plus its imgeff's effect nodes) and collect every
// WE layer id referenced as `_rt_link_<id>` by any material's texture slot.
static void CollectLinkedIds(SceneNode* node, Scene& scene, Set<i32>& out) {
    if (node == nullptr) return;
    auto inspect_material = [&](const SceneMaterial& mat) {
        for (auto& t : mat.textures) {
            if (IsSpecLinkTex(t)) out.insert((i32)ParseLinkTex(t));
        }
    };
    if (node->HasMaterial()) inspect_material(*node->Mesh()->Material());
    if (! node->Camera().empty()) {
        auto it = scene.cameras.find(node->Camera());
        if (it != scene.cameras.end() && it->second->HasImgEffect()) {
            auto& eff_layer = it->second->GetImgEffect();
            for (usize i = 0; i < eff_layer->EffectCount(); i++) {
                auto& eff = eff_layer->GetEffect(i);
                for (auto& n : eff->nodes) {
                    if (n.sceneNode && n.sceneNode->HasMaterial())
                        inspect_material(*n.sceneNode->Mesh()->Material());
                }
            }
        }
    }
    for (auto& c : node->GetChildren()) CollectLinkedIds(c.get(), scene, out);
}

std::unique_ptr<rg::RenderGraph> owe::sceneToRenderGraph(Scene& scene) {
    std::unique_ptr<rg::RenderGraph> rgraph = std::make_unique<rg::RenderGraph>();
    ExtraInfo                        extra { .rgraph = rgraph.get(), .scene = &scene };

    // Pass A: walk the scene tree (and post-process step nodes) once, collecting
    // every WE layer id that any material binds via `_rt_link_<id>`. This is the
    // delay-resolve step replacing a JSON pre-scan.
    Set<i32> linked_ids;
    CollectLinkedIds(scene.sceneGraph.get(), scene, linked_ids);
    for (auto& pp : scene.post_processes) {
        for (auto& step : pp->steps) {
            if (auto* sp = std::get_if<ScenePostProcessPass>(&step)) {
                CollectLinkedIds(sp->node.get(), scene, linked_ids);
            }
        }
    }

    // Pass B: emit passes. For parse-time-invisible layers, drop the ones with
    // no link consumer; route the remaining ones into a private `_rt_link_<id>`
    // RT instead of `_rt_default`.
    TraverseNode(
        [&extra, &scene, &linked_ids](SceneNode* node) {
            const i32 nid = node->ID();
            const bool init_invisible = scene.initial_invisible_ids.count(nid) != 0;
            if (init_invisible) {
                if (linked_ids.count(nid) == 0) return;
                std::string link_key = GenLinkTex((idx)nid);
                if (! node->Camera().empty()) {
                    auto cit = scene.cameras.find(node->Camera());
                    if (cit != scene.cameras.end() && cit->second->HasImgEffect()) {
                        cit->second->GetImgEffect()->SetFinalTarget(link_key);
                    }
                }
                if (scene.renderTargets.count(link_key) == 0) {
                    auto sz = node->Size();
                    scene.renderTargets[link_key] = {
                        .width      = sz.x() > 0 ? (i32)sz.x() : scene.ortho[0],
                        .height     = sz.y() > 0 ? (i32)sz.y() : scene.ortho[1],
                        .allowReuse = false,
                    };
                }
                ToGraphPass(node, link_key, nid, extra);
            } else {
                ToGraphPass(node, SpecTex_Default, nid, extra);
            }
        },
        scene.sceneGraph.get());

    // Emit global post-process passes after the main scene-graph traversal.
    // Each step is either a CustomShaderPass (built on the synthetic node's
    // mesh+material) or a CopyPass (RT-to-RT blit).
    for (auto& pp : scene.post_processes) {
        for (auto& step : pp->steps) {
            if (auto* sp = std::get_if<ScenePostProcessPass>(&step)) {
                std::string_view target = sp->output.empty() ? SpecTex_Default
                                                              : std::string_view(sp->output);
                ToGraphPass(sp->node.get(), target, sp->node->ID(), extra);
            } else if (auto* cp = std::get_if<ScenePostProcessCopy>(&step)) {
                rg::addCopyPass(*rgraph,
                                rg::createTexDesc(cp->src),
                                rg::createTexDesc(cp->dst));
            }
        }
    }

    for (auto& info : extra.link_info) {
        if (! exists(extra.id_link_map, info.link_id)) {
            rstd_error("link tex {} not found", info.link_id);
            continue;
        }
        rgraph->afterBuild(
            info.id, [&rgraph, &extra, &info](rg::RenderGraphBuilder& builder, rg::Pass& rgpass) {
                auto& pass = static_cast<vulkan::CustomShaderPass&>(rgpass);

                auto* link_tex_node = extra.id_link_map.at(info.link_id);
                auto  copy_desc     = link_tex_node->genDesc();
                copy_desc.key       = GenLinkTex((idx)info.link_id);
                copy_desc.name      = copy_desc.key;

                auto new_in = rg::addCopyPass(*rgraph, link_tex_node, &copy_desc);
                builder.read(new_in);
                pass.setDescTex((u32)info.tex_index, new_in->key());
                return true;
            });
    }

    if (extra.use_mipmap_framebuffer) {
        rg::addCopyPass(*rgraph,
                        rg::TexNode::Desc { .name = SpecTex_Default.data(),
                                            .key  = SpecTex_Default.data(),
                                            .type = rg::TexNode::TexType::Temp },
                        rg::TexNode::Desc { .name = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                            .key  = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                            .type = rg::TexNode::TexType::Temp });
    }

    return rgraph;
}
