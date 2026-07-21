module;

#include <rstd/macro.hpp>

module wescene.vulkan_render;
import rstd;
import rstd.cppstd;
import rstd.log;
import wescene.resource;
import wescene.scene;

using namespace rstd::prelude;

namespace owe::vulkan
{

auto ProgramUniformFrameContext::TextureFrame(SceneDrawItemId draw, usize texture_index) const
    -> Option<SceneTextureFrameView> {
    return m_textures->TextureFrame(draw, texture_index);
}

auto SceneUniformBindingPrepareContext::ResolveDraw(SceneDrawItemId id) const
    -> Option<UniformPrepareDraw> {
    auto draw = m_scene->ResourceIndex().resolve(id);
    if (draw.is_none() || draw->node == nullptr || draw->material == nullptr) return None();
    auto node_id = m_scene->ResourceIndex().nodeId(*draw->node);
    if (node_id.is_none()) return None();
    return Some(UniformPrepareDraw {
        .draw_item = id,
        .node_id   = *node_id,
        .node      = ref<SceneNode>::from_raw_parts(draw->node),
        .material  = ref<SceneMaterial>::from_raw_parts(draw->material),
    });
}

auto SceneUniformBindingPrepareContext::DrawItemFor(ref<SceneNode> node, u32 submesh_index) const
    -> Option<SceneDrawItemId> {
    auto node_id = m_scene->ResourceIndex().nodeId(*node);
    if (node_id.is_none()) return None();
    return m_scene->ResourceIndex().drawItemFor(*node_id, submesh_index);
}

auto SceneUniformBindingPrepareContext::GlobalSources() const -> slice<UniformSourceAttachment> {
    return m_scene->GlobalSources();
}

auto SceneUniformBindingPrepareContext::NodeSources(SceneNodeId node) const
    -> slice<UniformSourceAttachment> {
    return m_scene->NodeSources(node);
}

auto SceneUniformBindingPrepareContext::ResolveSource(UniformSourceId source) const
    -> Option<ref<dyn<UniformSource>>> {
    return m_scene->Resolve(source);
}

namespace detail
{

class SourceBindingCompiler {
public:
    SourceBindingCompiler(const UniformBufferLayout& layout, BoundUniformSource& source)
        : m_layout(layout), m_source(source) {}

    auto Bind(UniformOutputId output, std::string_view shader_member, UniformValueShape shape)
        -> Result<bool, UniformError> {
        bool matched = false;
        for (usize index {}; index < m_layout.slots.len(); ++index) {
            const auto& slot = m_layout.slots[index];
            if (rstd::cppstd::as_string_view(slot.name.as_str()) != shader_member) continue;
            if (slot.size % usize(sizeof(float)) != usize()) {
                return Err(UniformError {
                    .message =
                        rstd::format("uniform {} has non-float size {}", shader_member, slot.size),
                });
            }
            const auto elements =
                u32(static_cast<rstd::uint32_t>((slot.size / usize(sizeof(float))).to_primitive()));
            if ((shape.min_elements != u32() && elements < shape.min_elements) ||
                (shape.max_elements != u32() && elements > shape.max_elements)) {
                return Err(UniformError {
                    .message = rstd::format("uniform {} shape mismatch: reflected {} floats, "
                                            "source expects {}..{}",
                                            shader_member,
                                            elements,
                                            shape.min_elements,
                                            shape.max_elements),
                });
            }
            bool exists = false;
            for (const auto& binding : m_source.outputs) {
                if (binding.output == output && binding.slot_index == index) {
                    exists = true;
                    break;
                }
            }
            if (! exists) {
                m_source.outputs.push(BoundUniformOutput {
                    .output     = output,
                    .slot_index = index,
                });
            }
            matched = true;
        }
        return Ok(matched);
    }

private:
    const UniformBufferLayout& m_layout;
    BoundUniformSource&        m_source;
};

class SourceValueWriter {
public:
    SourceValueWriter(const UniformBufferBinding& binding, const BoundUniformSource& source)
        : m_binding(binding), m_source(source) {}

    bool Wants(UniformOutputId output) const {
        for (const auto& binding : m_source.outputs) {
            if (binding.output == output) return true;
        }
        return false;
    }

    auto Write(UniformOutputId output, UniformValueView value) -> Result<empty, UniformError> {
        bool wrote = false;
        for (const auto& binding : m_source.outputs) {
            if (binding.output != output) continue;
            wrote = m_binding.WriteSlot(binding.slot_index, value) || wrote;
        }
        if (! wrote) {
            return Err(UniformError { .message = String::make("uniform output is not bound") });
        }
        return Ok(empty {});
    }

private:
    const UniformBufferBinding& m_binding;
    const BoundUniformSource&   m_source;
};

class ResourceSnapshot {
public:
    ResourceSnapshot(ref<dyn<UniformBufferFrameContext>> frame, SceneDrawItemId draw,
                     slice<PreparedUniformTextureMetadata> textures)
        : m_frame(frame), m_draw(draw), m_textures(textures) {}

    auto Texture(usize index) const -> Option<UniformTextureView> {
        UniformTextureView view;
        bool               available = false;
        if (index < m_textures.len() && m_textures[index].available) {
            const auto& prepared = m_textures[index];
            view.has_extent      = true;
            view.source_extent   = prepared.source_extent;
            view.sample_extent   = prepared.sample_extent;
            view.has_mipmap      = prepared.has_mipmap;
            view.mipmap_level    = prepared.mipmap_level;
            view.revision        = prepared.revision;
            available            = true;
        }
        auto frame = m_frame->TextureFrame(m_draw, index);
        if (frame.is_some()) {
            view.has_transform = true;
            view.rotation      = frame->rotation;
            view.translation   = frame->translation;
            auto revision      = view.revision.to_primitive();
            revision ^= frame->revision.to_primitive() + 0x9e3779b97f4a7c15ULL + (revision << 6U) +
                        (revision >> 2U);
            view.revision = u64(revision);
            if (view.revision == u64()) view.revision = u64(1);
            available = true;
        }
        return available ? Some(view) : None<UniformTextureView>();
    }
    auto Viewport() const -> rstd::array<float, 2> { return m_frame->Viewport(); }
    auto TexelSize() const -> rstd::array<float, 2> {
        const auto viewport = m_frame->Viewport();
        return { viewport[usize(0)] > 0.0f ? 1.0f / viewport[usize(0)] : 0.0f,
                 viewport[usize(1)] > 0.0f ? 1.0f / viewport[usize(1)] : 0.0f };
    }

private:
    mutable ref<dyn<UniformBufferFrameContext>> m_frame;
    SceneDrawItemId                             m_draw;
    slice<PreparedUniformTextureMetadata>       m_textures;
};

class UpdateContext {
public:
    UpdateContext(ref<SceneFrame> frame, const ResourceSnapshot& resources)
        : m_frame(frame), m_resources(dyn<UniformResourceView>::from_ref(resources)) {}

    auto Frame() const -> ref<SceneFrame> { return m_frame; }
    auto Resources() const -> ref<dyn<UniformResourceView>> { return m_resources; }

private:
    ref<SceneFrame>               m_frame;
    ref<dyn<UniformResourceView>> m_resources;
};

} // namespace detail

} // namespace owe::vulkan

namespace owe::vulkan
{

auto CompileUniformBufferLayout(const resource::ShaderArtifactUniformBlock& block)
    -> Result<UniformBufferLayout, UniformBufferUpdateError> {
    UniformBufferLayout layout { .size = block.size };
    layout.slots.reserve(block.members.len());

    for (const auto& member : block.members) {
        const auto offset = rstd::as_cast<usize>(member.offset);
        if (offset > block.size || member.size > block.size - offset) {
            return Err(UniformBufferUpdateError {
                .message = rstd::format(
                    "uniform {} lies outside block {}", member.name.as_str(), block.name.as_str()),
            });
        }
        layout.slots.push(UniformSlot {
            .name   = member.name.clone(),
            .offset = offset,
            .size   = member.size,
            .count  = member.count,
        });
    }

    auto order = rstd::vec::Vec<usize>::with_capacity(layout.slots.len());
    for (usize index {}; index < layout.slots.len(); ++index) {
        order.push(usize(index.to_primitive()));
    }
    std::sort(order.begin(), order.end(), [&](usize lhs, usize rhs) {
        return layout.slots[lhs].offset < layout.slots[rhs].offset;
    });
    for (usize index { 1 }; index < order.len(); ++index) {
        const auto& previous = layout.slots[order[index - usize(1)]];
        const auto& current  = layout.slots[order[index]];
        if (current.offset < previous.offset + previous.size) {
            rstd_warn("uniform block {} overlaps {} and {}",
                      block.name.as_str(),
                      previous.name.as_str(),
                      current.name.as_str());
        }
    }
    return Ok(rstd::move(layout));
}

UniformBufferBinding::UniformBufferBinding(SceneDrawItemId           draw_item,
                                           resource::BufferUseHandle buffer,
                                           UniformBufferLayout       layout,
                                           Vec<BoundUniformSource> sources, ShaderValues defaults,
                                           ref<SceneMaterial>                  material,
                                           Vec<PreparedUniformTextureMetadata> textures)
    : m_draw_item(draw_item),
      m_buffer(buffer),
      m_layout(rstd::move(layout)),
      m_sources(rstd::move(sources)),
      m_data(rstd::vec::Vec<u8>::with_capacity(m_layout.size)),
      m_base_data(rstd::vec::Vec<u8>::with_capacity(m_layout.size)),
      m_defaults(rstd::move(defaults)),
      m_material(material),
      m_textures(rstd::move(textures)) {
    m_data.resize(m_layout.size, u8(0));
    m_base_data.resize(m_layout.size, u8(0));
}

bool UniformBufferBinding::WriteSlot(usize slot_index, UniformValueView value) const {
    if (slot_index >= m_layout.slots.len() || value.data == nullptr) return false;
    const auto& slot       = m_layout.slots[slot_index];
    const usize value_size = value.size * usize(sizeof(float));
    auto source = slice<u8>::from_raw_parts(reinterpret_cast<const u8*>(value.data), value_size);
    rstd::vec::Vec<float> resized;
    if (slot.size != value_size && slot.size % usize(sizeof(float)) == usize()) {
        const auto count = slot.size / usize(sizeof(float));
        resized          = rstd::vec::Vec<float>::with_capacity(count);
        resized.resize(count, 0.0f);
        for (usize index {}; index < rstd::cmp::min(value.size, count); ++index) {
            resized[index] = value.data[index.to_primitive()];
        }
        source = slice<u8>::from_raw_parts(reinterpret_cast<const u8*>(resized.data()), slot.size);
    } else if (slot.size != value_size) {
        rstd_warn("uniform {} size mismatch: reflected {} bytes, value {} bytes",
                  slot.name.as_str(),
                  slot.size,
                  value_size);
        source =
            slice<u8>::from_raw_parts(source.as_raw_ptr(), rstd::cmp::min(slot.size, source.len()));
    }
    if (slot.offset > m_data.len() || source.len() > m_data.len() - slot.offset) return false;
    for (usize index {}; index < source.len(); ++index) {
        m_data[slot.offset + index] = source[index];
    }
    return true;
}

bool UniformBufferBinding::WriteName(std::string_view name, const UniformValue& value) const {
    for (usize index {}; index < m_layout.slots.len(); ++index) {
        if (rstd::cppstd::as_string_view(m_layout.slots[index].name.as_str()) != name) continue;
        return WriteSlot(index, value.View());
    }
    return false;
}

auto UniformBufferBinding::Update(ref<dyn<UniformBufferFrameContext>>         frame_context,
                                  mut_ref<dyn<resource::BufferContentWriter>> buffers) const
    -> Result<empty, UniformBufferUpdateError> {
    auto&      material = *m_material;
    const bool force    = ! m_uploaded || m_material_version != material.customShader.value_version;
    if (force) {
        for (auto& byte : m_data) byte = u8();
        for (const auto& [name, value] : m_defaults) WriteName(name, value);
        for (const auto& [name, value] : material.customShader.constValues) {
            WriteName(name, value);
        }
        m_base_data = m_data.clone();
    }

    detail::ResourceSnapshot resources(frame_context, m_draw_item, m_textures.as_slice());

    detail::UpdateContext context_impl(frame_context->Frame(), resources);
    auto                  context = dyn<UniformUpdateContext>::from_ref(context_impl);

    auto versions = Vec<u64>::with_capacity(m_sources.len());
    bool changed  = force;
    for (auto& bound : m_sources) {
        u64 version = bound.source->Version(context.as_ref());
        versions.push(rstd::move(version));
        changed = changed || ! bound.evaluated || bound.version != version;
    }
    if (! changed) return Ok(empty {});

    m_data = m_base_data.clone();
    usize source_index {};
    for (auto& bound : m_sources) {
        detail::SourceValueWriter writer_impl(*this, bound);
        auto                      writer = dyn<UniformValueSink>::from_ref(writer_impl);
        auto evaluated = bound.source->Evaluate(context.as_ref(), writer.as_mut_ref());
        if (evaluated.is_err()) {
            return Err(UniformBufferUpdateError {
                .message = rstd::move(evaluated).unwrap_err_unchecked().message,
            });
        }
        bound.version   = versions[source_index++];
        bound.evaluated = true;
    }

    ++m_content_version;
    if (m_content_version == u64()) ++m_content_version;
    auto updated = buffers->UpdateBuffer(m_buffer, m_data.as_slice(), m_content_version);
    if (updated.is_err()) {
        auto error = rstd::move(updated).unwrap_err_unchecked();
        return Err(UniformBufferUpdateError { .message = rstd::move(error.message) });
    }
    m_material_version = material.customShader.value_version;
    m_uploaded         = true;
    return Ok(empty {});
}

auto MakeUniformBufferBinding(ref<dyn<UniformBindingPrepareContext>> prepare,
                              SceneDrawItemId draw_item, resource::BufferUseHandle buffer,
                              const resource::ShaderArtifactUniformBlock& block,
                              Vec<PreparedUniformTextureMetadata>         textures)
    -> Result<Box<dyn<UniformBufferUpdate>>, UniformBufferUpdateError> {
    auto draw = prepare->ResolveDraw(draw_item);
    if (draw.is_none()) {
        return Err(UniformBufferUpdateError {
            .message = String::make("uniform binding scene data is unavailable"),
        });
    }
    if (! draw->material->customShader.shader) {
        return Err(UniformBufferUpdateError {
            .message = String::make("uniform binding shader metadata is unavailable"),
        });
    }

    auto layout_result = CompileUniformBufferLayout(block);
    if (layout_result.is_err()) {
        return Err(rstd::move(layout_result).unwrap_err_unchecked());
    }
    auto layout = rstd::move(layout_result).unwrap_unchecked();

    struct RankedSource {
        UniformSourceId source;
        rstd::int32_t   priority { 0 };
    };
    auto ranked = Vec<RankedSource>::make();
    auto append = [&](slice<UniformSourceAttachment> attachments) {
        for (usize index {}; index < attachments.len(); ++index) {
            const auto&   attachment = attachments[index];
            Option<usize> found;
            for (usize candidate {}; candidate < ranked.len(); ++candidate) {
                if (ranked[candidate].source == attachment.source) {
                    found = Some(candidate);
                    break;
                }
            }
            if (found.is_none()) {
                ranked.push(RankedSource { attachment.source, attachment.priority });
            } else {
                ranked[*found].priority = attachment.priority;
            }
        }
    };
    append(prepare->GlobalSources());
    append(prepare->NodeSources(draw->node_id));
    std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.priority != rhs.priority) return lhs.priority < rhs.priority;
        if (lhs.source.generation != rhs.source.generation)
            return lhs.source.generation < rhs.source.generation;
        return lhs.source.index < rhs.source.index;
    });

    auto sources = Vec<BoundUniformSource>::with_capacity(ranked.len());
    rstd::collections::HashMap<usize, usize> slot_sources;
    usize                                    source_ordinal { 0 };
    for (const auto& candidate : ranked) {
        auto source = prepare->ResolveSource(candidate.source);
        if (source.is_none()) {
            return Err(UniformBufferUpdateError {
                .message = String::make("scene uniform source is unavailable"),
            });
        }
        BoundUniformSource bound {
            .source   = *source,
            .priority = candidate.priority,
        };
        detail::SourceBindingCompiler compiler_impl(layout, bound);
        auto                          compiler  = dyn<UniformBindingSink>::from_ref(compiler_impl);
        auto                          described = bound.source->Describe(compiler.as_mut_ref());
        if (described.is_err()) {
            return Err(UniformBufferUpdateError {
                .message = rstd::move(described).unwrap_err_unchecked().message,
            });
        }
        if (bound.outputs.is_empty()) continue;
        bound.lease = bound.source->AcquireBindingLease();
        for (const auto& output : bound.outputs) {
            auto previous = slot_sources.get(output.slot_index);
            if (previous.is_none()) {
                (void)slot_sources.insert(output.slot_index, source_ordinal);
            } else if (**previous != source_ordinal) {
                rstd_warn("uniform slot {} has multiple sources {} and {}",
                          layout.slots[output.slot_index].name.as_str(),
                          **previous,
                          source_ordinal);
            }
        }
        sources.push(rstd::move(bound));
        ++source_ordinal;
    }

    const auto&          shader = *draw->material->customShader.shader;
    UniformBufferBinding binding(draw->draw_item,
                                 buffer,
                                 rstd::move(layout),
                                 rstd::move(sources),
                                 shader.default_uniforms,
                                 draw->material,
                                 rstd::move(textures));
    return Ok(Box<dyn<UniformBufferUpdate>>::make(rstd::move(binding)));
}

} // namespace owe::vulkan
