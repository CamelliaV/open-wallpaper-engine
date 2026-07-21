module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;

import eigen;
import rstd;
import rstd.cppstd;
import wescene.core;
import wescene.particle;
import wescene.particle.program;
import wescene.scene;
import wescene.spec_names;

using namespace rstd::prelude;
using namespace owe;

namespace
{

struct WPGOption {
    bool thick_format { false };
};

struct AttrSlot {
    usize offset {};
    bool  enabled { false };
};

auto FindAttrSlot(
    const owe::Map<std::string, SceneVertexArray::SceneVertexAttributeOffset>& attributes,
    std::string_view name) noexcept -> AttrSlot {
    auto found = attributes.find(std::string(name));
    if (found == attributes.end()) return {};
    return {
        .offset  = found->second.offset / usize(sizeof(float)),
        .enabled = true,
    };
}

auto AnimationLifetime(const WPParticleConstRef& value, WPParticleAnimationSpec animation) noexcept
    -> float {
    if (value.lifetime <= 0.0f) return 0.0f;
    switch (animation.mode) {
    case WPParticleAnimationMode::RANDOMONE:
        return std::clamp(value.random, 0.0f, std::nextafter(1.0f, 0.0f));
    case WPParticleAnimationMode::SEQUENCE:
        if (value.initial.lifetime == 0.0f) return 0.0f;
        return (1.0f - (value.lifetime / value.initial.lifetime)) * animation.sequence_multiplier;
    }
    return 0.0f;
}

template<rstd::size_t N>
void AssignVertexTimes(std::span<float> destination, const rstd::array<float, N>& source,
                       usize count) noexcept {
    auto destination_size = usize(destination.size()) / count;
    for (usize index {}; index < count; ++index) {
        auto offset = index * destination_size;
        std::copy(source.begin(), source.end(), destination.begin() + offset.to_primitive());
    }
}

template<rstd::size_t N>
void AssignVertex(std::span<float> destination, const rstd::array<float, N>& source,
                  usize count) noexcept {
    auto destination_size = usize(destination.size()) / count;
    auto source_size      = usize(N) / count;
    for (usize index {}; index < count; ++index) {
        auto destination_offset = index * destination_size;
        auto source_offset      = index * source_size;
        std::copy(source.begin() + source_offset.to_primitive(),
                  source.begin() + (source_offset + source_size).to_primitive(),
                  destination.begin() + destination_offset.to_primitive());
    }
}

auto GenParticleData(particle::ParticleExtractContext& context,
                     const WPParticleSubSystem& subsystem, WPGOption option,
                     SceneVertexArray& vertices) noexcept -> usize {
    rstd::array<float, 32 * 4> storage {};
    auto                       one_size   = vertices.OneSize();
    auto                       total_size = one_size * usize(4);
    usize                      output_index {};

    for (usize instance_index {}; instance_index < context.instances.len(); ++instance_index) {
        if (subsystem.InstanceState(instance_index).no_live_particle) continue;
        const auto& particle_storage = context.instances[instance_index]->Storage();
        auto        states           = particle_storage.Values(particle_storage.SlotStateKey());
        for (usize slot_index {}; slot_index < states.len(); ++slot_index) {
            if (! states[slot_index].active) continue;
            auto value = MakeWPParticleConstRef(
                particle_storage, subsystem.Attributes(), particle::ParticleSlot { slot_index });
            if (value.lifetime <= 0.0f) continue;

            auto position =
                subsystem.InstanceState(instance_index).bounded.position + value.position;
            auto  size     = value.size * 0.5f;
            auto  lifetime = AnimationLifetime(value, subsystem.AnimationSpec());
            usize offset {};

            AssignVertexTimes(std::span<float> { storage.data() + offset.to_primitive(),
                                                 total_size.to_primitive() },
                              rstd::array<float, 3> { position[0], position[1], position[2] },
                              usize(4));
            offset += usize(4);

            AssignVertex(std::span<float> { storage.data() + offset.to_primitive(),
                                            total_size.to_primitive() },
                         rstd::array<float, 16> {
                             0.0f,
                             1.0f,
                             value.rotation[2],
                             size,
                             1.0f,
                             1.0f,
                             value.rotation[2],
                             size,
                             1.0f,
                             0.0f,
                             value.rotation[2],
                             size,
                             0.0f,
                             0.0f,
                             value.rotation[2],
                             size,
                         },
                         usize(4));
            offset += usize(4);

            AssignVertexTimes(std::span<float> { storage.data() + offset.to_primitive(),
                                                 total_size.to_primitive() },
                              rstd::array<float, 4> {
                                  value.color[0], value.color[1], value.color[2], value.alpha },
                              usize(4));
            offset += usize(4);

            if (option.thick_format) {
                AssignVertexTimes(std::span<float> { storage.data() + offset.to_primitive(),
                                                     total_size.to_primitive() },
                                  rstd::array<float, 4> {
                                      value.velocity[0],
                                      value.velocity[1],
                                      value.velocity[2],
                                      lifetime,
                                  },
                                  usize(4));
                offset += usize(4);
            }

            AssignVertexTimes(std::span<float> { storage.data() + offset.to_primitive(),
                                                 total_size.to_primitive() },
                              rstd::array<float, 2> { value.rotation[0], value.rotation[1] },
                              usize(4));
            vertices.SetVertexs(
                output_index++ * usize(4),
                std::span<const float> { storage.data(), total_size.to_primitive() });
        }
    }
    return output_index;
}

auto GenParticlePointData(particle::ParticleExtractContext& context,
                          const WPParticleSubSystem& subsystem, WPGOption option,
                          SceneVertexArray& vertices) noexcept -> usize {
    auto                   one_size          = vertices.OneSize();
    const auto             attribute_offsets = vertices.GetAttrOffsetMap();
    const AttrSlot         position          = FindAttrSlot(attribute_offsets, WE_IN_POSITION);
    const AttrSlot         texcoord          = FindAttrSlot(attribute_offsets, WE_IN_TEXCOORDVEC4);
    const AttrSlot         color             = FindAttrSlot(attribute_offsets, WE_IN_COLOR);
    const AttrSlot         velocity = FindAttrSlot(attribute_offsets, WE_IN_TEXCOORDVEC4C1);
    rstd::array<float, 16> data {};
    usize                  output_index {};

    auto write3 = [&](AttrSlot slot, float x, float y, float z) noexcept {
        if (! slot.enabled) return;
        data[slot.offset]            = x;
        data[slot.offset + usize(1)] = y;
        data[slot.offset + usize(2)] = z;
    };
    auto write4 = [&](AttrSlot slot, float x, float y, float z, float w) noexcept {
        if (! slot.enabled) return;
        data[slot.offset]            = x;
        data[slot.offset + usize(1)] = y;
        data[slot.offset + usize(2)] = z;
        data[slot.offset + usize(3)] = w;
    };

    for (usize instance_index {}; instance_index < context.instances.len(); ++instance_index) {
        if (subsystem.InstanceState(instance_index).no_live_particle) continue;
        const auto& particle_storage = context.instances[instance_index]->Storage();
        auto        states           = particle_storage.Values(particle_storage.SlotStateKey());
        for (usize slot_index {}; slot_index < states.len(); ++slot_index) {
            if (! states[slot_index].active) continue;
            auto value = MakeWPParticleConstRef(
                particle_storage, subsystem.Attributes(), particle::ParticleSlot { slot_index });
            if (value.lifetime <= 0.0f) continue;

            auto render_position =
                subsystem.InstanceState(instance_index).bounded.position + value.position;
            auto lifetime = AnimationLifetime(value, subsystem.AnimationSpec());
            std::fill(data.begin(), data.begin() + one_size.to_primitive(), 0.0f);
            write3(position, render_position[0], render_position[1], render_position[2]);
            write4(texcoord,
                   value.rotation[0],
                   value.rotation[1],
                   value.rotation[2],
                   value.size * 0.5f);
            write4(color, value.color[0], value.color[1], value.color[2], value.alpha);
            if (option.thick_format) {
                write4(velocity, value.velocity[0], value.velocity[1], value.velocity[2], lifetime);
            }
            vertices.SetVertexs(output_index++,
                                std::span<const float> { data.data(), one_size.to_primitive() });
        }
    }
    return output_index;
}

auto GenRopeParticleSegments(const WPParticleConstRef& value, const WPTrailHistoryAttribute& trails,
                             WPGOption option, SceneVertexArray& vertices, usize output_index)
    -> usize {
    auto state = trails.State(value.slot);
    if (state.len < usize(2)) return usize();

    auto                   one_size = vertices.OneSize();
    rstd::array<float, 32> data {};
    usize                  emitted {};
    auto                   size         = value.size * 0.5f;
    auto                   trail_length = static_cast<float>(state.len.to_primitive());

    for (usize sample_index { 1 }; sample_index < state.len; ++sample_index) {
        auto previous = trails.At(value.slot, sample_index - usize(1));
        auto current  = trails.At(value.slot, sample_index);
        auto start_control =
            sample_index >= usize(2) ? trails.At(value.slot, sample_index - usize(2)) : previous;
        auto  end_control    = sample_index + usize(1) < state.len
                                   ? trails.At(value.slot, sample_index + usize(1))
                                   : current;
        auto  trail_position = static_cast<float>((sample_index - usize(1)).to_primitive());
        usize offset {};

        data[offset++] = previous[0];
        data[offset++] = previous[1];
        data[offset++] = previous[2];
        data[offset++] = size;
        data[offset++] = current[0];
        data[offset++] = current[1];
        data[offset++] = current[2];
        data[offset++] = trail_length;
        data[offset++] = start_control[0];
        data[offset++] = start_control[1];
        data[offset++] = start_control[2];
        data[offset++] = trail_position;
        data[offset++] = end_control[0];
        data[offset++] = end_control[1];
        data[offset++] = end_control[2];
        data[offset++] = option.thick_format ? size : 0.0f;
        if (option.thick_format) {
            data[offset++] = value.color[0];
            data[offset++] = value.color[1];
            data[offset++] = value.color[2];
            data[offset++] = value.alpha;
        }
        data[offset++] = value.color[0];
        data[offset++] = value.color[1];
        data[offset++] = value.color[2];
        data[offset++] = value.alpha;

        rstd_assert(offset == one_size);
        vertices.SetVertexs(output_index + emitted,
                            std::span<const float> { data.data(), one_size.to_primitive() });
        ++emitted;
    }
    return emitted;
}

auto GenRopeParticleData(particle::ParticleExtractContext& context,
                         const WPParticleSubSystem& subsystem, WPGOption option,
                         SceneVertexArray& vertices) -> usize {
    auto trail_key = subsystem.TrailKey();
    if (trail_key.is_none()) return usize();

    usize output_count {};
    for (usize instance_index {}; instance_index < context.instances.len(); ++instance_index) {
        if (subsystem.InstanceState(instance_index).no_live_particle) continue;
        const auto& particle_storage = context.instances[instance_index]->Storage();
        auto        states           = particle_storage.Values(particle_storage.SlotStateKey());
        auto        trails           = particle_storage.AttributeRef(*trail_key);
        for (usize slot_index {}; slot_index < states.len(); ++slot_index) {
            if (! states[slot_index].active) continue;
            auto value = MakeWPParticleConstRef(
                particle_storage, subsystem.Attributes(), particle::ParticleSlot { slot_index });
            if (value.lifetime <= 0.0f) continue;
            output_count += GenRopeParticleSegments(value, *trails, option, vertices, output_count);
        }
    }
    return output_count;
}

void UpdateIndexArray(u32 first, usize count, SceneIndexArray& indices) noexcept {
    constexpr usize                single_size { 6 };
    rstd::uint32_t                 current = first.to_primitive() * 4U;
    rstd::array<rstd::uint32_t, 6> values {
        current, current + 1U, current + 3U, current + 1U, current + 2U, current + 3U,
    };
    for (usize index = rstd::as_cast<usize>(first); index < count; ++index) {
        indices.Assign(index * single_size, values);
        for (auto& value : values) value += 4U;
    }
}

} // namespace

void WPParticleRawGenerator::Extract(particle::ParticleExtractContext& context) {
    auto&     mesh     = m_subsystem->Mesh();
    auto&     vertices = mesh.GetVertexArray(usize());
    WPGOption option { .thick_format = vertices.GetOption(WE_CB_THICK_FORMAT) };

    if (vertices.GetOption(WE_PRENDER_ROPE)) {
        vertices.ResetSize();
        (void)GenRopeParticleData(context, *m_subsystem, option, vertices);
        return;
    }
    if (mesh.Primitive() == MeshPrimitive::POINT) {
        vertices.ResetSize();
        (void)GenParticlePointData(context, *m_subsystem, option, vertices);
        return;
    }

    auto  particle_count = GenParticleData(context, *m_subsystem, option, vertices);
    auto& indices        = mesh.GetIndexArray(usize());
    auto  index_count    = rstd::as_cast<u32>(indices.DataCount() / usize(6));
    if (particle_count > rstd::as_cast<usize>(index_count)) {
        UpdateIndexArray(index_count, particle_count, indices);
    }
    indices.SetRenderDataCount(particle_count * usize(6));
}
