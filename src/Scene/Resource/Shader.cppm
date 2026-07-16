export module wescene.resource:shader;
import rstd;
import wescene.types;
import :handle;

export namespace owe::resource
{

using namespace rstd::prelude;

struct ShaderDefinitionId {
    u32 index { numeric_limits<u32>::max() };
    u64 generation { 0 };

    bool Valid() const noexcept { return index != numeric_limits<u32>::max() && generation != 0; }

    friend bool operator==(const ShaderDefinitionId&, const ShaderDefinitionId&) = default;
};

struct ShaderRequest {
    String             name;
    ShaderDefinitionId source;
    u64                content_version { 1 };

    auto clone() const -> ShaderRequest {
        return ShaderRequest {
            .name            = name.clone(),
            .source          = source,
            .content_version = content_version,
        };
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

struct ShaderArtifact {
    ShaderDefinitionId                  source;
    u64                                 content_version { 1 };
    rstd::vec::Vec<ShaderArtifactStage> stages;

    auto clone() const -> ShaderArtifact {
        auto cloned = rstd::vec::Vec<ShaderArtifactStage>::with_capacity(stages.len());
        for (const auto& stage : stages) cloned.push(stage.clone());
        return ShaderArtifact {
            .source          = source,
            .content_version = content_version,
            .stages          = rstd::move(cloned),
        };
    }
};

} // namespace owe::resource
