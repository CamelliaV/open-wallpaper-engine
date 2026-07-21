export module wescene.resource:shader;
import rstd;
import wescene.types;
import :handle;

export namespace owe::resource
{

using namespace rstd::prelude;

struct ShaderDefinitionId {
    u32 index { u32::MAX };
    u64 generation {};

    bool Valid() const noexcept { return index != u32::MAX && generation != u64(); }

    friend bool operator==(const ShaderDefinitionId&, const ShaderDefinitionId&) = default;
};

struct ShaderRequest {
    String             name;
    ShaderDefinitionId source;
    u64                content_version { u64(1) };

    auto clone() const -> ShaderRequest {
        return ShaderRequest {
            .name            = name.clone(),
            .source          = source,
            .content_version = content_version,
        };
    }

    friend bool operator==(const ShaderRequest&, const ShaderRequest&) = default;
};

struct ShaderRequestHasher {
    rstd::hash::RandomState state;

    auto operator()(const ShaderRequest& request) const noexcept -> u64 {
        auto          seed = state(request.name);
        constexpr u64 mix_constant(0x9e3779b97f4a7c15ULL);
        auto          mix = [&](u64 value) {
            seed ^= value.wrapping_add(mix_constant)
                        .wrapping_add(seed.wrapping_shl(u64(6)))
                        .wrapping_add(seed >> u64(2));
        };
        mix(state(request.source.index));
        mix(state(request.source.generation));
        mix(state(request.content_version));
        return seed;
    }
};

struct ShaderArtifactStage {
    ShaderType          stage { ShaderType::VERTEX };
    String              entry_point;
    rstd::vec::Vec<u32> code;

    auto clone() const -> ShaderArtifactStage {
        auto cloned = rstd::vec::Vec<u32>::with_capacity(code.len());
        for (auto word : code) cloned.push(u32(word));
        return ShaderArtifactStage {
            .stage       = stage,
            .entry_point = entry_point.clone(),
            .code        = rstd::move(cloned),
        };
    }
};

struct ShaderArtifactUniformMember {
    String name;
    u32    offset {};
    usize  size {};
    usize  count { usize(1) };

    auto clone() const -> ShaderArtifactUniformMember {
        return ShaderArtifactUniformMember {
            .name   = name.clone(),
            .offset = offset,
            .size   = size,
            .count  = count,
        };
    }
};

struct ShaderArtifactUniformBlock {
    String                                      name;
    usize                                       size {};
    rstd::vec::Vec<ShaderArtifactUniformMember> members;

    auto clone() const -> ShaderArtifactUniformBlock {
        auto cloned = rstd::vec::Vec<ShaderArtifactUniformMember>::with_capacity(members.len());
        for (const auto& member : members) cloned.push(member.clone());
        return ShaderArtifactUniformBlock {
            .name    = name.clone(),
            .size    = size,
            .members = rstd::move(cloned),
        };
    }
};

struct ShaderArtifactDescriptorBinding {
    String name;
    u32    binding {};
    u32    descriptor_type {};
    u32    descriptor_count { u32(1) };
    u32    stage_flags {};

    auto clone() const -> ShaderArtifactDescriptorBinding {
        return ShaderArtifactDescriptorBinding {
            .name             = name.clone(),
            .binding          = binding,
            .descriptor_type  = descriptor_type,
            .descriptor_count = descriptor_count,
            .stage_flags      = stage_flags,
        };
    }
};

struct ShaderArtifactVertexInput {
    String name;
    u32    location {};
    u32    format {};

    auto clone() const -> ShaderArtifactVertexInput {
        return ShaderArtifactVertexInput {
            .name     = name.clone(),
            .location = location,
            .format   = format,
        };
    }
};

struct ShaderArtifact {
    ShaderDefinitionId                              source;
    u64                                             content_version { u64(1) };
    rstd::vec::Vec<ShaderArtifactStage>             stages;
    rstd::vec::Vec<ShaderArtifactUniformBlock>      uniform_blocks;
    rstd::vec::Vec<ShaderArtifactDescriptorBinding> descriptor_bindings;
    rstd::vec::Vec<ShaderArtifactVertexInput>       vertex_inputs;

    auto clone() const -> ShaderArtifact {
        auto cloned_stages = rstd::vec::Vec<ShaderArtifactStage>::with_capacity(stages.len());
        for (const auto& stage : stages) cloned_stages.push(stage.clone());
        auto cloned_blocks =
            rstd::vec::Vec<ShaderArtifactUniformBlock>::with_capacity(uniform_blocks.len());
        for (const auto& block : uniform_blocks) cloned_blocks.push(block.clone());
        auto cloned_bindings = rstd::vec::Vec<ShaderArtifactDescriptorBinding>::with_capacity(
            descriptor_bindings.len());
        for (const auto& binding : descriptor_bindings) cloned_bindings.push(binding.clone());
        auto cloned_inputs =
            rstd::vec::Vec<ShaderArtifactVertexInput>::with_capacity(vertex_inputs.len());
        for (const auto& input : vertex_inputs) cloned_inputs.push(input.clone());
        return ShaderArtifact {
            .source              = source,
            .content_version     = content_version,
            .stages              = rstd::move(cloned_stages),
            .uniform_blocks      = rstd::move(cloned_blocks),
            .descriptor_bindings = rstd::move(cloned_bindings),
            .vertex_inputs       = rstd::move(cloned_inputs),
        };
    }
};

} // namespace owe::resource
