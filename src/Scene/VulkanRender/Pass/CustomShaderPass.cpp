module;

#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

module wescene.vulkan_render;
import wescene.spec_names;
import wescene.core;
import rstd.log;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace owe::vulkan;
using namespace rstd::prelude;

CustomShaderPass::CustomShaderPass(Desc&& desc): m_desc(std::move(desc)) {}
CustomShaderPass::~CustomShaderPass() {}

namespace
{
rstd::Option<TextureRequest> TextureRequestFromScene(owe::Scene& scene, std::string_view name) {
    if (name.empty()) return rstd::None();
    if (! owe::IsSpecTex(name)) return rstd::Some(MakeImportedTextureRequest(name));
    auto it = scene.renderTargets.find(std::string(name));
    if (it == scene.renderTargets.end()) return rstd::None();
    return rstd::Some(MakeRenderTargetTextureRequest(name, it->second));
}

} // namespace

PassInvalidationFlags CustomShaderPass::finalizeResourceRequests(Scene& scene) {
    PassInvalidationFlags flags = PassInvalidationNone;
    for (auto& binding : m_desc.texture_bindings) {
        auto name = rstd::cppstd::as_string_view(binding.name.as_str());
        if (name.empty() || ! IsSpecTex(name)) continue;
        if (SetTextureRequestIfChanged(binding.request, TextureRequestFromScene(scene, name))) {
            flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
        }
    }

    if (! m_desc.output.empty() && IsSpecTex(m_desc.output)) {
        if (auto it = scene.renderTargets.find(m_desc.output); it != scene.renderTargets.end()) {
            auto& rt             = it->second;
            auto  output_request = MakeRenderTargetTextureRequest(m_desc.output, rt);
            if (SetTextureRequestIfChanged(m_desc.output_request, std::move(output_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }

            auto samples = TextureSampleCount(rt.sample_count);
            if (m_desc.samples != samples) {
                m_desc.samples = samples;
                flags |= PassInvalidationAll;
            }

            rstd::Option<TextureRequest> msaa_request;
            if (samples != VK_SAMPLE_COUNT_1_BIT) {
                auto twin_name = MsaaTwinName(m_desc.output, samples);
                msaa_request   = rstd::Some(MakeMsaaTextureRequest(twin_name, rt, samples));
            }
            if (SetTextureRequestIfChanged(m_desc.output_msaa_request, std::move(msaa_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }

            bool has_depth_attachment = false;
            if (m_desc.node.is_some() && (*m_desc.node)->Mesh() != nullptr) {
                auto&             mesh          = *(*m_desc.node)->Mesh();
                const std::size_t submesh_index = m_desc.submesh_index.to_primitive();
                if (submesh_index < mesh.Submeshes().size()) {
                    const auto& submesh = mesh.Submeshes()[submesh_index];
                    const auto& slots   = mesh.MaterialSlots();
                    if (submesh.material_slot < slots.size() && slots[submesh.material_slot]) {
                        has_depth_attachment =
                            rt.withDepth && UsesDepthAttachment(*slots[submesh.material_slot]);
                    }
                }
            }
            if (m_desc.has_depth_attachment != has_depth_attachment) {
                m_desc.has_depth_attachment = has_depth_attachment;
                flags |= PassInvalidationAll;
            }

            rstd::Option<TextureRequest> depth_request;
            if (has_depth_attachment) {
                depth_request = rstd::Some(MakeDepthTextureRequest(m_desc.output + "::depth", rt));
            }
            if (SetTextureRequestIfChanged(m_desc.depth_request, std::move(depth_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }
        }
    }
    return flags;
}

void CustomShaderPass::declareResources(ResourceDeclarationContext& context) {
    m_desc.shader_use      = rstd::None();
    m_desc.buffer_uses     = {};
    m_desc.ubo_use         = rstd::None();
    m_desc.pipeline_use    = rstd::None();
    m_desc.render_pass_use = rstd::None();
    m_desc.framebuffer_use = rstd::None();
    if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) return;

    auto&             mesh          = *(*m_desc.node)->Mesh();
    const std::size_t submesh_index = m_desc.submesh_index.to_primitive();
    if (submesh_index >= mesh.Submeshes().size()) return;
    const auto& submesh = mesh.Submeshes()[submesh_index];
    const auto& slots   = mesh.MaterialSlots();
    if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) return;
    auto& material = *slots[submesh.material_slot];
    if (! material.customShader.shader) return;

    m_desc.pipeline_use    = rstd::Some(context.ReservePipeline());
    m_desc.render_pass_use = rstd::Some(context.ReserveRenderPass());
    m_desc.framebuffer_use = rstd::Some(context.ReserveFramebuffer());
    auto shader_request    = MakeSceneShaderRequest(*material.customShader.shader);
    auto artifact_request  = shader_request.clone();
    m_desc.shader_use =
        rstd::Some(context.AddShader(rstd::move(shader_request), *material.customShader.shader));

    auto artifact = context.ShaderArtifact(artifact_request);
    if (artifact.is_some() && ! (**artifact).uniform_blocks.is_empty()) {
        auto size = (**artifact).uniform_blocks[usize()].size;
        if (size != usize()) {
            auto name = m_desc.draw_item.Valid()
                            ? BuildDrawBufferResourceName(m_desc.draw_item, DrawBufferRole::Uniform)
                            : rstd::format("pass:{}:{}:uniform",
                                           m_desc.graph_pass_index,
                                           m_desc.submesh_index);
            m_desc.ubo_use = rstd::Some(context.AddBuffer(resource::BufferRequest {
                .name = rstd::move(name),
                .definition =
                    resource::BufferDefinition {
                        .size      = size,
                        .usage     = resource::BufferUsage::Uniform,
                        .alignment = usize(4),
                    },
                .lifetime = resource::BufferLifetimeClass::Dynamic,
            }));
        }
    }

    for (std::size_t index = 0; index < submesh.vertex_arrays.size(); ++index) {
        const auto& vertex = submesh.vertex_arrays[index];
        auto        name =
            m_desc.draw_item.Valid()
                ? BuildDrawBufferResourceName(m_desc.draw_item,
                                              DrawBufferRole::Vertex,
                                              u32(static_cast<rstd::uint32_t>(index)))
                : rstd::format(
                      "pass:{}:{}:vertex:{}", m_desc.graph_pass_index, m_desc.submesh_index, index);
        auto use = context.AddBuffer(
            resource::BufferRequest {
                .name = rstd::move(name),
                .definition =
                    resource::BufferDefinition {
                        .size      = vertex.CapacitySizeOf(),
                        .usage     = resource::BufferUsage::Vertex,
                        .alignment = usize(4),
                    },
                .lifetime        = mesh.Dynamic() ? resource::BufferLifetimeClass::Dynamic
                                                  : resource::BufferLifetimeClass::Retained,
                .content_version = vertex.DataGeneration(),
            },
            rstd::slice<u8>::from_raw_parts(reinterpret_cast<const u8*>(vertex.Data()),
                                            vertex.CapacitySizeOf()));
        m_desc.buffer_uses.push(rstd::move(use));
    }
    if (submesh.index_arrays.empty()) return;

    const auto& index = submesh.index_arrays[0];
    auto        name =
        m_desc.draw_item.Valid()
            ? BuildDrawBufferResourceName(m_desc.draw_item, DrawBufferRole::Index)
            : rstd::format("pass:{}:{}:index", m_desc.graph_pass_index, m_desc.submesh_index);
    auto use = context.AddBuffer(
        resource::BufferRequest {
            .name = rstd::move(name),
            .definition =
                resource::BufferDefinition {
                    .size      = index.CapacitySizeof(),
                    .usage     = resource::BufferUsage::Index,
                    .alignment = usize(4),
                },
            .lifetime        = mesh.Dynamic() ? resource::BufferLifetimeClass::Dynamic
                                              : resource::BufferLifetimeClass::Retained,
            .content_version = index.DataGeneration(),
        },
        rstd::slice<u8>::from_raw_parts(reinterpret_cast<const u8*>(index.Data()),
                                        index.CapacitySizeof()));
    m_desc.buffer_uses.push(rstd::move(use));
}

PassResourceUses CustomShaderPass::resourceUses() const {
    PassResourceUses uses;
    for (const auto& binding : m_desc.texture_bindings) {
        if (binding.use.is_some()) {
            uses.textures.push(resource::TextureUseHandle(*binding.use));
        }
    }
    if (m_desc.output_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.output_use));
    }
    if (m_desc.output_msaa_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.output_msaa_use));
    }
    if (m_desc.depth_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.depth_use));
    }
    for (const auto& use : m_desc.buffer_uses) {
        uses.buffers.push(resource::BufferUseHandle(use));
    }
    if (m_desc.ubo_use.is_some()) {
        uses.buffers.push(resource::BufferUseHandle(*m_desc.ubo_use));
    }
    if (m_desc.shader_use.is_some()) {
        uses.shaders.push(resource::ShaderUseHandle(*m_desc.shader_use));
    }
    if (m_desc.pipeline_use.is_some()) {
        uses.pipelines.push(resource::PipelineUseHandle(*m_desc.pipeline_use));
    }
    if (m_desc.render_pass_use.is_some()) {
        uses.render_passes.push(resource::RenderPassUseHandle(*m_desc.render_pass_use));
    }
    if (m_desc.framebuffer_use.is_some()) {
        uses.framebuffers.push(resource::FramebufferUseHandle(*m_desc.framebuffer_use));
    }
    if (m_desc.descriptor_binding.is_some()) {
        uses.descriptors.push(resource::DescriptorBindingHandle(*m_desc.descriptor_binding));
    }
    return uses;
}

Option<owe::RenderItemId> CustomShaderPass::renderItemId() const {
    if (! m_desc.render_item.Valid()) return None();
    return Some<owe::RenderItemId>(m_desc.render_item);
}

std::optional<PipelineCacheKey> CustomShaderPass::pipelineCacheKey() const {
    return m_desc.pipeline_cache_key;
}

bool CustomShaderPass::pipelineCacheHit() const { return m_desc.pipeline_cache_hit; }

u64 CustomShaderPass::pipelineCacheObservedCount() const {
    return m_desc.pipeline_cache_observed_count;
}

std::optional<RenderPassCacheKey> CustomShaderPass::renderPassCacheKey() const {
    return m_desc.render_pass_cache_key;
}

bool CustomShaderPass::renderPassCacheHit() const { return m_desc.render_pass_cache_hit; }

u64 CustomShaderPass::renderPassCacheObservedCount() const {
    return m_desc.render_pass_cache_observed_count;
}

std::optional<FramebufferCacheKey> CustomShaderPass::framebufferCacheKey() const {
    return m_desc.framebuffer_cache_key;
}

bool CustomShaderPass::framebufferCacheHit() const { return m_desc.framebuffer_cache_hit; }

u64 CustomShaderPass::framebufferCacheObservedCount() const {
    return m_desc.framebuffer_cache_observed_count;
}

std::vector<PassTextureRequestDiagnostic> CustomShaderPass::textureRequestDiagnostics() const {
    std::vector<PassTextureRequestDiagnostic> out;
    out.reserve(m_desc.texture_bindings.size() + 3);
    for (std::size_t i = 0; i < m_desc.texture_bindings.size(); ++i) {
        const auto& binding = m_desc.texture_bindings[i];
        if (binding.name.is_empty() && binding.request.is_none()) continue;
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "sampled",
            .slot    = u32(static_cast<rstd::uint32_t>(i)),
            .name    = rstd::cppstd::to_string(binding.name.as_str()),
            .request = binding.request.is_some() ? rstd::Some(binding.request->clone())
                                                 : rstd::None<TextureRequest>(),
        });
    }
    if (! m_desc.output.empty() || m_desc.output_request.is_some()) {
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "output",
            .name    = m_desc.output,
            .request = m_desc.output_request.is_some() ? rstd::Some(m_desc.output_request->clone())
                                                       : rstd::None<TextureRequest>(),
        });
    }
    if (m_desc.output_msaa_request.is_some()) {
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "output-msaa",
            .name    = rstd::cppstd::to_string(m_desc.output_msaa_request->name.as_str()),
            .request = rstd::Some(m_desc.output_msaa_request->clone()),
        });
    }
    if (m_desc.depth_request.is_some()) {
        out.push_back(PassTextureRequestDiagnostic {
            .role    = "depth",
            .name    = rstd::cppstd::to_string(m_desc.depth_request->name.as_str()),
            .request = rstd::Some(m_desc.depth_request->clone()),
        });
    }
    return out;
}

MaterialTextureBindingRefresh
CustomShaderPass::refreshMaterialTextureBindings(const RenderSceneSnapshot& render_scene) {
    MaterialTextureBindingRefresh result;
    if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) return result;

    auto&             mesh          = *(*m_desc.node)->Mesh();
    const std::size_t submesh_index = m_desc.submesh_index.to_primitive();
    if (submesh_index >= mesh.Submeshes().size()) return result;
    const auto& submesh = mesh.Submeshes()[submesh_index];
    const auto& slots   = mesh.MaterialSlots();
    if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) return result;

    const auto& textures = slots[submesh.material_slot]->textures;
    if (textures.size() != m_desc.texture_bindings.size()) {
        result.requires_graph_rebuild = true;
        return result;
    }

    for (std::size_t i = 0; i < textures.size(); ++i) {
        const auto& next     = textures[i];
        const auto& old      = m_desc.texture_bindings[i];
        auto        old_name = rstd::cppstd::as_string_view(old.name.as_str());
        if (! CanRefreshSceneMaterialTextureBinding(old_name, next, m_desc.output)) {
            result.requires_graph_rebuild = true;
            return result;
        }
    }

    for (std::size_t i = 0; i < textures.size(); ++i) {
        const auto& next     = textures[i];
        auto&       old      = m_desc.texture_bindings[i];
        auto        next_dep = ClassifySceneMaterialTexture(next);
        if (old.name == rstd::cppstd::as_str(next) &&
            ! IsLocalSceneMaterialTextureDependency(next_dep))
            continue;

        TextureBindingRequest binding;
        if (! next.empty()) {
            binding.name = rstd::string::String::make(rstd::cppstd::as_str(next));
            binding.request =
                rstd::Some(MakeImportedTextureRequest(next, render_scene.textureDescId(next)));
        }

        if (! SameTextureBindingRequest(old, binding)) {
            old = std::move(binding);
            result.invalidation_flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
        }
    }

    return result;
}

auto CustomShaderPass::createUniformBufferUpdate(ref<dyn<UniformBindingPrepareContext>> prepare,
                                                 const PreparedPassResources&           resources)
    -> Result<Option<Box<dyn<UniformBufferUpdate>>>, UniformBufferUpdateError> {
    if (m_desc.ubo_use.is_none() || m_desc.shader_use.is_none()) {
        return Ok(Option<Box<dyn<UniformBufferUpdate>>>());
    }
    auto prepared = resources.Resolve(*m_desc.shader_use);
    if (prepared.is_none()) {
        return Err(UniformBufferUpdateError {
            .message = String::make("prepared uniform shader is unavailable"),
        });
    }
    const auto& artifact = (**prepared).shader.physical->artifact;
    if (artifact.uniform_blocks.is_empty()) {
        return Ok(Option<Box<dyn<UniformBufferUpdate>>>());
    }
    auto draw_item = m_desc.draw_item;
    if (prepare->ResolveDraw(draw_item).is_none() && m_desc.node.is_some()) {
        auto node    = ref<SceneNode>::from_raw_parts((*m_desc.node).as_ptr());
        auto current = prepare->DrawItemFor(node, m_desc.submesh_index);
        if (current.is_some()) draw_item = *current;
    }
    auto draw = prepare->ResolveDraw(draw_item);
    if (draw.is_none()) {
        return Err(UniformBufferUpdateError {
            .message = String::make("uniform texture metadata draw is unavailable"),
        });
    }
    auto textures =
        Vec<PreparedUniformTextureMetadata>::with_capacity(usize(m_desc.texture_bindings.size()));
    for (std::size_t index = 0; index < m_desc.texture_bindings.size(); ++index) {
        PreparedUniformTextureMetadata metadata;
        const auto&                    binding = m_desc.texture_bindings[index];
        if (binding.use.is_some()) {
            auto prepared = resources.Resolve(*binding.use);
            if (prepared.is_none()) {
                return Err(UniformBufferUpdateError {
                    .message = rstd::format("prepared texture metadata {} is unavailable",
                                            binding.name.as_str()),
                });
            }
            const auto image       = (**prepared).image.getActive();
            metadata.available     = true;
            metadata.source_extent = { static_cast<float>(image.extent.width),
                                       static_cast<float>(image.extent.height) };
            metadata.sample_extent = metadata.source_extent;
            metadata.has_mipmap    = (**prepared).request.kind == TextureRequestKind::RenderTarget;
            metadata.mipmap_level  = static_cast<float>(image.mipmap_level);
            metadata.revision      = (**prepared).physical_generation ^ image.generation;
            if (metadata.revision == u64()) metadata.revision = u64(1);
        }
        if (index < draw->material->texture_metadata.size()) {
            const auto& authored = draw->material->texture_metadata[index];
            if (authored.has_extent) {
                metadata.available     = true;
                metadata.source_extent = authored.source_extent;
                metadata.sample_extent = authored.sample_extent;
            }
        }
        textures.push(rstd::move(metadata));
    }
    auto binding = MakeUniformBufferBinding(prepare,
                                            draw_item,
                                            *m_desc.ubo_use,
                                            artifact.uniform_blocks[usize()],
                                            rstd::move(textures));
    if (binding.is_err()) return Err(rstd::move(binding).unwrap_err_unchecked());
    return Ok(Some(rstd::move(binding).unwrap_unchecked()));
}

bool CustomShaderPass::prepareResourceStates(
    rstd::mut_ref<rstd::dyn<resource_registry::TextureStatePreparer>> states) {
    m_desc.sampled_barriers.Clear();
    for (const auto& binding : m_desc.texture_bindings) {
        if (binding.use.is_none()) continue;
        auto barrier = states->Prepare(*binding.use, resource_registry::TextureStateKind::Sampled);
        if (barrier.is_none()) return false;
        m_desc.sampled_barriers.Add(rstd::move(barrier).unwrap_unchecked());
    }
    if (m_desc.output_use.is_some() &&
        ! states->Set(*m_desc.output_use, resource_registry::TextureStateKind::Sampled)) {
        return false;
    }
    if (m_desc.output_msaa_use.is_some() &&
        ! states->Set(*m_desc.output_msaa_use,
                      resource_registry::TextureStateKind::ColorAttachment)) {
        return false;
    }
    return m_desc.depth_use.is_none() ||
           states->Set(*m_desc.depth_use, resource_registry::TextureStateKind::DepthAttachment);
}

void CustomShaderPass::prepare(Scene& scene, const Device& device, PassPrepareContext& context) {
    std::vector<ImageSlotsRef> vk_textures(m_desc.texture_bindings.size());
    ImageParameters            vk_output;
    ImageParameters            vk_output_msaa;
    ImageParameters            vk_depth;
    for (std::size_t i = 0; i < m_desc.texture_bindings.size(); i++) {
        auto& binding = m_desc.texture_bindings[i];
        if (binding.empty()) continue;

        if (binding.use.is_none()) {
            rstd_error("sampled texture {} has no resource use", binding.name);
            return;
        }
        auto prepared = context.resources->Resolve(*binding.use);
        if (prepared.is_none()) {
            rstd_error("prepared sampled texture {} not found", binding.name);
            return;
        }
        vk_textures[i] = (**prepared).image;
    }
    bool                         out_force_clear { false };
    rstd::Option<TextureRequest> output_attachment_request;
    rstd::Option<TextureRequest> msaa_attachment_request;
    rstd::Option<TextureRequest> depth_attachment_request;
    {
        auto& tex_name = m_desc.output;
        rstd_assert(IsSpecTex(tex_name));
        rstd_assert(scene.renderTargets.count(tex_name) > 0);
        auto& rt        = scene.renderTargets.at(tex_name);
        out_force_clear = rt.force_clear;
        if (m_desc.output_use.is_none()) return;
        auto prepared = context.resources->Resolve(*m_desc.output_use);
        if (prepared.is_none()) {
            rstd_error("prepared output texture {} not found", tex_name);
            return;
        }
        vk_output                 = (**prepared).image.getActive();
        m_desc.output_extent      = { vk_output.extent.width, vk_output.extent.height };
        output_attachment_request = rstd::Some((**prepared).request.clone());
        m_desc.samples            = TextureSampleCount(rt.sample_count);
        if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
            if (m_desc.output_msaa_use.is_none()) return;
            auto msaa = context.resources->Resolve(*m_desc.output_msaa_use);
            if (msaa.is_none()) {
                rstd_error("prepared MSAA texture {} not found", tex_name);
                return;
            }
            vk_output_msaa          = (**msaa).image.getActive();
            msaa_attachment_request = rstd::Some((**msaa).request.clone());
        }
    }

    if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) return;
    SceneMesh&        mesh          = *(*m_desc.node)->Mesh();
    const std::size_t submesh_index = m_desc.submesh_index.to_primitive();
    if (mesh.Submeshes().empty() || submesh_index >= mesh.Submeshes().size()) return;
    const auto& submesh = mesh.Submeshes()[submesh_index];
    const auto& slots   = mesh.MaterialSlots();
    if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) return;
    SceneMaterial& material_ref         = *slots[submesh.material_slot];
    auto&          output_rt            = scene.renderTargets.at(m_desc.output);
    const bool     has_depth_attachment = output_rt.withDepth && UsesDepthAttachment(material_ref);
    m_desc.has_depth_attachment         = has_depth_attachment;
    VkAttachmentLoadOp depthLoadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    if (has_depth_attachment) {
        depthLoadOp = m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        m_desc.depth_load_op = depthLoadOp;

        if (m_desc.depth_use.is_none()) return;
        auto depth = context.resources->Resolve(*m_desc.depth_use);
        if (depth.is_none()) {
            rstd_error("prepared depth texture {} not found", m_desc.output);
            return;
        }
        vk_depth                 = (**depth).image.getActive();
        depth_attachment_request = rstd::Some((**depth).request.clone());
    }

    std::vector<Uni_ShaderSpv> spvs;
    DescriptorSetInfo          descriptor_info;
    ShaderReflected            shader_reflection;
    const ShaderReflected*     ref { nullptr };
    {
        SceneShader& shader = *(material_ref.customShader.shader);

        if (m_desc.shader_use.is_none()) {
            rstd_error("shader artifact provider unavailable, {}", shader.name);
            return;
        }
        auto prepared_shader = context.resources->Resolve(*m_desc.shader_use);
        if (prepared_shader.is_none()) {
            rstd_error("prepared shader artifact unavailable, {}", shader.name);
            return;
        }
        const auto& artifact = (**prepared_shader).shader.physical->artifact;
        shader_reflection    = ShaderReflectionFromArtifact(artifact);
        spvs                 = ShaderSpvsFromArtifact(artifact);
        if (spvs.empty()) {
            rstd_error("prepared shader artifact is empty, {}", shader.name);
            return;
        }
        ref = &shader_reflection;

        auto& bindings = descriptor_info.bindings;
        bindings.resize(ref->binding_map.size());

        /*
        rstd_info("----shader------");
        rstd_info("{}", shader.name);
        rstd_info("--inputs:");
        for (auto& i : ref->input_location_map) {
            rstd_info("{} {}", i.second, i.first);
        }
        rstd_info("--bindings:");
        */

        std::transform(
            ref->binding_map.begin(), ref->binding_map.end(), bindings.begin(), [](auto& item) {
                // rstd_info("{} {}", item.second.binding, item.first);
                return item.second;
            });

        m_desc.vk_tex_binding.clear();
        m_desc.vk_tex_binding.reserve(vk_textures.size());

        for (std::size_t i = 0; i < vk_textures.size(); i++) {
            rstd::int32_t binding { -1 };
            const auto    member = shader.SamplerMember(i);
            if (! member.empty()) {
                auto reflected = ref->binding_map.find(std::string(member));
                if (reflected != ref->binding_map.end()) {
                    binding = static_cast<rstd::int32_t>(reflected->second.binding);
                }
            }
            m_desc.vk_tex_binding.push_back(binding);
        }
    }

    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        RenderBufferResolver buffer_resolver(*context.resources);
        DrawBufferRequest    buffer_request { .render_item   = m_desc.render_item,
                                              .mesh          = &mesh,
                                              .submesh_index = m_desc.submesh_index,
                                              .buffer_uses   = m_desc.buffer_uses.as_slice() };
        auto                 draw_buffers = buffer_resolver.prepareDrawBuffers(buffer_request);
        if (! draw_buffers) return;
        m_desc.draw_buffers = std::move(*draw_buffers);

        for (unsigned i = 0; i < submesh.vertex_arrays.size(); i++) {
            const auto& vertex    = submesh.vertex_arrays[i];
            auto        attrs_map = vertex.GetAttrOffsetMap();

            VkVertexInputBindingDescription bind_desc {
                .binding   = i,
                .stride    = static_cast<rstd::uint32_t>(vertex.OneSizeOf().to_primitive()),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };
            bind_descriptions.push_back(bind_desc);

            for (auto& item : ref->input_location_map) {
                auto& name   = item.first;
                auto& input  = item.second;
                usize offset = exists(attrs_map, name) ? attrs_map[name].offset : usize();

                VkVertexInputAttributeDescription attr_desc {
                    .location = input.location,
                    .binding  = i,
                    .format   = input.format,
                    .offset   = static_cast<rstd::uint32_t>(offset.to_primitive()),
                };
                attr_descriptions.push_back(attr_desc);
            }
        }
    }
    {
        VkPipelineColorBlendAttachmentState color_blend {};
        VkAttachmentLoadOp                  loadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        {
            VkColorComponentFlags colorMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            bool writes_alpha = ! ((*m_desc.node)->Camera().empty() ||
                                   sstart_with((*m_desc.node)->Camera(), "global"));

            if (writes_alpha) colorMask |= VK_COLOR_COMPONENT_A_BIT;
            color_blend.colorWriteMask = colorMask;

            auto blendmode = material_ref.blenmode;
            SetBlend(blendmode, color_blend);
            SetAlphaBlendWritePolicy(color_blend, writes_alpha);
            m_desc.blending = color_blend.blendEnable;

            SetAttachmentLoadOp(blendmode, loadOp);
            if (m_desc.preserve_output) loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            if (m_desc.clear_output) loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            if (out_force_clear) loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        }
        m_desc.color_load_op                       = loadOp;
        constexpr VkFormat      color_format       = VK_FORMAT_R8G8B8A8_UNORM;
        constexpr VkImageLayout color_final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        descriptor_info.push_descriptor = device.capabilities().push_descriptor;
        GraphicsPipeline pipeline_state;
        pipeline_state.toDefault();
        pipeline_state.setSampleCount(m_desc.samples);
        if (has_depth_attachment) SetDepthState(material_ref, pipeline_state.depth);
        SetCullMode(material_ref.cull_mode, pipeline_state.raster);
        const bool          has_index = m_desc.draw_buffers.hasIndex();
        VkPrimitiveTopology topology  = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        switch (mesh.Primitive()) {
        case MeshPrimitive::POINT: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case MeshPrimitive::TRIANGLE:
            topology = has_index ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        }
        PipelineResourceRequest pipeline_request {
            .descriptor_sets      = { descriptor_info },
            .vertex_bindings      = std::move(bind_descriptions),
            .vertex_attrs         = std::move(attr_descriptions),
            .shader_stages        = std::move(spvs),
            .color_blend          = color_blend,
            .depth                = pipeline_state.depth,
            .raster               = pipeline_state.raster,
            .multisample          = pipeline_state.multisample,
            .topology             = topology,
            .color_format         = color_format,
            .color_final_layout   = color_final_layout,
            .color_load_op        = loadOp,
            .depth_load_op        = depthLoadOp,
            .has_depth_attachment = has_depth_attachment,
        };
        m_desc.pipeline_cache_key.reset();
        m_desc.render_pass_cache_key.reset();
        m_desc.pipeline_cache_hit               = false;
        m_desc.pipeline_cache_observed_count    = u64();
        m_desc.render_pass_cache_hit            = false;
        m_desc.render_pass_cache_observed_count = u64();
        if (m_desc.pipeline_use.is_none() || m_desc.render_pass_use.is_none()) return;
        auto prepared = context.graphics->PreparePipeline(
            *m_desc.pipeline_use, *m_desc.render_pass_use, device, std::move(pipeline_request));
        if (prepared.is_err()) {
            auto error = rstd::move(prepared).unwrap_err_unchecked();
            rstd_error("prepare pipeline failed: {}", error.message);
            return;
        }
        auto pipeline                           = rstd::move(prepared).unwrap_unchecked();
        m_desc.pipeline_cache_key               = rstd::move(pipeline.cache_key);
        m_desc.render_pass_cache_key            = rstd::move(pipeline.render_pass_key);
        m_desc.pipeline_cache_hit               = pipeline.cache_hit;
        m_desc.pipeline_cache_observed_count    = pipeline.cache_observed_count;
        m_desc.render_pass_cache_hit            = pipeline.render_pass_cache_hit;
        m_desc.render_pass_cache_observed_count = pipeline.render_pass_cache_observed_count;
    }

    {
        const bool has_msaa = m_desc.samples != VK_SAMPLE_COUNT_1_BIT;
        if (output_attachment_request.is_none()) return;
        if (has_msaa && msaa_attachment_request.is_none()) return;
        if (has_depth_attachment && depth_attachment_request.is_none()) return;

        std::vector<FramebufferAttachmentDesc> attachments;
        attachments.reserve((has_msaa ? 2u : 1u) + (has_depth_attachment ? 1u : 0u));
        if (has_msaa) {
            attachments.push_back(
                MakeFramebufferAttachment(*msaa_attachment_request, vk_output_msaa));
        }
        attachments.push_back(MakeFramebufferAttachment(*output_attachment_request, vk_output));
        if (has_depth_attachment) {
            attachments.push_back(MakeFramebufferAttachment(*depth_attachment_request, vk_depth));
        }

        m_desc.framebuffer_cache_key.reset();
        m_desc.framebuffer_cache_hit            = false;
        m_desc.framebuffer_cache_observed_count = u64();
        if (m_desc.framebuffer_use.is_none() || m_desc.render_pass_use.is_none()) return;
        auto prepared = context.graphics->PrepareFramebuffer(*m_desc.framebuffer_use,
                                                             *m_desc.render_pass_use,
                                                             device,
                                                             std::move(attachments),
                                                             m_desc.output_extent);
        if (prepared.is_err()) {
            auto error = rstd::move(prepared).unwrap_err_unchecked();
            rstd_error("prepare framebuffer failed: {}", error.message);
            return;
        }
        auto framebuffer                        = rstd::move(prepared).unwrap_unchecked();
        m_desc.framebuffer_cache_key            = rstd::move(framebuffer.cache_key);
        m_desc.framebuffer_cache_hit            = framebuffer.cache_hit;
        m_desc.framebuffer_cache_observed_count = framebuffer.cache_observed_count;
    }

    {
        auto images = rstd::vec::Vec<resource_registry::DescriptorImageBinding>::with_capacity(
            usize(vk_textures.size()));
        for (std::size_t index = 0; index < vk_textures.size(); ++index) {
            auto  binding = m_desc.vk_tex_binding[index];
            auto& slots   = vk_textures[index];
            if (binding < 0 || slots.slots.empty()) continue;
            images.push(resource_registry::DescriptorImageBinding {
                .binding = static_cast<rstd::uint32_t>(binding),
                .image   = slots.getActive(),
            });
        }
        rstd::Option<resource_registry::DescriptorBufferBinding> buffer = rstd::None();
        if (m_desc.ubo_use.is_some()) {
            auto prepared = context.resources->Resolve(*m_desc.ubo_use);
            if (prepared.is_none()) return;
            auto& allocation = (**prepared).buffer.physical->buffer;
            buffer           = rstd::Some(resource_registry::DescriptorBufferBinding {
                .binding = 0,
                .buffer  = allocation.buffer(),
                .offset  = allocation.offset(),
                .size    = allocation.size(),
            });
        }
        if (m_desc.pipeline_use.is_none()) return;
        auto descriptor = context.graphics->PrepareDescriptor(
            device, *m_desc.pipeline_use, images.as_slice(), buffer);
        if (descriptor.is_err()) {
            auto error = rstd::move(descriptor).unwrap_err_unchecked();
            rstd_error("prepare descriptor binding failed: {}", error.message);
            return;
        }
        m_desc.descriptor_binding = rstd::Some(rstd::move(descriptor).unwrap_unchecked());
    }

    {
        if (out_force_clear || m_desc.transparent_clear) {
            // Some offscreen RTs need a transparent reset, not the scene's
            // opaque clear color.
            m_desc.clear_value =
                VkClearValue { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } } };
            m_desc.clear_value_src = rstd::None();
        } else {
            auto& sc           = scene.clearColor;
            m_desc.clear_value = VkClearValue {
                .color = { .float32 = { sc[usize()], sc[usize(1)], sc[usize(2)], 1.0f } }
            };
            // Track the live scene.clearColor: per-frame re-sync in
            // execute() picks up live edits (e.g. `schemecolor` user
            // property changes) without a render-graph rebuild.
            m_desc.clear_value_src = rstd::Some(rstd::ref<rstd::array<float, 3>>::from_raw_parts(
                rstd::addressof(scene.clearColor)));
        }
    }
    setPrepared();
}

bool CustomShaderPass::supportsRenderScope() const { return prepared(); }

bool CustomShaderPass::canJoinRenderScopeAfter(const VulkanPass& previous) const {
    const auto* prev_pass = dynamic_cast<const CustomShaderPass*>(&previous);
    if (prev_pass == nullptr) return false;
    if (! prepared() || ! prev_pass->prepared()) return false;
    if (m_desc.clear_output || m_desc.clear_depth) return false;
    if (m_desc.color_load_op != VK_ATTACHMENT_LOAD_OP_LOAD) return false;
    if (m_desc.has_depth_attachment && m_desc.depth_load_op != VK_ATTACHMENT_LOAD_OP_LOAD)
        return false;

    const auto& prev = prev_pass->m_desc;
    if (m_desc.output != prev.output) return false;
    if (m_desc.samples != prev.samples) return false;
    if (m_desc.has_depth_attachment != prev.has_depth_attachment) return false;
    if (m_desc.output_extent.width != prev.output_extent.width ||
        m_desc.output_extent.height != prev.output_extent.height) {
        return false;
    }
    return true;
}

bool CustomShaderPass::update(PassUpdateContext& context) {
    if (m_desc.clear_value_src) {
        const auto& sc                      = **m_desc.clear_value_src;
        m_desc.clear_value.color.float32[0] = sc[usize()];
        m_desc.clear_value.color.float32[1] = sc[usize(1)];
        m_desc.clear_value.color.float32[2] = sc[usize(2)];
        m_desc.clear_value.color.float32[3] = 1.0f;
    }

    if (m_desc.draw_buffers.dynamic) {
        if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) return false;
        DrawBufferRequest request {
            .render_item   = m_desc.render_item,
            .mesh          = (*m_desc.node)->Mesh(),
            .submesh_index = m_desc.submesh_index,
            .buffer_uses   = m_desc.buffer_uses.as_slice(),
        };
        if (! RenderBufferResolver::updateDynamicDrawBuffers(
                request, m_desc.draw_buffers, context.buffers)) {
            return false;
        }
    }

    return true;
}

void CustomShaderPass::prepareRenderScopeDraw(PassRecordContext& context) {
    recordSampledImageBarriers(context);
}

void CustomShaderPass::recordSampledImageBarriers(PassRecordContext& context) {
    m_desc.sampled_barriers.Record(*context.command);
}

void CustomShaderPass::beginRenderScope(PassRecordContext& context) {
    if (m_desc.render_pass_use.is_none() || m_desc.framebuffer_use.is_none()) return;
    auto render_pass = context.resources->Resolve(*m_desc.render_pass_use);
    auto framebuffer = context.resources->Resolve(*m_desc.framebuffer_use);
    if (render_pass.is_none() || framebuffer.is_none()) return;
    auto&                cmd      = *context.command;
    auto&                outext   = m_desc.output_extent;
    const bool           has_msaa = m_desc.samples != VK_SAMPLE_COUNT_1_BIT;
    const rstd::uint32_t clear_count =
        (has_msaa ? 2u : 1u) + (m_desc.has_depth_attachment ? 1u : 0u);
    rstd::array<VkClearValue, 3> clears {};
    clears[usize()] = m_desc.clear_value;
    if (m_desc.has_depth_attachment) {
        const rstd::uint32_t depth_index        = has_msaa ? 2u : 1u;
        clears[usize(depth_index)].depthStencil = { 1.0f, 0 };
    }
    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = **(**render_pass).physical,
        .framebuffer = **(**framebuffer).physical,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { outext.width, outext.height },
            },
        .clearValueCount = clear_count,
        .pClearValues    = clears.data(),
    };
    cmd.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void CustomShaderPass::recordRenderScopeDraw(PassRecordContext& context) {
    if (m_desc.pipeline_use.is_none()) return;
    auto pipeline = context.resources->Resolve(*m_desc.pipeline_use);
    if (pipeline.is_none()) return;
    auto& cmd    = *context.command;
    auto& outext = m_desc.output_extent;
    if (m_desc.descriptor_binding.is_some()) {
        auto descriptor = context.resources->Resolve(*m_desc.descriptor_binding);
        if (descriptor.is_none()) return;
        (**descriptor).Record(cmd, *(**pipeline).physical->pipeline.layout);
    }

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *(**pipeline).physical->pipeline.handle);
    VkViewport viewport {
        .x        = 0,
        .y        = (float)outext.height,
        .width    = (float)outext.width,
        .height   = -(float)outext.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor { { 0, 0 }, { outext.width, outext.height } };

    cmd.SetViewport(0, viewport);
    cmd.SetScissor(0, scissor);

    auto& draw_buffers = m_desc.draw_buffers;
    for (usize i {}; i < draw_buffers.vertices.len(); i++) {
        auto prepared = context.resources->Resolve(draw_buffers.vertices[i]);
        if (prepared.is_none()) return;
        auto&        mref = (**prepared).buffer.physical->buffer;
        VkBuffer     vb   = mref.buffer();
        VkDeviceSize off  = mref.offset();
        cmd.BindVertexBuffers(static_cast<rstd::uint32_t>(i.to_primitive()), 1, &vb, &off);
    }
    if (draw_buffers.index.is_some()) {
        auto prepared = context.resources->Resolve(*draw_buffers.index);
        if (prepared.is_none()) return;
        auto&        mref = (**prepared).buffer.physical->buffer;
        VkBuffer     ib   = mref.buffer();
        VkDeviceSize off  = mref.offset();
        cmd.BindIndexBuffer(ib, off, VK_INDEX_TYPE_UINT32);
    }

    const bool has_index = draw_buffers.hasIndex();
    if (has_index) {
        const auto& submeshes = (*m_desc.node)->Mesh()->Submeshes();
        static const std::vector<SceneMesh::DrawRange> kEmpty;
        const std::size_t submesh_index = m_desc.submesh_index.to_primitive();
        const auto&       ranges =
            (submesh_index < submeshes.size()) ? submeshes[submesh_index].draw_ranges : kEmpty;
        if (ranges.empty()) {
            cmd.DrawIndexed(draw_buffers.draw_count.to_primitive(), 1, 0, 0, 0);
        } else {
            // Per-part drawing — preserves the file's z-order so later parts
            // overdraw earlier ones (eyelid over pupil during blink).
            for (const auto& r : ranges) {
                cmd.DrawIndexed(r.index_count, 1, r.first_index, 0, 0);
            }
        }
    } else {
        cmd.Draw(draw_buffers.draw_count.to_primitive(), 1, 0, 0);
    }
}

void CustomShaderPass::endRenderScope(PassRecordContext& context) {
    context.command->EndRenderPass();
}

void CustomShaderPass::record(PassRecordContext& context) {
    prepareRenderScopeDraw(context);
    beginRenderScope(context);
    recordRenderScopeDraw(context);
    endRenderScope(context);
}

void CustomShaderPass::destory(const Device&) {
    m_desc.descriptor_binding = rstd::None();
    m_desc.pipeline_cache_key.reset();
    m_desc.render_pass_cache_key.reset();
    m_desc.framebuffer_cache_key.reset();
    m_desc.pipeline_cache_hit               = false;
    m_desc.pipeline_cache_observed_count    = u64();
    m_desc.render_pass_cache_hit            = false;
    m_desc.render_pass_cache_observed_count = u64();
    m_desc.framebuffer_cache_hit            = false;
    m_desc.framebuffer_cache_observed_count = u64();
    m_desc.draw_buffers                     = {};
}

bool CustomShaderPass::setTextureBinding(u32 index, TextureBindingRequest binding) {
    const std::size_t native_index = index.to_primitive();
    if (native_index >= m_desc.texture_bindings.size()) return false;
    m_desc.texture_bindings[native_index] = std::move(binding);
    return true;
}
