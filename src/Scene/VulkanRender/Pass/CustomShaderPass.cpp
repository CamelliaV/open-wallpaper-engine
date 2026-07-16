module;

#include <rstd/macro.hpp>
#include "Utils/AutoDeletor.hpp"
#include "vvk/macros.hpp"

module wescene.vulkan_render;
import wescene.spec_names;
import wescene.core;
import rstd.log;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace owe::vulkan;

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
                auto& mesh = *(*m_desc.node)->Mesh();
                if (m_desc.submesh_index < mesh.Submeshes().size()) {
                    const auto& submesh = mesh.Submeshes()[m_desc.submesh_index];
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

std::optional<owe::RenderItemId> CustomShaderPass::renderItemId() const {
    return m_desc.render_item;
}

std::optional<PipelineCacheKey> CustomShaderPass::pipelineCacheKey() const {
    return m_desc.pipeline_cache_key;
}

bool CustomShaderPass::pipelineCacheHit() const { return m_desc.pipeline_cache_hit; }

uint64_t CustomShaderPass::pipelineCacheObservedCount() const {
    return m_desc.pipeline_cache_observed_count;
}

std::optional<RenderPassCacheKey> CustomShaderPass::renderPassCacheKey() const {
    return m_desc.render_pass_cache_key;
}

bool CustomShaderPass::renderPassCacheHit() const { return m_desc.render_pass_cache_hit; }

uint64_t CustomShaderPass::renderPassCacheObservedCount() const {
    return m_desc.render_pass_cache_observed_count;
}

std::optional<FramebufferCacheKey> CustomShaderPass::framebufferCacheKey() const {
    return m_desc.framebuffer_cache_key;
}

bool CustomShaderPass::framebufferCacheHit() const { return m_desc.framebuffer_cache_hit; }

uint64_t CustomShaderPass::framebufferCacheObservedCount() const {
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
            .slot    = static_cast<uint32_t>(i),
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

    auto& mesh = *(*m_desc.node)->Mesh();
    if (m_desc.submesh_index >= mesh.Submeshes().size()) return result;
    const auto& submesh = mesh.Submeshes()[m_desc.submesh_index];
    const auto& slots   = mesh.MaterialSlots();
    if (submesh.material_slot >= slots.size() || ! slots[submesh.material_slot]) return result;

    const auto& textures = slots[submesh.material_slot]->textures;
    if (textures.size() != m_desc.texture_bindings.size()) {
        result.requires_graph_rebuild = true;
        return result;
    }

    for (usize i = 0; i < textures.size(); ++i) {
        const auto& next     = textures[i];
        const auto& old      = m_desc.texture_bindings[i];
        auto        old_name = rstd::cppstd::as_string_view(old.name.as_str());
        if (! CanRefreshSceneMaterialTextureBinding(old_name, next, m_desc.output)) {
            result.requires_graph_rebuild = true;
            return result;
        }
    }

    for (usize i = 0; i < textures.size(); ++i) {
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

        if (next.empty()) {
            m_desc.sprites_map.erase(i);
            continue;
        }

        auto  texture_id = render_scene.textureDescId(next);
        auto* texture    = texture_id.has_value() ? render_scene.textureDesc(*texture_id) : nullptr;
        if (texture != nullptr && texture->desc.isSprite) {
            m_desc.sprites_map[i] = texture->desc.spriteAnim;
        } else {
            m_desc.sprites_map.erase(i);
        }
    }

    return result;
}

static std::span<uint8_t> MakeUniformUploadBytes(const owe::ShaderValue& value, size_t refl_size,
                                                 std::vector<owe::ShaderValue::value_type>& resized,
                                                 bool& compatible) {
    compatible                    = true;
    const size_t       value_size = value.size() * sizeof(owe::ShaderValue::value_type);
    std::span<uint8_t> value_u8 {
        const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())),
        value_size,
    };

    if (refl_size != value_size && refl_size % sizeof(owe::ShaderValue::value_type) == 0) {
        const size_t refl_count = refl_size / sizeof(owe::ShaderValue::value_type);
        resized.assign(refl_count, 0.0f);
        std::copy_n(value.data(), std::min(value.size(), refl_count), resized.begin());
        value_u8 = { reinterpret_cast<uint8_t*>(resized.data()), refl_size };
    } else if (refl_size != value_size) {
        compatible = false;
        value_u8   = value_u8.first(std::min(refl_size, value_u8.size()));
    }
    return value_u8;
}

static void UpdateUniform(StagingBuffer& buf, const StagingBufferRef& bufref,
                          const ShaderReflected::Block& block, std::string_view name,
                          const owe::ShaderValue& value) {
    using namespace owe;
    auto uni = block.member_map.find(name);
    if (uni == block.member_map.end()) {
        return;
    }

    const size_t                         offset    = uni->second.offset;
    const size_t                         refl_size = uni->second.size;
    bool                                 compatible {};
    std::vector<ShaderValue::value_type> resized;
    auto value_u8 = MakeUniformUploadBytes(value, refl_size, resized, compatible);
    if (! compatible) {
        rstd_warn("uniform \"{}\" size mismatch: reflected {} bytes, uploader {} bytes",
                  name,
                  refl_size,
                  value.size() * sizeof(ShaderValue::value_type));
    }
    buf.writeToBuf(bufref, value_u8, offset);
}

// Sanity-check the reflected cbuffer: members in `block.member_map` must not
// overlap. An overlap means glslang packed two members at conflicting std140
// offsets — uploader writes will clobber neighbouring slots (the failure mode
// the EmitCBufferStd140 helper was added to prevent).
static void CheckBlockOverlap(const ShaderReflected::Block& block, std::string_view shader_name) {
    struct Span {
        std::size_t      off;
        std::size_t      end;
        std::string_view name;
    };
    std::vector<Span> spans;
    spans.reserve(block.member_map.size());
    for (const auto& [n, u] : block.member_map) {
        if (u.size == 0) continue;
        spans.push_back({ u.offset, u.offset + u.size, n });
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
        return a.off < b.off;
    });
    for (std::size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].off < spans[i - 1].end) {
            rstd_warn("cbuffer overlap in \"{}\": \"{}\"[{}..{}) overlaps \"{}\"[{}..{})",
                      shader_name,
                      spans[i - 1].name,
                      spans[i - 1].off,
                      spans[i - 1].end,
                      spans[i].name,
                      spans[i].off,
                      spans[i].end);
        }
    }
}

bool CustomShaderPass::prepareResourceStates(resource_registry::ResourceStateTracker& states) {
    m_desc.sampled_barriers.Clear();
    for (const auto& binding : m_desc.texture_bindings) {
        if (binding.use.is_none()) continue;
        auto barrier = states.Prepare(*binding.use, resource_registry::TextureStateKind::Sampled);
        if (barrier.is_none()) return false;
        m_desc.sampled_barriers.Add(rstd::move(barrier).unwrap_unchecked());
    }
    if (m_desc.output_use.is_some() &&
        ! states.Set(*m_desc.output_use, resource_registry::TextureStateKind::Sampled)) {
        return false;
    }
    if (m_desc.output_msaa_use.is_some() &&
        ! states.Set(*m_desc.output_msaa_use,
                     resource_registry::TextureStateKind::ColorAttachment)) {
        return false;
    }
    return m_desc.depth_use.is_none() ||
           states.Set(*m_desc.depth_use, resource_registry::TextureStateKind::DepthAttachment);
}

void CustomShaderPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    m_desc.vk_textures.resize(m_desc.texture_bindings.size());
    for (usize i = 0; i < m_desc.texture_bindings.size(); i++) {
        auto& binding = m_desc.texture_bindings[i];
        if (binding.empty()) continue;

        if (binding.use.is_none()) {
            rstd_error("sampled texture {} has no resource use", binding.name.as_str());
            return;
        }
        auto prepared = rr.prepared_resources.Resolve(*binding.use);
        if (prepared.is_none()) {
            rstd_error("prepared sampled texture {} not found", binding.name.as_str());
            return;
        }
        m_desc.vk_textures[i] = (**prepared).image;
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
        auto prepared = rr.prepared_resources.Resolve(*m_desc.output_use);
        if (prepared.is_none()) {
            rstd_error("prepared output texture {} not found", tex_name);
            return;
        }
        m_desc.vk_output          = (**prepared).image.getActive();
        output_attachment_request = rstd::Some((**prepared).request.clone());
        m_desc.samples            = TextureSampleCount(rt.sample_count);
        if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
            if (m_desc.output_msaa_use.is_none()) return;
            auto msaa = rr.prepared_resources.Resolve(*m_desc.output_msaa_use);
            if (msaa.is_none()) {
                rstd_error("prepared MSAA texture {} not found", tex_name);
                return;
            }
            m_desc.vk_output_msaa   = (**msaa).image.getActive();
            msaa_attachment_request = rstd::Some((**msaa).request.clone());
        }
    }

    if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) return;
    SceneMesh& mesh = *(*m_desc.node)->Mesh();
    if (mesh.Submeshes().empty() || m_desc.submesh_index >= mesh.Submeshes().size()) return;
    const auto& submesh = mesh.Submeshes()[m_desc.submesh_index];
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
        auto depth = rr.prepared_resources.Resolve(*m_desc.depth_use);
        if (depth.is_none()) {
            rstd_error("prepared depth texture {} not found", m_desc.output);
            return;
        }
        m_desc.vk_depth          = (**depth).image.getActive();
        depth_attachment_request = rstd::Some((**depth).request.clone());
    }

    std::vector<Uni_ShaderSpv>    spvs;
    DescriptorSetInfo             descriptor_info;
    const CachedShaderReflection* shader_reflection { nullptr };
    const ShaderReflected*        ref { nullptr };
    {
        SceneShader& shader = *(material_ref.customShader.shader);

        if (rr.shader_reflection_cache.is_none()) {
            rstd_error("shader artifact provider unavailable, {}", shader.name);
            return;
        }
        SceneShaderArtifactProvider provider(**rr.shader_reflection_cache, shader);
        auto                        request = provider.Request();
        auto artifact_provider = rstd::dyn<resource::ShaderArtifactProvider>::from_ref(provider);
        auto registered =
            rr.resource_registries.Shaders().Ensure(rstd::move(request), artifact_provider);
        if (registered.is_err()) {
            auto error = rstd::move(registered).unwrap_err_unchecked();
            rstd_error("prepare shader artifact failed: {}", error.message.as_str());
            return;
        }
        m_desc.shader     = rstd::Some(rstd::move(registered).unwrap_unchecked());
        shader_reflection = provider.Reflection();
        if (shader_reflection == nullptr) {
            rstd_error("gen spv reflect failed, {}", shader.name);
            return;
        }
        ref  = &shader_reflection->reflected;
        spvs = CloneShaderSpvs(*shader_reflection);
        for (const auto& blk : ref->blocks) CheckBlockOverlap(blk, shader.name);

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
        m_desc.vk_tex_binding.reserve(m_desc.vk_textures.size());

        for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
            i32 binding { -1 };
            if (i < WE_GLTEX_NAMES.size() && exists(ref->binding_map, WE_GLTEX_NAMES[i])) {
                binding = (i32)ref->binding_map.at(WE_GLTEX_NAMES[i]).binding;
            }
            m_desc.vk_tex_binding.push_back(binding);
        }
    }

    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        if (rr.dyn_buf.is_none()) return;
        RenderBufferResolver buffer_resolver(
            rr.resource_registries.Buffers(), rr.resource_registries.Meshes(), **rr.dyn_buf);
        DrawBufferRequest buffer_request { .render_item   = m_desc.render_item,
                                           .mesh          = &mesh,
                                           .submesh_index = m_desc.submesh_index };
        auto              draw_buffers = buffer_resolver.prepareDrawBuffers(buffer_request);
        if (! draw_buffers) return;
        m_desc.draw_buffers = std::move(*draw_buffers);

        for (unsigned i = 0; i < submesh.vertex_arrays.size(); i++) {
            const auto& vertex    = submesh.vertex_arrays[i];
            auto        attrs_map = vertex.GetAttrOffsetMap();

            VkVertexInputBindingDescription bind_desc {
                .binding   = i,
                .stride    = (uint32_t)vertex.OneSizeOf(),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };
            bind_descriptions.push_back(bind_desc);

            for (auto& item : ref->input_location_map) {
                auto& name   = item.first;
                auto& input  = item.second;
                usize offset = exists(attrs_map, name) ? attrs_map[name].offset : 0;

                VkVertexInputAttributeDescription attr_desc {
                    .location = input.location,
                    .binding  = i,
                    .format   = input.format,
                    .offset   = (u32)offset,
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
        PipelineResourceSystem  pipeline_resources(device,
                                                   rr.resource_registries.DescriptorLayouts(),
                                                   &rr.resource_registries.PipelineCache(),
                                                   &rr.resource_registries.RenderPassCache());
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
        m_desc.pipeline_cache_observed_count    = 0;
        m_desc.render_pass_cache_hit            = false;
        m_desc.render_pass_cache_observed_count = 0;
        auto pipeline_result =
            pipeline_resources.CreateGraphicsPipeline(std::move(pipeline_request));
        if (! pipeline_result.has_value()) return;
        m_desc.pipeline                         = std::move(pipeline_result->pipeline);
        m_desc.pipeline_handle                  = rstd::Some(pipeline_result->handle);
        m_desc.render_pass_handle               = rstd::Some(pipeline_result->render_pass);
        m_desc.pipeline_cache_key               = pipeline_result->cache_key;
        m_desc.render_pass_cache_key            = pipeline_result->render_pass_key;
        m_desc.pipeline_cache_hit               = pipeline_result->cache_hit;
        m_desc.pipeline_cache_observed_count    = pipeline_result->cache_observed_count;
        m_desc.render_pass_cache_hit            = pipeline_result->render_pass_cache_hit;
        m_desc.render_pass_cache_observed_count = pipeline_result->render_pass_cache_observed_count;
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
                MakeFramebufferAttachment(*msaa_attachment_request, m_desc.vk_output_msaa));
        }
        attachments.push_back(
            MakeFramebufferAttachment(*output_attachment_request, m_desc.vk_output));
        if (has_depth_attachment) {
            attachments.push_back(
                MakeFramebufferAttachment(*depth_attachment_request, m_desc.vk_depth));
        }

        m_desc.framebuffer_cache_key.reset();
        m_desc.framebuffer_cache_hit            = false;
        m_desc.framebuffer_cache_observed_count = 0;
        FramebufferResourceSystem framebuffer_resources(
            device,
            &rr.resource_registries.FramebufferCache(),
            &rr.resource_registries.FramebufferDiagnostics());
        auto framebuffer = framebuffer_resources.CreateFramebuffer(FramebufferResourceRequest {
            .render_pass     = **m_desc.pipeline->pipeline.pass,
            .render_pass_key = m_desc.render_pass_cache_key.value_or(RenderPassCacheKey {}),
            .attachments     = std::move(attachments),
            .extent          = { m_desc.vk_output.extent.width, m_desc.vk_output.extent.height },
        });
        if (! framebuffer.has_value()) return;
        m_desc.fb                               = std::move(framebuffer->framebuffer);
        m_desc.framebuffer_handle               = rstd::Some(framebuffer->handle);
        m_desc.framebuffer_cache_key            = framebuffer->cache_key;
        m_desc.framebuffer_cache_hit            = framebuffer->cache_hit;
        m_desc.framebuffer_cache_observed_count = framebuffer->cache_observed_count;
    }

    if (! ref->blocks.empty()) {
        auto& block = ref->blocks.front();
        if (rr.dyn_buf.is_none()) return;
        (*rr.dyn_buf)
            ->allocateSubRef(
                block.size, m_desc.ubo_buf, device.limits().minUniformBufferOffsetAlignment);
    }

    {
        auto images = rstd::vec::Vec<resource_registry::DescriptorImageBinding>::with_capacity(
            m_desc.vk_textures.size());
        for (usize index = 0; index < m_desc.vk_textures.size(); ++index) {
            auto  binding = m_desc.vk_tex_binding[index];
            auto& slots   = m_desc.vk_textures[index];
            if (binding < 0 || slots.slots.empty()) continue;
            images.push(resource_registry::DescriptorImageBinding {
                .binding = static_cast<u32>(binding),
                .image   = slots.getActive(),
            });
        }
        rstd::Option<resource_registry::DescriptorBufferBinding> buffer = rstd::None();
        if (m_desc.ubo_buf) {
            buffer = rstd::Some(resource_registry::DescriptorBufferBinding {
                .binding = 0,
                .buffer  = (*rr.dyn_buf)->gpuBuf(),
                .offset  = m_desc.ubo_buf.offset,
                .size    = m_desc.ubo_buf.size,
            });
        }
        if (m_desc.pipeline->descriptor_layouts.is_empty()) return;
        auto layout = rr.resource_registries.DescriptorLayouts().Resolve(
            m_desc.pipeline->descriptor_layouts[0]);
        if (layout.is_none()) return;
        auto descriptor = rr.resource_registries.Descriptors().Prepare(
            device, **layout, images.as_slice(), buffer);
        if (descriptor.is_err()) {
            auto error = rstd::move(descriptor).unwrap_err_unchecked();
            rstd_error("prepare descriptor binding failed: {}", error.message.as_str());
            return;
        }
        auto prepared = rstd::move(descriptor).unwrap_unchecked();
        auto handle   = prepared.handle;
        if (! rr.prepared_resources.Insert(rstd::move(prepared))) return;
        m_desc.descriptor_binding = rstd::Some(handle);
    }

    if (! ref->blocks.empty()) {
        std::function<void()> update_dyn_buf_op;
        if (m_desc.draw_buffers.dynamic) {
            auto& mesh        = *(*m_desc.node)->Mesh();
            auto  smi         = m_desc.submesh_index;
            auto  render_item = m_desc.render_item;
            if (rr.dyn_buf.is_none()) return;
            auto resolver_buf = *rr.dyn_buf;
            auto resolver_buffers =
                rstd::mut_ref<resource_registry::BufferRegistry>::from_raw_parts(
                    rstd::addressof(rr.resource_registries.Buffers()));
            auto resolver_meshes = rstd::mut_ref<MeshCache>::from_raw_parts(
                rstd::addressof(rr.resource_registries.Meshes()));
            auto& draw_buffers = m_desc.draw_buffers;
            update_dyn_buf_op  = [&mesh,
                                  smi,
                                  render_item,
                                  &draw_buffers,
                                  resolver_buffers,
                                  resolver_meshes,
                                  resolver_buf]() mutable {
                RenderBufferResolver resolver(*resolver_buffers, *resolver_meshes, *resolver_buf);
                DrawBufferRequest    request { .render_item   = render_item,
                                               .mesh          = &mesh,
                                               .submesh_index = smi };
                (void)resolver.updateDynamicDrawBuffers(request, draw_buffers);
            };
        }

        auto block = ref->blocks.front();
        if (rr.dyn_buf.is_none()) return;
        auto  buf    = *rr.dyn_buf;
        auto* bufref = &m_desc.ubo_buf;

        auto  node           = *m_desc.node;
        auto* shader_updater = scene.shaderValueUpdater.get();
        auto& sprites        = m_desc.sprites_map;
        auto& vk_textures    = m_desc.vk_textures;

        m_desc.update_op = [shader_updater,
                            block,
                            buf,
                            bufref,
                            node,
                            &sprites,
                            &vk_textures,
                            update_dyn_buf_op,
                            mat = &material_ref]() mutable {
            // Re-push constValues when the host wrote a new user property since
            // the last frame. Same-thread mutation (RenderHandler runs on the
            // render loop), so no atomic needed.
            if (mat->customShader.dirty) {
                for (auto& v : mat->customShader.constValues) {
                    if (exists(block.member_map, v.first)) {
                        UpdateUniform(*buf, *bufref, block, v.first, v.second);
                    }
                }
                mat->customShader.dirty = false;
            }
            auto update_unf_op = [&block, buf, bufref](std::string_view name,
                                                       owe::ShaderValue value) mutable {
                UpdateUniform(*buf, *bufref, block, name, value);
            };
            shader_updater->UpdateUniforms(node.as_raw_ptr(), sprites, update_unf_op);
            // update image slot for sprites
            {
                for (auto& [i, sp] : sprites) {
                    if (i >= vk_textures.size()) continue;
                    vk_textures.at(i).active = sp.GetCurFrame().imageId;
                }
            }
            if (update_dyn_buf_op) update_dyn_buf_op();
        };

        auto exists_unf_op = [&block](std::string_view name) {
            return exists(block.member_map, name);
        };
        shader_updater->InitUniforms(node.as_raw_ptr(), exists_unf_op);

        // memset uniform buf
        buf->fillBuf(*bufref, 0, bufref->size, 0);
        {
            auto&      default_values = material_ref.customShader.shader->default_uniforms;
            auto&      const_values   = material_ref.customShader.constValues;
            std::array values_array   = { &default_values, &const_values };
            for (auto& values : values_array) {
                for (auto& v : *values) {
                    if (exists(block.member_map, v.first)) {
                        UpdateUniform(*buf, *bufref, block, v.first, v.second);
                    }
                }
            }
            // const_values was just fully written — clear any pending re-push
            // request from a prior RenderSetUserProperty.
            material_ref.customShader.dirty = false;
        }
        m_desc.update_op();
    }

    {
        if (out_force_clear || m_desc.transparent_clear) {
            // Some offscreen RTs need a transparent reset, not the scene's
            // opaque clear color.
            m_desc.clear_value =
                VkClearValue { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } } };
            m_desc.clear_value_src = rstd::None();
        } else {
            auto& sc = scene.clearColor;
            m_desc.clear_value =
                VkClearValue { .color = { .float32 = { sc[0], sc[1], sc[2], 1.0f } } };
            // Track the live scene.clearColor: per-frame re-sync in
            // execute() picks up live edits (e.g. `schemecolor` user
            // property changes) without a render-graph rebuild.
            m_desc.clear_value_src = rstd::Some(
                rstd::ref<std::array<float, 3>>::from_raw_parts(rstd::addressof(scene.clearColor)));
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
    if (m_desc.vk_output.handle != prev.vk_output.handle ||
        m_desc.vk_output.view != prev.vk_output.view ||
        m_desc.vk_output.extent.width != prev.vk_output.extent.width ||
        m_desc.vk_output.extent.height != prev.vk_output.extent.height) {
        return false;
    }
    if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT &&
        (m_desc.vk_output_msaa.handle != prev.vk_output_msaa.handle ||
         m_desc.vk_output_msaa.view != prev.vk_output_msaa.view)) {
        return false;
    }
    if (m_desc.has_depth_attachment && (m_desc.vk_depth.handle != prev.vk_depth.handle ||
                                        m_desc.vk_depth.view != prev.vk_depth.view)) {
        return false;
    }
    return true;
}

void CustomShaderPass::prepareRenderScopeDraw(PassRecordContext& context) {
    if (m_desc.update_op) m_desc.update_op();

    // Re-sync clear_value from the live scene.clearColor when this pass
    // tracks the scene clear (i.e. not a per-layer transparent reset).
    // Cheap: a 3-float copy per pass per frame.
    if (m_desc.clear_value_src) {
        const auto& sc                      = **m_desc.clear_value_src;
        m_desc.clear_value.color.float32[0] = sc[0];
        m_desc.clear_value.color.float32[1] = sc[1];
        m_desc.clear_value.color.float32[2] = sc[2];
        m_desc.clear_value.color.float32[3] = 1.0f;
    }

    recordSampledImageBarriers(context);
}

void CustomShaderPass::recordSampledImageBarriers(PassRecordContext& context) {
    m_desc.sampled_barriers.Record(*context.command);
}

void CustomShaderPass::beginRenderScope(PassRecordContext& context) {
    auto&          cmd         = *context.command;
    auto&          outext      = m_desc.vk_output.extent;
    const bool     has_msaa    = m_desc.samples != VK_SAMPLE_COUNT_1_BIT;
    const uint32_t clear_count = (has_msaa ? 2u : 1u) + (m_desc.has_depth_attachment ? 1u : 0u);
    std::array<VkClearValue, 3> clears {};
    clears[0] = m_desc.clear_value;
    if (m_desc.has_depth_attachment) {
        const uint32_t depth_index       = has_msaa ? 2u : 1u;
        clears[depth_index].depthStencil = { 1.0f, 0 };
    }
    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = **m_desc.pipeline->pipeline.pass,
        .framebuffer = **m_desc.fb,
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
    auto& cmd    = *context.command;
    auto& outext = m_desc.vk_output.extent;
    if (m_desc.descriptor_binding.is_some()) {
        auto descriptor = context.prepared_resources->Resolve(*m_desc.descriptor_binding);
        if (descriptor.is_none()) return;
        (**descriptor).Record(cmd, *m_desc.pipeline->pipeline.layout);
    }

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline->pipeline.handle);
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
    if (draw_buffers.dynamic) {
        if (context.dynamic_buffer.is_none()) return;
        auto gpu_buf = (*context.dynamic_buffer)->gpuBuf();
        for (usize i = 0; i < draw_buffers.dynamic_vertices.size(); i++) {
            auto& buf = draw_buffers.dynamic_vertices[i];
            cmd.BindVertexBuffers((u32)i, 1, &gpu_buf, &buf.offset);
        }
        if (draw_buffers.dynamic_index) {
            cmd.BindIndexBuffer(gpu_buf, draw_buffers.dynamic_index.offset, VK_INDEX_TYPE_UINT32);
        }
    } else {
        for (usize i = 0; i < draw_buffers.static_vertices.len(); i++) {
            auto&        mref = draw_buffers.static_vertices[i].physical->buffer;
            VkBuffer     vb   = mref.buffer();
            VkDeviceSize off  = mref.offset();
            cmd.BindVertexBuffers((u32)i, 1, &vb, &off);
        }
        if (draw_buffers.static_index.is_some()) {
            auto&        mref = draw_buffers.static_index->physical->buffer;
            VkBuffer     ib   = mref.buffer();
            VkDeviceSize off  = mref.offset();
            cmd.BindIndexBuffer(ib, off, VK_INDEX_TYPE_UINT32);
        }
    }

    const bool has_index = draw_buffers.hasIndex();
    if (has_index) {
        const auto& submeshes = (*m_desc.node)->Mesh()->Submeshes();
        static const std::vector<SceneMesh::DrawRange> kEmpty;
        const auto& ranges = (m_desc.submesh_index < submeshes.size())
                                 ? submeshes[m_desc.submesh_index].draw_ranges
                                 : kEmpty;
        if (ranges.empty()) {
            cmd.DrawIndexed(draw_buffers.draw_count, 1, 0, 0, 0);
        } else {
            // Per-part drawing — preserves the file's z-order so later parts
            // overdraw earlier ones (eyelid over pupil during blink).
            for (const auto& r : ranges) {
                cmd.DrawIndexed(r.index_count, 1, r.first_index, 0, 0);
            }
        }
    } else {
        cmd.Draw(draw_buffers.draw_count, 1, 0, 0);
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

void CustomShaderPass::destory(const Device&, RenderingResources& rr) {
    m_desc.update_op          = {};
    m_desc.pipeline_handle    = rstd::None();
    m_desc.render_pass_handle = rstd::None();
    m_desc.framebuffer_handle = rstd::None();
    m_desc.shader             = rstd::None();
    m_desc.descriptor_binding = rstd::None();
    rr.resource_registries.Retirement().Retire(std::move(m_desc.fb));
    m_desc.fb.reset();
    rr.resource_registries.Retirement().Retire(std::move(m_desc.pipeline));
    m_desc.pipeline.reset();
    m_desc.pipeline_cache_key.reset();
    m_desc.render_pass_cache_key.reset();
    m_desc.framebuffer_cache_key.reset();
    m_desc.pipeline_cache_hit               = false;
    m_desc.pipeline_cache_observed_count    = 0;
    m_desc.render_pass_cache_hit            = false;
    m_desc.render_pass_cache_observed_count = 0;
    m_desc.framebuffer_cache_hit            = false;
    m_desc.framebuffer_cache_observed_count = 0;
    if (rr.dyn_buf.is_none()) return;
    RenderBufferResolver resolver(
        rr.resource_registries.Buffers(), rr.resource_registries.Meshes(), **rr.dyn_buf);
    resolver.releaseDynamicDrawBuffers(m_desc.draw_buffers);
    if (m_desc.ubo_buf) (*rr.dyn_buf)->unallocateSubRef(m_desc.ubo_buf);
    m_desc.ubo_buf      = {};
    m_desc.draw_buffers = {};
}

bool CustomShaderPass::setTextureBinding(uint32_t index, TextureBindingRequest binding) {
    if (index >= m_desc.texture_bindings.size()) return false;
    m_desc.texture_bindings[index] = std::move(binding);
    return true;
}
