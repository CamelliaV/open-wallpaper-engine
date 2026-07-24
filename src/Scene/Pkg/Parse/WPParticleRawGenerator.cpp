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
    ref<str> name) noexcept -> AttrSlot {
    auto found = attributes.find(rstd::cppstd::as_string_view(name));
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
            vertices.SetVertexs(output_index++ * usize(4),
                                rstd::slice<float>::from_raw_parts(storage.data(), total_size));
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
            for (usize index {}; index < one_size; ++index) data[index] = 0.0f;
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
                                rstd::slice<float>::from_raw_parts(data.data(), one_size));
        }
    }
    return output_index;
}

auto GenRopeParticleData(particle::ParticleExtractContext& context,
                         const WPParticleSubSystem& subsystem, WPGOption option,
                         SceneVertexArray& vertices) -> usize {
    auto           one_size          = vertices.OneSize();
    const auto     attribute_offsets = vertices.GetAttrOffsetMap();
    const AttrSlot position          = FindAttrSlot(attribute_offsets, WE_IN_POSITIONVEC4);
    const AttrSlot endpoint          = FindAttrSlot(attribute_offsets, WE_IN_TEXCOORDVEC4);
    const AttrSlot previous_point    = FindAttrSlot(attribute_offsets, WE_IN_TEXCOORDVEC4C1);
    const AttrSlot next_point        = FindAttrSlot(
        attribute_offsets, option.thick_format ? WE_IN_TEXCOORDVEC4C2 : WE_IN_TEXCOORDVEC3C2);
    const AttrSlot         color_end = FindAttrSlot(attribute_offsets, WE_IN_TEXCOORDVEC4C3);
    const AttrSlot         color     = FindAttrSlot(attribute_offsets, WE_IN_COLOR);
    rstd::array<float, 32> data {};
    usize                  output_count {};

    auto write3 = [&](AttrSlot slot, const Eigen::Vector3f& source) noexcept {
        if (! slot.enabled) return;
        data[slot.offset]            = source[0];
        data[slot.offset + usize(1)] = source[1];
        data[slot.offset + usize(2)] = source[2];
    };
    auto write4 = [&](AttrSlot slot, const Eigen::Vector3f& source, float w) noexcept {
        if (! slot.enabled) return;
        write3(slot, source);
        data[slot.offset + usize(3)] = w;
    };
    auto write_color = [&](AttrSlot slot, const WPParticleConstRef& value) noexcept {
        if (! slot.enabled) return;
        data[slot.offset]            = value.color[0];
        data[slot.offset + usize(1)] = value.color[1];
        data[slot.offset + usize(2)] = value.color[2];
        data[slot.offset + usize(3)] = value.alpha;
    };

    for (usize instance_index {}; instance_index < context.instances.len(); ++instance_index) {
        if (subsystem.InstanceState(instance_index).no_live_particle) continue;
        const auto&        particle_storage = context.instances[instance_index]->Storage();
        auto               states = particle_storage.Values(particle_storage.SlotStateKey());
        std::vector<usize> slots;
        slots.reserve(states.len().to_primitive());
        for (usize slot_index {}; slot_index < states.len(); ++slot_index) {
            if (! states[slot_index].active) continue;
            auto value = MakeWPParticleConstRef(
                particle_storage, subsystem.Attributes(), particle::ParticleSlot { slot_index });
            if (value.lifetime > 0.0f) slots.push_back(slot_index);
        }
        std::sort(slots.begin(), slots.end(), [&](usize lhs, usize rhs) {
            return states[lhs].spawn_sequence < states[rhs].spawn_sequence;
        });
        if (slots.size() < 2) continue;

        auto particle = [&](usize index) {
            return MakeWPParticleConstRef(particle_storage,
                                          subsystem.Attributes(),
                                          particle::ParticleSlot { slots[index.to_primitive()] });
        };
        auto render_position = [&](const WPParticleConstRef& value) {
            return (subsystem.InstanceState(instance_index).bounded.position + value.position)
                .eval();
        };

        float sequence_offset {};
        if (slots.size() > 1) {
            auto newest      = particle(usize(slots.size() - 1));
            auto before      = particle(usize(slots.size() - 2));
            auto newest_age  = newest.initial.lifetime - newest.lifetime;
            auto before_age  = before.initial.lifetime - before.lifetime;
            auto emit_period = before_age - newest_age;
            if (emit_period > 1e-6f)
                sequence_offset = -std::clamp(newest_age / emit_period, 0.0f, 1.0f);
        }

        auto segment_count = usize(slots.size() - 1);
        for (usize index {}; index < segment_count; ++index) {
            auto previous_index = index == usize() ? usize() : index - usize(1);
            auto next_index     = index + usize(1);
            auto after_index    = rstd::cmp::min(index + usize(2), usize(slots.size() - 1));
            auto current        = particle(index);
            auto previous       = particle(previous_index);
            auto next           = particle(next_index);
            auto after          = particle(after_index);

            for (usize index {}; index < one_size; ++index) data[index] = 0.0f;
            write4(position, render_position(current), current.size * 0.5f);
            write4(
                endpoint, render_position(next), static_cast<float>(segment_count.to_primitive()));
            write4(previous_point,
                   render_position(previous),
                   static_cast<float>(index.to_primitive()) + sequence_offset);
            if (option.thick_format)
                write4(next_point, render_position(after), next.size * 0.5f);
            else
                write3(next_point, render_position(after));
            write_color(color_end, next);
            write_color(color, current);
            vertices.SetVertexs(output_count++,
                                rstd::slice<float>::from_raw_parts(data.data(), one_size));
        }
    }
    return output_count;
}

auto GenRopeTrailSegments(const WPParticleConstRef& value, const Eigen::Vector3f& instance_position,
                          const WPTrailHistoryAttribute& trails, WPGOption option,
                          SceneVertexArray& vertices, usize output_index) -> usize {
    auto state = trails.State(value.slot);
    if (state.len == usize()) return usize();

    auto           one_size           = vertices.OneSize();
    const auto     attribute_offsets  = vertices.GetAttrOffsetMap();
    const AttrSlot position           = FindAttrSlot(attribute_offsets, WE_IN_POSITIONVEC4);
    const AttrSlot endpoint           = FindAttrSlot(attribute_offsets, WE_IN_TEXCOORDVEC4);
    const AttrSlot start_control_slot = FindAttrSlot(attribute_offsets, WE_IN_TEXCOORDVEC4C1);
    const AttrSlot end_control_slot   = FindAttrSlot(
        attribute_offsets, option.thick_format ? WE_IN_TEXCOORDVEC4C2 : WE_IN_TEXCOORDVEC3C2);
    const AttrSlot         color_end = FindAttrSlot(attribute_offsets, WE_IN_TEXCOORDVEC4C3);
    const AttrSlot         color     = FindAttrSlot(attribute_offsets, WE_IN_COLOR);
    rstd::array<float, 32> data {};
    usize                  emitted {};
    auto                   size         = value.size * 0.5f;
    auto                   trail_length = static_cast<float>(state.sample_count.to_primitive());

    auto write3 = [&](AttrSlot slot, const Eigen::Vector3f& source) noexcept {
        if (! slot.enabled) return;
        data[slot.offset]            = source[0];
        data[slot.offset + usize(1)] = source[1];
        data[slot.offset + usize(2)] = source[2];
    };
    auto write4 = [&](AttrSlot slot, const Eigen::Vector3f& source, float w) noexcept {
        if (! slot.enabled) return;
        write3(slot, source);
        data[slot.offset + usize(3)] = w;
    };
    auto write_color = [&](AttrSlot slot) noexcept {
        if (! slot.enabled) return;
        data[slot.offset]            = value.color[0];
        data[slot.offset + usize(1)] = value.color[1];
        data[slot.offset + usize(2)] = value.color[2];
        data[slot.offset + usize(3)] = value.alpha;
    };

    auto point = [&](usize point_index) -> Eigen::Vector3f {
        if (point_index == usize()) return (value.position + instance_position).eval();
        return (trails.At(value.slot, state.len - point_index) + instance_position).eval();
    };

    for (usize sample_index {}; sample_index < state.len; ++sample_index) {
        auto previous       = point(sample_index);
        auto current        = point(sample_index + usize(1));
        auto start_control  = point(sample_index == usize() ? usize() : sample_index - usize(1));
        auto end_control    = point(rstd::cmp::min(sample_index + usize(2), state.len));
        auto trail_position = static_cast<float>(sample_index.to_primitive());
        for (usize index {}; index < one_size; ++index) data[index] = 0.0f;
        write4(position, previous, size);
        write4(endpoint, current, trail_length);
        write4(start_control_slot, start_control, trail_position);
        if (option.thick_format)
            write4(end_control_slot, end_control, size);
        else
            write3(end_control_slot, end_control);
        write_color(color_end);
        write_color(color);
        vertices.SetVertexs(output_index + emitted,
                            rstd::slice<float>::from_raw_parts(data.data(), one_size));
        ++emitted;
    }
    return emitted;
}

auto GenRopeTrailData(particle::ParticleExtractContext& context,
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
            output_count +=
                GenRopeTrailSegments(value,
                                     subsystem.InstanceState(instance_index).bounded.position,
                                     *trails,
                                     option,
                                     vertices,
                                     output_count);
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
        indices.Assign(index * single_size, values.as_slice());
        for (auto& value : values) value += 4U;
    }
}

} // namespace

void WPParticleRawGenerator::Extract(particle::ParticleExtractContext& context) {
    auto&     mesh     = m_subsystem->Mesh();
    auto&     vertices = mesh.GetVertexArray(usize());
    WPGOption option {
        .thick_format = vertices.GetOption(rstd::cppstd::as_string_view(WE_CB_THICK_FORMAT)),
    };

    if (vertices.GetOption(rstd::cppstd::as_string_view(WE_PRENDER_ROPE))) {
        vertices.ResetSize();
        (void)GenRopeParticleData(context, *m_subsystem, option, vertices);
        return;
    }
    if (vertices.GetOption(rstd::cppstd::as_string_view(WE_PRENDER_ROPE_TRAIL))) {
        vertices.ResetSize();
        (void)GenRopeTrailData(context, *m_subsystem, option, vertices);
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
