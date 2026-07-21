module;

#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

module wescene.vulkan;

import rstd.cppstd;
import rstd.log;

using namespace rstd::prelude;

namespace owe::vulkan
{

namespace
{

constexpr VkDeviceSize kBufferPageSize  = 2 * 1024 * 1024;
constexpr VkDeviceSize kUploadBlockSize = 2 * 1024 * 1024;
constexpr VkDeviceSize kLargeAlignment  = 64 * 1024;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1) return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

VkBufferUsageFlags DestinationUsage() {
    return VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
}

VkAccessFlags DestinationAccess(BufferUploadClass usage) {
    switch (usage) {
    case BufferUploadClass::Vertex: return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    case BufferUploadClass::Index: return VK_ACCESS_INDEX_READ_BIT;
    case BufferUploadClass::Uniform: return VK_ACCESS_UNIFORM_READ_BIT;
    case BufferUploadClass::Storage: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    case BufferUploadClass::Transfer:
        return VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    return VK_ACCESS_MEMORY_READ_BIT;
}

VkPipelineStageFlags DestinationStages(BufferUploadClass usage) {
    switch (usage) {
    case BufferUploadClass::Vertex:
    case BufferUploadClass::Index: return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    case BufferUploadClass::Uniform:
    case BufferUploadClass::Storage:
        return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case BufferUploadClass::Transfer: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    return VK_PIPELINE_STAGE_TRANSFER_BIT;
}

class BufferPage {
public:
    static std::shared_ptr<BufferPage> Create(const Device& device, VkDeviceSize size) {
        auto page = std::shared_ptr<BufferPage>(new BufferPage(device, size));
        if (! page->Initialize()) return {};
        return page;
    }

    ~BufferPage() {
        if (m_virtual_block != VK_NULL_HANDLE) {
            vmaClearVirtualBlock(m_virtual_block);
            vmaDestroyVirtualBlock(m_virtual_block);
        }
    }

    bool TryAllocate(VkDeviceSize size, VkDeviceSize alignment, VmaVirtualAllocation& allocation,
                     VkDeviceSize& offset) {
        VmaVirtualAllocationCreateInfo info {
            .size      = size,
            .alignment = alignment,
        };
        if (vmaVirtualAllocate(m_virtual_block, &info, &allocation, &offset) != VK_SUCCESS) {
            return false;
        }
        ++m_live_allocations;
        return true;
    }

    void Release(VmaVirtualAllocation allocation) {
        if (allocation == VK_NULL_HANDLE) return;
        vmaVirtualFree(m_virtual_block, allocation);
        if (m_live_allocations > 0) --m_live_allocations;
    }

    VkBuffer     handle() const noexcept { return *m_buffer.handle; }
    VkDeviceSize size() const noexcept { return m_size; }
    bool         empty() const noexcept { return m_live_allocations == 0; }

private:
    BufferPage(const Device& device, VkDeviceSize size): m_device(device), m_size(size) {}

    bool Initialize() {
        VkBufferCreateInfo buffer_info {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = m_size,
            .usage = DestinationUsage(),
        };
        VmaAllocationCreateInfo allocation_info {};
        allocation_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        m_buffer.req_size     = m_size;
        VVK_CHECK_BOOL_RE(vvk::CreateBuffer(
            m_device.vma_allocator(), buffer_info, allocation_info, m_buffer.handle));

        VmaVirtualBlockCreateInfo virtual_info { .size = m_size };
        VVK_CHECK_BOOL_RE(vmaCreateVirtualBlock(&virtual_info, &m_virtual_block));
        return true;
    }

    const Device&       m_device;
    VkDeviceSize        m_size { 0 };
    VmaBufferParameters m_buffer;
    VmaVirtualBlock     m_virtual_block { VK_NULL_HANDLE };
    std::size_t         m_live_allocations { 0 };
};

class UploadBlock {
public:
    static std::shared_ptr<UploadBlock> Create(const Device& device, VkDeviceSize size) {
        auto block = std::shared_ptr<UploadBlock>(new UploadBlock(device, size));
        if (! block->Initialize()) return {};
        return block;
    }

    ~UploadBlock() {
        if (m_mapped != nullptr) m_buffer.handle.UnMapMemory();
    }

    void Reset() {
        m_cursor        = 0;
        m_touched_begin = m_size;
        m_touched_end   = 0;
    }

    bool TryAllocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
        auto aligned = AlignUp(m_cursor, alignment);
        if (aligned > m_size || size > m_size - aligned) return false;
        offset   = aligned;
        m_cursor = aligned + size;
        return true;
    }

    void Write(VkDeviceSize offset, std::span<const rstd::uint8_t> data) {
        auto* bytes = static_cast<rstd::uint8_t*>(m_mapped);
        std::memcpy(bytes + offset, data.data(), data.size());
        m_touched_begin = std::min(m_touched_begin, offset);
        m_touched_end   = std::max(m_touched_end, offset + static_cast<VkDeviceSize>(data.size()));
    }

    bool Flush() const {
        if (m_touched_end <= m_touched_begin) return true;
        VVK_CHECK_BOOL_RE(vmaFlushAllocation(m_device.vma_allocator(),
                                             m_buffer.handle.Allocation(),
                                             m_touched_begin,
                                             m_touched_end - m_touched_begin));
        return true;
    }

    VkBuffer     handle() const noexcept { return *m_buffer.handle; }
    VkDeviceSize size() const noexcept { return m_size; }

private:
    UploadBlock(const Device& device, VkDeviceSize size): m_device(device), m_size(size) {}

    bool Initialize() {
        if (! CreateStagingBuffer(m_device.vma_allocator(), m_size, m_buffer)) return false;
        VVK_CHECK_BOOL_RE(m_buffer.handle.MapMemory(&m_mapped));
        Reset();
        return true;
    }

    const Device&       m_device;
    VkDeviceSize        m_size { 0 };
    VmaBufferParameters m_buffer;
    void*               m_mapped { nullptr };
    VkDeviceSize        m_cursor { 0 };
    VkDeviceSize        m_touched_begin { 0 };
    VkDeviceSize        m_touched_end { 0 };
};

struct BufferCopyOperation {
    std::shared_ptr<void>        destination_lease;
    std::shared_ptr<UploadBlock> source;
    VkBuffer                     destination { VK_NULL_HANDLE };
    VkDeviceSize                 source_offset { 0 };
    VkDeviceSize                 destination_offset { 0 };
    VkDeviceSize                 size { 0 };
    BufferUploadClass            usage { BufferUploadClass::Vertex };
};

} // namespace

struct BufferAllocation::State {
    std::shared_ptr<BufferPage> page;
    VmaVirtualAllocation        allocation { VK_NULL_HANDLE };
    VkDeviceSize                offset { 0 };
    VkDeviceSize                size { 0 };
    BufferUploadClass           usage { BufferUploadClass::Vertex };

    ~State() {
        if (page) page->Release(allocation);
    }
};

struct RecordedBufferUploads::State {
    u64                                       serial { 0 };
    bool                                      recorded { false };
    std::vector<BufferCopyOperation>          copies;
    std::vector<std::shared_ptr<UploadBlock>> blocks;
    std::vector<BufferUploadTicket>           tickets;
};

struct BufferManager::Impl {
    explicit Impl(const Device& value): device(value) {}

    const Device&                                 device;
    bool                                          initialized { false };
    u64                                           next_ticket { 0 };
    u64                                           next_batch { 0 };
    std::vector<std::shared_ptr<BufferPage>>      pages;
    std::vector<std::shared_ptr<UploadBlock>>     upload_blocks;
    std::shared_ptr<RecordedBufferUploads::State> pending;
};

BufferAllocation::BufferAllocation(std::shared_ptr<State> state): m_state(std::move(state)) {}
BufferAllocation::~BufferAllocation()                                      = default;
BufferAllocation::BufferAllocation(BufferAllocation&&) noexcept            = default;
BufferAllocation& BufferAllocation::operator=(BufferAllocation&&) noexcept = default;

BufferAllocation::operator bool() const noexcept {
    return m_state && m_state->page && m_state->allocation != VK_NULL_HANDLE && m_state->size > 0;
}

VkBuffer BufferAllocation::buffer() const noexcept {
    return m_state && m_state->page ? m_state->page->handle() : VK_NULL_HANDLE;
}

VkDeviceSize BufferAllocation::offset() const noexcept { return m_state ? m_state->offset : 0; }

VkDeviceSize BufferAllocation::size() const noexcept { return m_state ? m_state->size : 0; }

RecordedBufferUploads::RecordedBufferUploads(BufferManager* owner, std::shared_ptr<State> state)
    : m_owner(owner), m_state(std::move(state)) {}

RecordedBufferUploads::~RecordedBufferUploads() { Reset(); }

RecordedBufferUploads::RecordedBufferUploads(RecordedBufferUploads&& other) noexcept
    : m_owner(other.m_owner), m_state(std::move(other.m_state)) {
    other.m_owner = nullptr;
}

RecordedBufferUploads& RecordedBufferUploads::operator=(RecordedBufferUploads&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    m_owner       = other.m_owner;
    m_state       = std::move(other.m_state);
    other.m_owner = nullptr;
    return *this;
}

bool RecordedBufferUploads::Valid() const noexcept { return m_owner != nullptr && m_state; }

void RecordedBufferUploads::Reset() {
    if (m_owner && m_state) m_owner->CancelRecordedUploads(m_state);
    m_owner = nullptr;
    m_state.reset();
}

BufferUploadBatchLease::BufferUploadBatchLease(std::shared_ptr<RecordedBufferUploads::State> state)
    : m_state(std::move(state)) {}

BufferUploadBatchLease::~BufferUploadBatchLease() = default;
BufferUploadBatchLease::BufferUploadBatchLease(BufferUploadBatchLease&& other) noexcept
    : m_state(std::move(other.m_state)) {}

BufferUploadBatchLease& BufferUploadBatchLease::operator=(BufferUploadBatchLease&& other) noexcept {
    if (this != &other) m_state = std::move(other.m_state);
    return *this;
}

bool BufferUploadBatchLease::Valid() const noexcept { return static_cast<bool>(m_state); }

std::span<const BufferUploadTicket> BufferUploadBatchLease::Tickets() const noexcept {
    if (! m_state) return {};
    return m_state->tickets;
}

BufferManager::BufferManager(const Device& device): m_impl(Box<Impl>::make(device)) {}
BufferManager::~BufferManager() { destroy(); }

bool BufferManager::init() {
    m_impl->initialized = true;
    return true;
}

void BufferManager::destroy() {
    if (! m_impl->initialized) return;
    m_impl->pending.reset();
    m_impl->upload_blocks.clear();
    m_impl->pages.clear();
    m_impl->initialized = false;
}

Option<BufferAllocation> BufferManager::Allocate(const BufferAllocationRequest& request) {
    if (! m_impl->initialized || request.size == 0) return None();

    VkDeviceSize alignment = std::max<VkDeviceSize>(request.alignment, 4);
    if (request.usage == BufferUploadClass::Uniform) {
        alignment = std::max(alignment, m_impl->device.limits().minUniformBufferOffsetAlignment);
    } else if (request.usage == BufferUploadClass::Storage) {
        alignment = std::max(alignment, m_impl->device.limits().minStorageBufferOffsetAlignment);
    }

    VmaVirtualAllocation        allocation { VK_NULL_HANDLE };
    VkDeviceSize                offset { 0 };
    std::shared_ptr<BufferPage> page;
    for (const auto& candidate : m_impl->pages) {
        if (candidate->TryAllocate(request.size, alignment, allocation, offset)) {
            page = candidate;
            break;
        }
    }
    if (! page) {
        const auto page_size = request.size > kBufferPageSize
                                   ? AlignUp(request.size, kLargeAlignment)
                                   : kBufferPageSize;
        page                 = BufferPage::Create(m_impl->device, page_size);
        if (! page || ! page->TryAllocate(request.size, alignment, allocation, offset)) {
            return None();
        }
        m_impl->pages.push_back(page);
        rstd_info(
            "new destination buffer page, size: {}, pages: {}", page_size, m_impl->pages.size());
    }

    auto state        = std::make_shared<BufferAllocation::State>();
    state->page       = std::move(page);
    state->allocation = allocation;
    state->offset     = offset;
    state->size       = request.size;
    state->usage      = request.usage;
    return Some(BufferAllocation(std::move(state)));
}

Option<BufferUploadTicket> BufferManager::QueueWrite(BufferAllocation&              allocation,
                                                     std::span<const rstd::uint8_t> data,
                                                     VkDeviceSize destination_offset) {
    if (! m_impl->initialized || ! allocation || destination_offset > allocation.size() ||
        static_cast<VkDeviceSize>(data.size()) > allocation.size() - destination_offset) {
        return None();
    }
    if (data.empty()) return Some(BufferUploadTicket {});
    if (m_impl->pending && m_impl->pending->recorded) {
        rstd_error("queue buffer write while the pending upload batch is recorded");
        return None();
    }

    if (! m_impl->pending) {
        ++m_impl->next_batch;
        if (m_impl->next_batch == u64()) ++m_impl->next_batch;
        m_impl->pending         = std::make_shared<RecordedBufferUploads::State>();
        m_impl->pending->serial = m_impl->next_batch;
    }

    VkDeviceSize                 upload_offset { 0 };
    std::shared_ptr<UploadBlock> block;
    for (const auto& candidate : m_impl->pending->blocks) {
        if (candidate->TryAllocate(static_cast<VkDeviceSize>(data.size()), 4, upload_offset)) {
            block = candidate;
            break;
        }
    }
    if (! block) {
        const auto required = static_cast<VkDeviceSize>(data.size());
        for (const auto& candidate : m_impl->upload_blocks) {
            if (candidate.use_count() != 1 || candidate->size() < required) continue;
            candidate->Reset();
            if (candidate->TryAllocate(required, 4, upload_offset)) {
                block = candidate;
                break;
            }
        }
        if (! block) {
            const auto block_size =
                required > kUploadBlockSize ? AlignUp(required, kLargeAlignment) : kUploadBlockSize;
            block = UploadBlock::Create(m_impl->device, block_size);
            if (! block || ! block->TryAllocate(required, 4, upload_offset)) return None();
            m_impl->upload_blocks.push_back(block);
            rstd_info("new upload buffer block, size: {}, blocks: {}",
                      block_size,
                      m_impl->upload_blocks.size());
        }
        m_impl->pending->blocks.push_back(block);
    }

    block->Write(upload_offset, data);
    ++m_impl->next_ticket;
    if (m_impl->next_ticket == u64()) ++m_impl->next_ticket;
    BufferUploadTicket ticket { .value = m_impl->next_ticket };
    m_impl->pending->copies.push_back(BufferCopyOperation {
        .destination_lease  = allocation.m_state,
        .source             = std::move(block),
        .destination        = allocation.buffer(),
        .source_offset      = upload_offset,
        .destination_offset = allocation.offset() + destination_offset,
        .size               = static_cast<VkDeviceSize>(data.size()),
        .usage              = allocation.m_state->usage,
    });
    m_impl->pending->tickets.push_back(ticket);
    return Some(ticket);
}

bool BufferManager::HasPendingUploads() const noexcept {
    return m_impl->pending && ! m_impl->pending->copies.empty();
}

bool BufferManager::RecordPendingUploads(vvk::CommandBuffer& cmd, RecordedBufferUploads& recorded) {
    recorded.Reset();
    if (! HasPendingUploads()) return true;
    if (m_impl->pending->recorded) {
        rstd_error("buffer upload batch is already recorded");
        return false;
    }
    for (const auto& block : m_impl->pending->blocks) {
        if (! block->Flush()) return false;
    }
    for (const auto& copy : m_impl->pending->copies) {
        cmd.CopyBuffer(copy.source->handle(),
                       copy.destination,
                       VkBufferCopy {
                           .srcOffset = copy.source_offset,
                           .dstOffset = copy.destination_offset,
                           .size      = copy.size,
                       });
    }
    for (const auto& copy : m_impl->pending->copies) {
        VkBufferMemoryBarrier barrier {
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = DestinationAccess(copy.usage),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = copy.destination,
            .offset              = copy.destination_offset,
            .size                = copy.size,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            DestinationStages(copy.usage),
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);
    }
    m_impl->pending->recorded = true;
    recorded                  = RecordedBufferUploads(this, m_impl->pending);
    return true;
}

Option<BufferUploadBatchLease>
BufferManager::CommitRecordedUploads(RecordedBufferUploads&& recorded) {
    if (! recorded.Valid() || recorded.m_owner != this || recorded.m_state != m_impl->pending ||
        ! recorded.m_state->recorded) {
        return None();
    }
    auto state       = std::move(recorded.m_state);
    recorded.m_owner = nullptr;
    state->recorded  = false;
    m_impl->pending.reset();
    return Some(BufferUploadBatchLease(std::move(state)));
}

void BufferManager::CancelRecordedUploads(
    const std::shared_ptr<RecordedBufferUploads::State>& state) {
    if (state && state == m_impl->pending) state->recorded = false;
}

void BufferManager::Trim() {
    bool kept_empty_page = false;
    m_impl->pages.erase(std::remove_if(m_impl->pages.begin(),
                                       m_impl->pages.end(),
                                       [&kept_empty_page](const auto& page) {
                                           if (! page->empty()) return false;
                                           if (! kept_empty_page) {
                                               kept_empty_page = true;
                                               return false;
                                           }
                                           return page.use_count() == 1;
                                       }),
                        m_impl->pages.end());

    bool kept_upload_block = false;
    m_impl->upload_blocks.erase(std::remove_if(m_impl->upload_blocks.begin(),
                                               m_impl->upload_blocks.end(),
                                               [&kept_upload_block](const auto& block) {
                                                   if (block.use_count() != 1) return false;
                                                   if (! kept_upload_block) {
                                                       kept_upload_block = true;
                                                       return false;
                                                   }
                                                   return true;
                                               }),
                                m_impl->upload_blocks.end());
}

} // namespace owe::vulkan
