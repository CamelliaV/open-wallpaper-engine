export module wescene.resource_registry:prepared;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.vulkan;

import :texture_registry;
import :descriptor;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct PreparedTexture {
    resource::TextureUseHandle use;
    resource::TextureHandle    resource;
    resource::TextureRequest   request;
    vulkan::ImageSlotsRef      image;
    u64                        physical_generation { 0 };
    resource::ReadyToken       ready;
};

struct PreparedTextureLease {
    resource::TextureHandle resource;
    u64                     physical_generation { 0 };
};

struct PreparedDescriptorLease {
    resource::DescriptorBindingHandle handle;
    Option<vvk::DescriptorSetLease>   set;
};

struct PreparedResourceLeases {
    rstd::vec::Vec<PreparedTextureLease>    textures;
    rstd::vec::Vec<PreparedDescriptorLease> descriptors;
};

class PreparedResourceTable {
public:
    explicit PreparedResourceTable(u64 generation = 0): m_generation(generation) {}

    bool Insert(PreparedTexture texture) {
        return m_textures.insert(texture.use, rstd::move(texture)).is_none();
    }

    auto Resolve(resource::TextureUseHandle use) const -> Option<ref<PreparedTexture>> {
        return m_textures.get(use);
    }

    bool Insert(PreparedDescriptorBinding binding) {
        return m_descriptors.insert(binding.handle, rstd::move(binding)).is_none();
    }

    auto Resolve(resource::DescriptorBindingHandle handle) const
        -> Option<ref<PreparedDescriptorBinding>> {
        return m_descriptors.get(handle);
    }

    auto Generation() const noexcept -> u64 { return m_generation; }
    auto TextureCount() const noexcept -> usize { return m_textures.len(); }
    auto DescriptorCount() const noexcept -> usize { return m_descriptors.len(); }

    auto Leases() const -> PreparedResourceLeases {
        auto texture_leases = rstd::vec::Vec<PreparedTextureLease>::with_capacity(m_textures.len());
        auto textures       = m_textures.values();
        for (auto texture = textures.next(); texture.is_some(); texture = textures.next()) {
            texture_leases.push(PreparedTextureLease {
                .resource            = (**texture).resource,
                .physical_generation = (**texture).physical_generation,
            });
        }

        auto descriptor_leases =
            rstd::vec::Vec<PreparedDescriptorLease>::with_capacity(m_descriptors.len());
        auto descriptors = m_descriptors.values();
        for (auto descriptor = descriptors.next(); descriptor.is_some();
             descriptor      = descriptors.next()) {
            descriptor_leases.push(PreparedDescriptorLease {
                .handle = (**descriptor).handle,
                .set    = (**descriptor).set.is_some() ? Some((**descriptor).set->clone())
                                                       : None<vvk::DescriptorSetLease>(),
            });
        }
        return PreparedResourceLeases {
            .textures    = rstd::move(texture_leases),
            .descriptors = rstd::move(descriptor_leases),
        };
    }

private:
    using TextureMap =
        rstd::collections::HashMap<resource::TextureUseHandle, PreparedTexture,
                                   resource::ResourceHandleHasher<resource::TextureUseHandle>>;
    using DescriptorMap = rstd::collections::HashMap<
        resource::DescriptorBindingHandle, PreparedDescriptorBinding,
        resource::ResourceHandleHasher<resource::DescriptorBindingHandle>>;

    u64           m_generation { 0 };
    TextureMap    m_textures;
    DescriptorMap m_descriptors;
};

class ResourcePrepareService;

class ResourcePlanPrepareVisitor {
public:
    ResourcePlanPrepareVisitor(ResourcePrepareService& service, PreparedResourceTable& table,
                               Option<mut_ref<dyn<resource::TextureContentProvider>>> content)
        : m_service(service), m_table(table), m_content(content) {}

    auto VisitTexture(const resource::TexturePlanEntry&) -> Result<empty, resource::ResourceError>;
    auto VisitBuffer(const resource::BufferPlanEntry&) -> Result<empty, resource::ResourceError>;
    auto VisitShader(const resource::ShaderPlanEntry&) -> Result<empty, resource::ResourceError>;

private:
    ResourcePrepareService&                                m_service;
    PreparedResourceTable&                                 m_table;
    Option<mut_ref<dyn<resource::TextureContentProvider>>> m_content;
};

class ResourcePrepareService {
public:
    ResourcePrepareService(resource::TextureRegistry& textures, vulkan::TextureCache& backend)
        : m_textures(textures), m_backend(backend) {}

    auto Prepare(const resource::ResourcePlan&                          plan,
                 Option<mut_ref<dyn<resource::TextureContentProvider>>> content = None())
        -> Result<PreparedResourceTable, resource::ResourceError> {
        PreparedResourceTable      table(plan.generation);
        ResourcePlanPrepareVisitor visitor(*this, table, content);
        auto object  = rstd::dyn<resource::ResourcePlanVisitor>::from_ref(visitor);
        auto visited = resource::VisitResourcePlan(plan, object);
        if (visited.is_err()) return Err(rstd::move(visited).unwrap_err_unchecked());
        return Ok(rstd::move(table));
    }

    auto PrepareTexture(const resource::TexturePlanEntry& entry, PreparedResourceTable& table,
                        Option<mut_ref<dyn<resource::TextureContentProvider>>> content)
        -> Result<empty, resource::ResourceError> {
        auto image = Resolve(entry.request, content);
        if (image.is_err()) return Err(rstd::move(image).unwrap_err_unchecked());

        auto resolved = rstd::move(image).unwrap_unchecked();
        auto handle   = m_textures.Register(entry.request.clone());
        if (! handle.Valid()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("invalid texture request {}", entry.request.name.as_str()),
            });
        }

        auto ready_value = resolved.slots.empty() ? u64(1) : u64(resolved.getActive().generation);
        auto published =
            m_textures.Publish(handle, resolved, resource::ReadyToken { .value = ready_value });
        if (published.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("publish texture {} failed", entry.request.name.as_str()),
            });
        }

        auto physical = m_textures.Resolve(handle);
        if (physical.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("resolve texture {} failed", entry.request.name.as_str()),
            });
        }
        (void)table.Insert(PreparedTexture {
            .use                 = entry.handle,
            .resource            = handle,
            .request             = entry.request.clone(),
            .image               = (**physical).image,
            .physical_generation = *published,
            .ready               = (**physical).ready,
        });
        return Ok(empty {});
    }

private:
    auto Resolve(const resource::TextureRequest&                        request,
                 Option<mut_ref<dyn<resource::TextureContentProvider>>> content)
        -> Result<vulkan::ImageSlotsRef, resource::ResourceError> {
        auto name = rstd::cppstd::as_string_view(request.name.as_str());
        if (request.kind == resource::TextureRequestKind::Imported) {
            if (auto cached = m_backend.FindImportedTexture(name); cached.has_value()) {
                return Ok(rstd::move(*cached));
            }
            if (content.is_none()) {
                return Err(resource::ResourceError {
                    .kind = resource::ResourceErrorKind::MissingContent,
                    .message =
                        rstd::format("texture content {} unavailable", request.name.as_str()),
                });
            }
            auto loaded = (*content)->LoadTexture(request);
            if (loaded.is_err()) return Err(rstd::move(loaded).unwrap_err_unchecked());
            return Ok(m_backend.CreateTex(*rstd::move(loaded).unwrap_unchecked()));
        }

        if (request.definition.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("texture definition {} unavailable", request.name.as_str()),
            });
        }
        auto image =
            m_backend.Query(name,
                            ToTextureKey(*request.definition),
                            request.lifetime != resource::TextureLifetimeClass::FrameLocal);
        if (! image.has_value()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("create texture {} failed", request.name.as_str()),
            });
        }
        vulkan::ImageSlotsRef slots;
        slots.slots.push_back(*image);
        return Ok(rstd::move(slots));
    }

    static auto ToTextureKey(const resource::TextureDefinition& definition) -> vulkan::TextureKey {
        return vulkan::TextureKey {
            .width  = definition.width,
            .height = definition.height,
            .usage  = definition.usage == resource::TextureUsage::Depth ? vulkan::TexUsage::DEPTH
                                                                        : vulkan::TexUsage::COLOR,
            .format = definition.format,
            .sample = definition.sample,
            .mipmap_level = definition.mip_levels,
            .samples      = TextureSampleCount(definition.samples),
        };
    }

    static auto TextureSampleCount(u32 sample_count) -> VkSampleCountFlagBits {
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

    resource::TextureRegistry& m_textures;
    vulkan::TextureCache&      m_backend;
};

inline auto ResourcePlanPrepareVisitor::VisitTexture(const resource::TexturePlanEntry& entry)
    -> Result<empty, resource::ResourceError> {
    return m_service.PrepareTexture(entry, m_table, m_content);
}

inline auto ResourcePlanPrepareVisitor::VisitBuffer(const resource::BufferPlanEntry&)
    -> Result<empty, resource::ResourceError> {
    return Err(resource::ResourceError {
        .kind    = resource::ResourceErrorKind::MissingContent,
        .message = rstd::format("buffer prepare service is unavailable"),
    });
}

inline auto ResourcePlanPrepareVisitor::VisitShader(const resource::ShaderPlanEntry&)
    -> Result<empty, resource::ResourceError> {
    return Err(resource::ResourceError {
        .kind    = resource::ResourceErrorKind::MissingContent,
        .message = rstd::format("shader prepare service is unavailable"),
    });
}

} // namespace owe::resource_registry
