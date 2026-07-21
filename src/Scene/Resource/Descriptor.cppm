module;

#include <algorithm>

export module wescene.resource_registry:descriptor;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.vulkan;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct DescriptorBindingSchema {
    rstd::uint32_t binding { 0 };
    rstd::uint32_t descriptor_type { 0 };
    rstd::uint32_t descriptor_count { 1 };
    rstd::uint32_t stage_flags { 0 };

    friend bool operator==(const DescriptorBindingSchema&,
                           const DescriptorBindingSchema&) = default;
};

struct DescriptorSetSchema {
    bool                                    push_descriptor { false };
    rstd::vec::Vec<DescriptorBindingSchema> bindings;

    auto clone() const -> DescriptorSetSchema {
        auto cloned = rstd::vec::Vec<DescriptorBindingSchema>::with_capacity(bindings.len());
        for (const auto& binding : bindings) {
            cloned.push(DescriptorBindingSchema {
                .binding          = binding.binding,
                .descriptor_type  = binding.descriptor_type,
                .descriptor_count = binding.descriptor_count,
                .stage_flags      = binding.stage_flags,
            });
        }
        return DescriptorSetSchema {
            .push_descriptor = push_descriptor,
            .bindings        = rstd::move(cloned),
        };
    }

    friend bool operator==(const DescriptorSetSchema& lhs, const DescriptorSetSchema& rhs) {
        if (lhs.push_descriptor != rhs.push_descriptor ||
            lhs.bindings.len() != rhs.bindings.len()) {
            return false;
        }
        for (usize index = usize(); index < lhs.bindings.len(); ++index) {
            if (lhs.bindings[index] != rhs.bindings[index]) return false;
        }
        return true;
    }
};

struct DescriptorSetSchemaHasher {
    rstd::hash::RandomState state;

    auto operator()(const DescriptorSetSchema& schema) const noexcept -> u64 {
        auto seed = state(schema.push_descriptor);
        auto mix  = [&](u64 value) {
            seed ^= value.wrapping_add(u64(0x9e3779b97f4a7c15ULL))
                        .wrapping_add(seed.wrapping_shl(u64(6)))
                        .wrapping_add(seed >> u64(2));
        };
        for (const auto& binding : schema.bindings) {
            mix(state(binding.binding));
            mix(state(binding.descriptor_type));
            mix(state(binding.descriptor_count));
            mix(state(binding.stage_flags));
        }
        return seed;
    }
};

struct DescriptorLayoutEntry {
    resource::DescriptorLayoutHandle handle;
    DescriptorSetSchema              schema;
    vvk::DescriptorSetLayout         layout;
};

class DescriptorLayoutRegistry {
public:
    DescriptorLayoutRegistry()                                           = default;
    DescriptorLayoutRegistry(const DescriptorLayoutRegistry&)            = delete;
    DescriptorLayoutRegistry& operator=(const DescriptorLayoutRegistry&) = delete;

    auto Ensure(const vulkan::Device& device, const vulkan::DescriptorSetInfo& info)
        -> Result<resource::DescriptorLayoutHandle, resource::ResourceError> {
        if (info.push_descriptor && ! device.capabilities().push_descriptor) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("push descriptors are unavailable"),
            });
        }
        auto schema = CanonicalSchema(info);
        if (schema.is_err()) return Err(rstd::move(schema).unwrap_err_unchecked());
        auto normalized = rstd::move(schema).unwrap_unchecked();
        if (auto existing = m_handles.get(normalized); existing.is_some()) return Ok(**existing);

        auto bindings =
            rstd::vec::Vec<VkDescriptorSetLayoutBinding>::with_capacity(normalized.bindings.len());
        for (const auto& binding : normalized.bindings) {
            bindings.push(VkDescriptorSetLayoutBinding {
                .binding            = binding.binding,
                .descriptorType     = static_cast<VkDescriptorType>(binding.descriptor_type),
                .descriptorCount    = binding.descriptor_count,
                .stageFlags         = binding.stage_flags,
                .pImmutableSamplers = nullptr,
            });
        }

        VkDescriptorSetLayoutCreateInfo create_info {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext        = nullptr,
            .flags        = normalized.push_descriptor
                                ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR
                                : VkDescriptorSetLayoutCreateFlags {},
            .bindingCount = static_cast<rstd::uint32_t>(bindings.len().to_primitive()),
            .pBindings    = bindings.data(),
        };
        vvk::DescriptorSetLayout layout;
        if (device.handle().CreateDescriptorSetLayout(create_info, layout) != VK_SUCCESS) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("create descriptor set layout failed"),
            });
        }

        resource::DescriptorLayoutHandle handle {
            .index      = m_next_index++,
            .generation = m_generation,
        };
        auto identity = normalized.clone();
        (void)m_entries.insert(handle,
                               DescriptorLayoutEntry {
                                   .handle = handle,
                                   .schema = rstd::move(normalized),
                                   .layout = rstd::move(layout),
                               });
        (void)m_handles.insert(rstd::move(identity), handle);
        return Ok(handle);
    }

    auto Resolve(resource::DescriptorLayoutHandle handle) const
        -> Option<ref<DescriptorLayoutEntry>> {
        return m_entries.get(handle);
    }

    void Reset() {
        m_handles.clear();
        m_entries.clear();
        m_next_index = u64();
        ++m_generation;
        if (m_generation == u64()) ++m_generation;
    }

    auto Size() const noexcept -> usize { return m_entries.len(); }

    static auto CanonicalSchema(const vulkan::DescriptorSetInfo& info)
        -> Result<DescriptorSetSchema, resource::ResourceError> {
        auto bindings =
            rstd::vec::Vec<DescriptorBindingSchema>::with_capacity(usize(info.bindings.size()));
        for (const auto& binding : info.bindings) {
            if (binding.pImmutableSamplers != nullptr) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::MissingDefinition,
                    .message = rstd::format("immutable descriptor samplers are not supported"),
                });
            }
            bindings.push(DescriptorBindingSchema {
                .binding          = binding.binding,
                .descriptor_type  = static_cast<rstd::uint32_t>(binding.descriptorType),
                .descriptor_count = binding.descriptorCount,
                .stage_flags      = binding.stageFlags,
            });
        }
        std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.binding < rhs.binding;
        });
        for (usize index = usize(); index < bindings.len(); ++index) {
            if (bindings[index].descriptor_count == 0 || bindings[index].stage_flags == 0 ||
                (index > usize() &&
                 bindings[index - usize(1)].binding == bindings[index].binding)) {
                return Err(resource::ResourceError {
                    .kind = resource::ResourceErrorKind::MissingDefinition,
                    .message =
                        rstd::format("invalid descriptor binding {}", bindings[index].binding),
                });
            }
        }
        return Ok(DescriptorSetSchema {
            .push_descriptor = info.push_descriptor,
            .bindings        = rstd::move(bindings),
        });
    }

private:
    using EntryMap = rstd::collections::HashMap<
        resource::DescriptorLayoutHandle, DescriptorLayoutEntry,
        resource::ResourceHandleHasher<resource::DescriptorLayoutHandle>>;
    using IdentityMap =
        rstd::collections::HashMap<DescriptorSetSchema, resource::DescriptorLayoutHandle,
                                   DescriptorSetSchemaHasher>;

    u64         m_generation { 1 };
    u64         m_next_index { 0 };
    EntryMap    m_entries;
    IdentityMap m_handles;
};

struct DescriptorImageBinding {
    rstd::uint32_t          binding { 0 };
    vulkan::ImageParameters image;
};

struct DescriptorBufferBinding {
    rstd::uint32_t binding { 0 };
    VkBuffer       buffer { VK_NULL_HANDLE };
    VkDeviceSize   offset { 0 };
    VkDeviceSize   size { 0 };
};

enum class DescriptorBindingBackend
{
    Push,
    Set,
};

struct PreparedDescriptorBinding {
    resource::DescriptorBindingHandle      handle;
    DescriptorBindingBackend               backend { DescriptorBindingBackend::Push };
    rstd::vec::Vec<DescriptorImageBinding> images;
    Option<DescriptorBufferBinding>        buffer;
    Option<vvk::DescriptorSetLease>        set;

    auto clone() const -> PreparedDescriptorBinding {
        auto cloned_images = rstd::vec::Vec<DescriptorImageBinding>::with_capacity(images.len());
        for (const auto& image : images) {
            cloned_images.push(DescriptorImageBinding {
                .binding = image.binding,
                .image   = image.image,
            });
        }
        return PreparedDescriptorBinding {
            .handle  = handle,
            .backend = backend,
            .images  = rstd::move(cloned_images),
            .buffer  = buffer,
            .set     = set.is_some() ? Some(set->clone()) : None(),
        };
    }

    void Record(vvk::CommandBuffer& command, VkPipelineLayout layout) const {
        if (backend == DescriptorBindingBackend::Set) {
            if (set.is_none() || ! set->valid()) return;
            auto descriptor_set = set->handle;
            command.BindDescriptorSets(
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                layout,
                0,
                slice<VkDescriptorSet>::from_raw_parts(&descriptor_set, usize(1)),
                {});
            return;
        }
        for (const auto& binding : images) {
            VkDescriptorImageInfo image {
                .sampler     = binding.image.sampler,
                .imageView   = binding.image.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            VkWriteDescriptorSet write {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext           = nullptr,
                .dstSet          = {},
                .dstBinding      = binding.binding,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo      = &image,
            };
            command.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, write);
        }
        if (buffer.is_none()) return;

        VkDescriptorBufferInfo info {
            .buffer = buffer->buffer,
            .offset = buffer->offset,
            .range  = buffer->size,
        };
        VkWriteDescriptorSet write {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = buffer->binding,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &info,
        };
        command.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, write);
    }
};

class DescriptorSystem {
public:
    auto PreparePush(rstd::slice<DescriptorImageBinding> images,
                     Option<DescriptorBufferBinding>     buffer) -> PreparedDescriptorBinding {
        auto prepared_images = rstd::vec::Vec<DescriptorImageBinding>::with_capacity(images.len());
        for (usize index = usize(); index < images.len(); ++index) {
            prepared_images.push(DescriptorImageBinding {
                .binding = images[index].binding,
                .image   = images[index].image,
            });
        }
        return PreparedDescriptorBinding {
            .handle =
                resource::DescriptorBindingHandle {
                    .index      = m_next_index++,
                    .generation = m_generation,
                },
            .backend = DescriptorBindingBackend::Push,
            .images  = rstd::move(prepared_images),
            .buffer  = buffer,
        };
    }

    auto Prepare(const vulkan::Device& device, const DescriptorLayoutEntry& layout,
                 rstd::slice<DescriptorImageBinding> images, Option<DescriptorBufferBinding> buffer)
        -> Result<PreparedDescriptorBinding, resource::ResourceError> {
        auto prepared = PreparePush(images, buffer);
        if (layout.schema.push_descriptor) return Ok(rstd::move(prepared));
        prepared.backend = DescriptorBindingBackend::Set;

        auto allocated = AllocateSet(device, *layout.layout);
        if (allocated.is_err()) return Err(rstd::move(allocated).unwrap_err_unchecked());
        prepared.set = Some(rstd::move(allocated).unwrap_unchecked());

        vvk::DescriptorUpdateBatch updates;
        for (const auto& binding : prepared.images) {
            VkDescriptorImageInfo image {
                .sampler     = binding.image.sampler,
                .imageView   = binding.image.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            if (! updates.WriteImage(
                    prepared.set->clone(),
                    binding.binding,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    slice<VkDescriptorImageInfo>::from_raw_parts(&image, usize(1)))) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::BackendFailure,
                    .message = rstd::format("prepare descriptor image binding failed"),
                });
            }
        }
        if (prepared.buffer.is_some()) {
            VkDescriptorBufferInfo info {
                .buffer = prepared.buffer->buffer,
                .offset = prepared.buffer->offset,
                .range  = prepared.buffer->size,
            };
            if (! updates.WriteBuffer(
                    prepared.set->clone(),
                    prepared.buffer->binding,
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    slice<VkDescriptorBufferInfo>::from_raw_parts(&info, usize(1)))) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::BackendFailure,
                    .message = rstd::format("prepare descriptor buffer binding failed"),
                });
            }
        }
        if (! updates.Commit().committed()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("update descriptor set failed"),
            });
        }
        return Ok(rstd::move(prepared));
    }

    void Reset() {
        m_pool       = None();
        m_next_index = u64();
        ++m_generation;
        if (m_generation == u64()) ++m_generation;
    }

private:
    auto AllocateSet(const vulkan::Device& device, VkDescriptorSetLayout layout)
        -> Result<vvk::DescriptorSetLease, resource::ResourceError> {
        if (m_pool.is_none()) {
            auto created = CreatePool(device);
            if (created.is_err()) return Err(rstd::move(created).unwrap_err_unchecked());
            m_pool = Some(rstd::move(created).unwrap_unchecked());
        }

        auto allocated = vvk::DescriptorArenaGeneration::Allocate(*m_pool, layout);
        if (! allocated.allocated()) {
            auto created = CreatePool(device);
            if (created.is_err()) return Err(rstd::move(created).unwrap_err_unchecked());
            m_pool    = Some(rstd::move(created).unwrap_unchecked());
            allocated = vvk::DescriptorArenaGeneration::Allocate(*m_pool, layout);
        }
        if (! allocated.allocated()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("allocate descriptor set failed: {}",
                                        static_cast<rstd::int32_t>(allocated.api_result)),
            });
        }
        return Ok(rstd::move(allocated.lease));
    }

    auto CreatePool(const vulkan::Device& device)
        -> Result<rstd::sync::Arc<vvk::DescriptorArenaGeneration>, resource::ResourceError> {
        auto sizes = rstd::vec::Vec<VkDescriptorPoolSize>::with_capacity(usize(2));
        sizes.push(VkDescriptorPoolSize {
            .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 8192,
        });
        sizes.push(VkDescriptorPoolSize {
            .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 2048,
        });
        auto created =
            vvk::DescriptorArenaGeneration::Create(*device.handle(), 2048, sizes.as_slice());
        if (! created.created()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("create descriptor pool failed: {}",
                                        static_cast<rstd::int32_t>(created.api_result)),
            });
        }
        return Ok(rstd::move(*created.arena));
    }

    Option<rstd::sync::Arc<vvk::DescriptorArenaGeneration>> m_pool;
    u64                                                     m_generation { 1 };
    u64                                                     m_next_index { 0 };
};

} // namespace owe::resource_registry
