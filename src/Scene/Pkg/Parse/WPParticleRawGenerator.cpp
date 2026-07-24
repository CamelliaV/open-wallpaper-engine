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

struct WPExtractParticle {
    particle::ParticleSlot             slot;
    const particle::ParticleSlotState& state;
    const Eigen::Vector3f&             position;
    const Eigen::Vector3f&             velocity;
    const Eigen::Vector3f&             rotation;
    const Eigen::Vector3f&             color;
    const float&                       alpha;
    const float&                       size;
    const float&                       lifetime;
    const float&                       random;
    const float&                       initial_lifetime;
};

struct WPExtractInstance {
    slice<particle::ParticleSlotState>   states;
    slice<Eigen::Vector3f>               positions;
    slice<Eigen::Vector3f>               velocities;
    slice<Eigen::Vector3f>               rotations;
    slice<Eigen::Vector3f>               colors;
    slice<float>                         alphas;
    slice<float>                         sizes;
    slice<float>                         lifetimes;
    slice<float>                         randoms;
    slice<float>                         initial_lifetimes;
    slice<particle::ParticleSlot>        slots;
    Option<ref<WPTrailHistoryAttribute>> trail;
    usize                                instance_index {};

    auto Particle(particle::ParticleSlot slot) const -> WPExtractParticle {
        auto index = slot.index;
        return {
            .slot             = slot,
            .state            = states[index],
            .position         = positions[index],
            .velocity         = velocities[index],
            .rotation         = rotations[index],
            .color            = colors[index],
            .alpha            = alphas[index],
            .size             = sizes[index],
            .lifetime         = lifetimes[index],
            .random           = randoms[index],
            .initial_lifetime = initial_lifetimes[index],
        };
    }
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

auto AnimationLifetime(const WPExtractParticle& value, WPParticleAnimationSpec animation) noexcept
    -> float {
    if (value.lifetime <= 0.0f) return 0.0f;
    switch (animation.mode) {
    case WPParticleAnimationMode::RANDOMONE:
        return std::clamp(value.random, 0.0f, std::nextafter(1.0f, 0.0f));
    case WPParticleAnimationMode::SEQUENCE:
        if (value.initial_lifetime == 0.0f) return 0.0f;
        return (1.0f - (value.lifetime / value.initial_lifetime)) * animation.sequence_multiplier;
    }
    return 0.0f;
}

auto GenParticlePointData(slice<WPExtractInstance> instances, const WPParticleSubSystem& subsystem,
                          WPGOption option, SceneVertexArray& vertices) noexcept -> usize {
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

    for (const auto& instance : instances) {
        if (subsystem.InstanceState(instance.instance_index).no_live_particle) continue;
        for (auto slot : instance.slots) {
            auto value = instance.Particle(slot);
            if (value.lifetime <= 0.0f) continue;

            auto render_position =
                subsystem.RenderPosition(instance.instance_index, value.position);
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

auto GenRopeParticleData(slice<WPExtractInstance> instances, const WPParticleSubSystem& subsystem,
                         WPGOption option, SceneVertexArray& vertices) -> usize {
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
    auto write_color = [&](AttrSlot slot, const WPExtractParticle& value) noexcept {
        if (! slot.enabled) return;
        data[slot.offset]            = value.color[0];
        data[slot.offset + usize(1)] = value.color[1];
        data[slot.offset + usize(2)] = value.color[2];
        data[slot.offset + usize(3)] = value.alpha;
    };

    for (const auto& instance : instances) {
        if (subsystem.InstanceState(instance.instance_index).no_live_particle) continue;
        std::vector<usize> slots;
        slots.reserve(instance.slots.len().to_primitive());
        for (auto slot : instance.slots) {
            if (instance.lifetimes[slot.index] > 0.0f) slots.push_back(slot.index);
        }
        std::sort(slots.begin(), slots.end(), [&](usize lhs, usize rhs) {
            return instance.states[lhs].spawn_sequence < instance.states[rhs].spawn_sequence;
        });
        if (slots.size() < 2) continue;

        auto particle = [&](usize index) {
            return instance.Particle(particle::ParticleSlot { slots[index.to_primitive()] });
        };
        auto render_position = [&](const WPExtractParticle& value) {
            return subsystem.RenderPosition(instance.instance_index, value.position);
        };

        float sequence_offset {};
        if (slots.size() > 1) {
            auto newest      = particle(usize(slots.size() - 1));
            auto before      = particle(usize(slots.size() - 2));
            auto newest_age  = newest.initial_lifetime - newest.lifetime;
            auto before_age  = before.initial_lifetime - before.lifetime;
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

auto GenRopeTrailSegments(const WPExtractParticle& value, const WPParticleSubSystem& subsystem,
                          usize instance_index, const WPTrailHistoryAttribute& trails,
                          WPGOption option, SceneVertexArray& vertices, usize output_index)
    -> usize {
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
        if (point_index == usize()) return subsystem.RenderPosition(instance_index, value.position);
        return subsystem.RenderPosition(instance_index,
                                        trails.At(value.slot, state.len - point_index));
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

auto GenRopeTrailData(slice<WPExtractInstance> instances, const WPParticleSubSystem& subsystem,
                      WPGOption option, SceneVertexArray& vertices) -> usize {
    usize output_count {};
    for (const auto& instance : instances) {
        if (subsystem.InstanceState(instance.instance_index).no_live_particle) continue;
        if (instance.trail.is_none()) continue;
        auto trails = *instance.trail;
        for (auto slot : instance.slots) {
            auto value = instance.Particle(slot);
            if (value.lifetime <= 0.0f) continue;
            output_count += GenRopeTrailSegments(
                value, subsystem, instance.instance_index, *trails, option, vertices, output_count);
        }
    }
    return output_count;
}

} // namespace

void WPParticleRawGenerator::Compile(particle::ParticleViewCompiler& compiler) {
    auto attributes = m_subsystem->Attributes();
    compiler.ReadBase(attributes.position);
    m_velocity         = compiler.Read(attributes.velocity);
    m_rotation         = compiler.Read(attributes.rotation);
    m_color            = compiler.Read(attributes.color);
    m_alpha            = compiler.Read(attributes.alpha);
    m_size             = compiler.Read(attributes.size);
    m_lifetime         = compiler.Read(attributes.lifetime);
    m_random           = compiler.Read(attributes.random);
    m_initial_lifetime = compiler.Read(attributes.initial_lifetime);
    auto trail_key     = m_subsystem->TrailKey();
    if (trail_key.is_some()) m_trail = compiler.ReadObject(*trail_key);
}

void WPParticleRawGenerator::Extract(particle::ParticleExtractContext& context) {
    auto instances = rstd::vec::Vec<WPExtractInstance>::with_capacity(context.instances.len());
    for (const auto& instance : context.instances) {
        Option<ref<WPTrailHistoryAttribute>> trail = None();
        if (m_trail.Valid()) trail = Some(instance.view.ReadObject(m_trail));
        instances.emplace_back(WPExtractInstance {
            .states            = instance.view.States(),
            .positions         = instance.view.Positions(),
            .velocities        = instance.view.Read(m_velocity),
            .rotations         = instance.view.Read(m_rotation),
            .colors            = instance.view.Read(m_color),
            .alphas            = instance.view.Read(m_alpha),
            .sizes             = instance.view.Read(m_size),
            .lifetimes         = instance.view.Read(m_lifetime),
            .randoms           = instance.view.Read(m_random),
            .initial_lifetimes = instance.view.Read(m_initial_lifetime),
            .slots             = instance.slots,
            .trail             = trail,
            .instance_index    = instance.instance_index,
        });
    }

    auto&     mesh     = m_subsystem->Mesh();
    auto&     vertices = mesh.GetVertexArray(usize());
    WPGOption option {
        .thick_format = vertices.GetOption(rstd::cppstd::as_string_view(WE_CB_THICK_FORMAT)),
    };

    if (vertices.GetOption(rstd::cppstd::as_string_view(WE_PRENDER_ROPE))) {
        vertices.ResetSize();
        (void)GenRopeParticleData(instances.as_slice(), *m_subsystem, option, vertices);
        return;
    }
    if (vertices.GetOption(rstd::cppstd::as_string_view(WE_PRENDER_ROPE_TRAIL))) {
        vertices.ResetSize();
        (void)GenRopeTrailData(instances.as_slice(), *m_subsystem, option, vertices);
        return;
    }
    vertices.ResetSize();
    (void)GenParticlePointData(instances.as_slice(), *m_subsystem, option, vertices);
}
