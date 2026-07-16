module;

#include <rstd/macro.hpp>

export module wescene.vulkan_render:resource;
import wescene.core;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.types;
export import wescene.resource;
export import wescene.resource_registry;
import wescene.vulkan;
import wescene.scene;

export namespace owe::vulkan
{

class ShaderReflectionCache;

using resource_registry::FramebufferCacheDiagnostics;
using resource_registry::FramebufferResourceResult;
using resource_registry::FramebufferResourceSystem;
using resource_registry::PipelineCacheDiagnostics;
using resource_registry::PipelineResourceEntry;
using resource_registry::PipelineResourceResult;
using resource_registry::PipelineResourceSystem;
using resource_registry::PipelineRetireQueue;

using resource::SetTextureRequestIfChanged;
using resource::TextureBindingRequest;
using resource::TextureDefinition;
using resource::TextureDefinitionId;
using resource::TextureLifetimeClass;
using resource::TextureRequest;
using resource::TextureRequestKind;
using resource::TextureUsage;

inline bool SameTextureSample(const TextureSample& lhs, const TextureSample& rhs) {
    return lhs.wrapS == rhs.wrapS && lhs.wrapT == rhs.wrapT && lhs.magFilter == rhs.magFilter &&
           lhs.minFilter == rhs.minFilter;
}

inline bool SameTextureKey(const TextureKey& lhs, const TextureKey& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height && lhs.usage == rhs.usage &&
           lhs.format == rhs.format && SameTextureSample(lhs.sample, rhs.sample) &&
           lhs.mipmap_level == rhs.mipmap_level && lhs.samples == rhs.samples;
}

using resource::SameTextureBindingRequest;
using resource::SameTextureRequest;

inline void WriteTextureSampleIdentity(PipelineKeyWriter& writer, const TextureSample& sample) {
    WritePipelineScalar(writer, sample.wrapS);
    WritePipelineScalar(writer, sample.wrapT);
    WritePipelineScalar(writer, sample.magFilter);
    WritePipelineScalar(writer, sample.minFilter);
}

inline void WriteTextureKeyIdentity(PipelineKeyWriter& writer, const TextureKey& key) {
    writer.writeU32(static_cast<std::uint32_t>(key.width));
    writer.writeU32(static_cast<std::uint32_t>(key.height));
    WritePipelineScalar(writer, key.usage);
    WritePipelineScalar(writer, key.format);
    WriteTextureSampleIdentity(writer, key.sample);
    writer.writeU32(key.mipmap_level);
    WritePipelineScalar(writer, key.samples);
}

inline void WriteTextureDefinitionIdIdentity(PipelineKeyWriter&         writer,
                                             const TextureDefinitionId& id) {
    writer.writeU64(static_cast<std::uint64_t>(id.index));
    writer.writeU64(static_cast<std::uint64_t>(id.generation));
}

inline void WriteTextureDefinitionIdentity(PipelineKeyWriter&       writer,
                                           const TextureDefinition& definition) {
    writer.writeU32(static_cast<std::uint32_t>(definition.width));
    writer.writeU32(static_cast<std::uint32_t>(definition.height));
    WritePipelineScalar(writer, definition.usage);
    WritePipelineScalar(writer, definition.format);
    WriteTextureSampleIdentity(writer, definition.sample);
    writer.writeU32(definition.mip_levels);
    writer.writeU32(definition.samples);
}

inline void WriteTextureRequestIdentity(PipelineKeyWriter& writer, const TextureRequest& request) {
    WritePipelineScalar(writer, request.kind);
    writer.writeString(rstd::cppstd::as_string_view(request.name.as_str()));
    writer.writeBool(request.source.is_some());
    if (request.source.is_some()) {
        WriteTextureDefinitionIdIdentity(writer, *request.source);
    }
    writer.writeBool(request.definition.is_some());
    if (request.definition.is_some()) WriteTextureDefinitionIdentity(writer, *request.definition);
    WritePipelineScalar(writer, request.lifetime);
}

inline void WriteImageParametersIdentity(PipelineKeyWriter& writer, const ImageParameters& image) {
    writer.writeU64(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(image.handle)));
    writer.writeU64(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(image.view)));
    writer.writeU32(image.extent.width);
    writer.writeU32(image.extent.height);
    writer.writeU32(image.extent.depth);
    writer.writeU32(image.mipmap_level);
    writer.writeU64(image.generation);
}

inline FramebufferAttachmentIdentity
MakeFramebufferAttachmentIdentity(const TextureRequest& request, const ImageParameters& image) {
    PipelineKeyWriter writer;
    writer.writeString("framebuffer-attachment-v1");
    WriteTextureRequestIdentity(writer, request);
    WriteImageParametersIdentity(writer, image);
    return ToFramebufferAttachmentIdentity(std::move(writer).finish());
}

inline FramebufferAttachmentIdentity
MakeFramebufferAttachmentIdentity(const ImageParameters& image) {
    PipelineKeyWriter writer;
    writer.writeString("framebuffer-attachment-image-v1");
    WriteImageParametersIdentity(writer, image);
    return ToFramebufferAttachmentIdentity(std::move(writer).finish());
}

inline FramebufferAttachmentDesc MakeFramebufferAttachment(const TextureRequest&  request,
                                                           const ImageParameters& image) {
    return FramebufferAttachmentDesc {
        .view     = image.view,
        .identity = MakeFramebufferAttachmentIdentity(request, image),
    };
}

inline FramebufferAttachmentDesc MakeFramebufferAttachment(const ImageParameters& image) {
    return FramebufferAttachmentDesc {
        .view     = image.view,
        .identity = MakeFramebufferAttachmentIdentity(image),
    };
}

inline VkSampleCountFlagBits TextureSampleCount(unsigned sample_count) {
    switch (sample_count) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

inline unsigned TextureSampleCountValue(VkSampleCountFlagBits sample_count) {
    return static_cast<unsigned>(sample_count);
}

inline TextureDefinition RenderTargetTextureDefinition(owe::SceneRenderTarget rt) {
    return TextureDefinition {
        .width      = rt.width,
        .height     = rt.height,
        .usage      = TextureUsage::Color,
        .format     = owe::TextureFormat::RGBA8,
        .sample     = rt.sample,
        .mip_levels = rt.mipmap_level,
    };
}

inline TextureDefinition RenderTargetTextureDefinitionNoMip(owe::SceneRenderTarget rt) {
    return TextureDefinition {
        .width  = rt.width,
        .height = rt.height,
        .usage  = TextureUsage::Color,
        .format = owe::TextureFormat::RGBA8,
        .sample = rt.sample,
    };
}

inline TextureDefinition MsaaTextureDefinition(owe::SceneRenderTarget rt,
                                               VkSampleCountFlagBits  samples) {
    auto definition    = RenderTargetTextureDefinition(rt);
    definition.samples = TextureSampleCountValue(samples);
    return definition;
}

inline TextureDefinition DepthTextureDefinition(owe::SceneRenderTarget rt) {
    return TextureDefinition {
        .width      = rt.width,
        .height     = rt.height,
        .usage      = TextureUsage::Depth,
        .format     = owe::TextureFormat::D32F,
        .sample     = rt.sample,
        .mip_levels = 1,
        .samples    = rt.sample_count,
    };
}

inline TextureKey ToTextureKey(const TextureDefinition& definition) {
    return TextureKey {
        .width        = definition.width,
        .height       = definition.height,
        .usage        = definition.usage == TextureUsage::Depth ? TexUsage::DEPTH : TexUsage::COLOR,
        .format       = definition.format,
        .sample       = definition.sample,
        .mipmap_level = definition.mip_levels,
        .samples      = TextureSampleCount(definition.samples),
    };
}

inline TextureRequest
MakeImportedTextureRequest(std::string_view                   name,
                           std::optional<RenderTextureDescId> texture = std::nullopt) {
    auto source = texture.has_value()
                      ? rstd::Some(TextureDefinitionId { .index      = texture->index,
                                                         .generation = texture->generation })
                      : rstd::None<TextureDefinitionId>();
    return TextureRequest {
        .kind   = TextureRequestKind::Imported,
        .name   = rstd::string::String::make(rstd::cppstd::as_str(name)),
        .source = rstd::move(source),
    };
}

inline TextureRequest MakeRenderTargetTextureRequest(std::string_view         name,
                                                     const SceneRenderTarget& rt) {
    return TextureRequest {
        .kind       = TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str(name)),
        .definition = rstd::Some(RenderTargetTextureDefinition(rt)),
        .lifetime =
            rt.allowReuse ? TextureLifetimeClass::FrameLocal : TextureLifetimeClass::Retained,
    };
}

inline TextureRequest MakeRenderTargetNoMipTextureRequest(std::string_view         name,
                                                          const SceneRenderTarget& rt) {
    return TextureRequest {
        .kind       = TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str(name)),
        .definition = rstd::Some(RenderTargetTextureDefinitionNoMip(rt)),
        .lifetime =
            rt.allowReuse ? TextureLifetimeClass::FrameLocal : TextureLifetimeClass::Retained,
    };
}

inline TextureRequest MakeMsaaTextureRequest(std::string_view name, const SceneRenderTarget& rt,
                                             VkSampleCountFlagBits samples) {
    return TextureRequest {
        .kind       = TextureRequestKind::RenderTargetMsaa,
        .name       = rstd::string::String::make(rstd::cppstd::as_str(name)),
        .definition = rstd::Some(MsaaTextureDefinition(rt, samples)),
        .lifetime   = TextureLifetimeClass::Dedicated,
    };
}

inline TextureRequest MakeDepthTextureRequest(std::string_view name, const SceneRenderTarget& rt) {
    return TextureRequest {
        .kind       = TextureRequestKind::DepthAttachment,
        .name       = rstd::string::String::make(rstd::cppstd::as_str(name)),
        .definition = rstd::Some(DepthTextureDefinition(rt)),
        .lifetime =
            rt.allowReuse ? TextureLifetimeClass::FrameLocal : TextureLifetimeClass::Retained,
    };
}

inline std::optional<ImageParameters> QueryTextureRequest(TextureCache&         textures,
                                                          const TextureRequest& request) {
    if (request.definition.is_none()) return std::nullopt;
    auto name = rstd::cppstd::as_string_view(request.name.as_str());
    return textures.Query(name,
                          ToTextureKey(*request.definition),
                          request.lifetime != TextureLifetimeClass::FrameLocal);
}

inline std::optional<std::string>
ResolveImportedTextureName(const RenderSceneSnapshot& render_scene, const TextureRequest& request) {
    if (request.kind != TextureRequestKind::Imported) return std::nullopt;
    auto catalog  = rstd::dyn<resource::TextureCatalog>::from_ref(render_scene);
    auto resolved = rstd::None<TextureRequest>();
    if (request.source.is_some()) {
        resolved = catalog->ResolveTexture(*request.source);
    }
    if (resolved.is_none()) {
        resolved = catalog->FindTexture(request.name.as_str());
    }
    if (resolved.is_none()) return std::nullopt;
    return rstd::cppstd::to_string(resolved->name.as_str());
}

class SnapshotImportedTextureProvider {
public:
    SnapshotImportedTextureProvider(const RenderSceneSnapshot& render_scene,
                                    IImageParser*              image_parser)
        : m_catalog(rstd::dyn<resource::TextureCatalog>::from_ref(render_scene)),
          m_image_parser(
              image_parser == nullptr
                  ? rstd::None<rstd::mut_ref<IImageParser>>()
                  : rstd::Some(rstd::mut_ref<IImageParser>::from_raw_parts(image_parser))) {}

    auto LoadTexture(const TextureRequest& request)
        -> rstd::Result<rstd::mut_ref<Image>, resource::ResourceError> {
        auto name  = ResolveName(request);
        auto found = m_loaded_content.find(name);
        if (found != m_loaded_content.end()) {
            return rstd::Ok(rstd::mut_ref<Image>::from_raw_parts(rstd::addressof(*found->second)));
        }
        if (m_image_parser.is_none()) {
            return rstd::Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingContent,
                .message = rstd::format("texture parser unavailable for {}", request.name.as_str()),
            });
        }
        auto image = (*m_image_parser)->Parse(name);
        if (! image) {
            return rstd::Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingContent,
                .message = rstd::format("parse texture {} failed", request.name.as_str()),
            });
        }
        auto stored = m_loaded_content.emplace(rstd::move(name), rstd::move(image)).first;
        return rstd::Ok(rstd::mut_ref<Image>::from_raw_parts(rstd::addressof(*stored->second)));
    }

private:
    std::string ResolveName(const TextureRequest& request) const {
        auto resolved = rstd::None<TextureRequest>();
        if (request.source.is_some()) resolved = m_catalog->ResolveTexture(*request.source);
        if (resolved.is_none()) resolved = m_catalog->FindTexture(request.name.as_str());
        if (resolved.is_some()) return rstd::cppstd::to_string(resolved->name.as_str());
        return rstd::cppstd::to_string(request.name.as_str());
    }

    mutable rstd::ref<rstd::dyn<resource::TextureCatalog>>  m_catalog;
    rstd::Option<rstd::mut_ref<IImageParser>>               m_image_parser;
    std::unordered_map<std::string, std::shared_ptr<Image>> m_loaded_content;
};

struct RenderingResources {
    vvk::CommandBuffer command;

    vvk::Semaphore sem_swap_wait_image;
    vvk::Semaphore sem_export;
    vvk::Semaphore sem_upload;
    vvk::Fence     fence_frame;

    // Static vertex/index buffers are owned by the resource registries;
    // only the per-rebuild dyn_buf lives here.
    rstd::Option<rstd::mut_ref<StagingBuffer>>         dyn_buf;
    rstd::Option<rstd::mut_ref<ShaderReflectionCache>> shader_reflection_cache;
    resource_registry::ResourceRegistries              resource_registries;
    resource_registry::PreparedResourceTable           prepared_resources;
};

} // namespace owe::vulkan

export namespace rstd
{

template<>
struct Impl<owe::resource::TextureCatalog, owe::RenderSceneSnapshot>
    : ImplBase<owe::RenderSceneSnapshot> {
    auto ResolveTexture(owe::resource::TextureDefinitionId id) const
        -> Option<owe::resource::TextureRequest> {
        auto record = this->self().textureDesc(
            owe::RenderTextureDescId { .index = id.index, .generation = id.generation });
        if (record == nullptr) return None();
        auto name = record->desc.url.empty() ? std::string_view(record->key)
                                             : std::string_view(record->desc.url);
        return Some(owe::vulkan::MakeImportedTextureRequest(
            name, owe::RenderTextureDescId { .index = id.index, .generation = id.generation }));
    }

    auto FindTexture(ref<str> name) const -> Option<owe::resource::TextureRequest> {
        auto id = this->self().textureDescId(rstd::cppstd::as_string_view(name));
        if (! id.has_value()) return None();
        return ResolveTexture(owe::resource::TextureDefinitionId { .index      = id->index,
                                                                   .generation = id->generation });
    }
};

template<>
struct Impl<owe::resource::TextureContentProvider, owe::vulkan::SnapshotImportedTextureProvider>
    : ImplBase<owe::vulkan::SnapshotImportedTextureProvider> {
    auto LoadTexture(const owe::resource::TextureRequest& request)
        -> Result<mut_ref<owe::Image>, owe::resource::ResourceError> {
        return this->self().LoadTexture(request);
    }
};

} // namespace rstd
