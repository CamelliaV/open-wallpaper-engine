module;

#include <rstd/macro.hpp>

module wescene.vulkan;

import rstd.cppstd;
import rstd.log;

using namespace rstd::prelude;

namespace owe::vulkan
{

BufferAllocation::BufferAllocation(BufferUploadPool* owner, StagingBufferRef ref)
    : m_owner(owner), m_ref(ref) {}

BufferAllocation::~BufferAllocation() {
    if (m_owner) m_owner->Release(m_ref);
}

BufferAllocation::BufferAllocation(BufferAllocation&& o) noexcept
    : m_owner(o.m_owner), m_ref(o.m_ref) {
    o.m_owner = nullptr;
    o.m_ref   = {};
}

BufferAllocation& BufferAllocation::operator=(BufferAllocation&& o) noexcept {
    if (this != &o) {
        if (m_owner) m_owner->Release(m_ref);
        m_owner   = o.m_owner;
        m_ref     = o.m_ref;
        o.m_owner = nullptr;
        o.m_ref   = {};
    }
    return *this;
}

VkBuffer BufferAllocation::buffer() const noexcept {
    return m_owner ? m_owner->gpuBuf() : VK_NULL_HANDLE;
}

namespace
{
constexpr VkDeviceSize kSeedSize = 2 * 1024 * 1024;
} // namespace

BufferUploadPool::BufferUploadPool(const Device& d): m_device(d) {}
BufferUploadPool::~BufferUploadPool() { destroy(); }

bool BufferUploadPool::init() {
    if (m_buf.is_some()) return true;
    m_buf = Some(Box<StagingBuffer>::make(
        m_device,
        kSeedSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    if (! m_buf->get()->allocate()) {
        m_buf = None();
        return false;
    }
    return true;
}

void BufferUploadPool::destroy() {
    if (m_buf.is_some()) {
        m_buf->get()->destroy();
        m_buf = None();
    }
    m_dirty = false;
}

Option<BufferAllocation> BufferUploadPool::Upload(std::span<const rstd::uint8_t> data,
                                                  const BufferUploadRequest&     request) {
    if (m_buf.is_none() || request.size == 0 || data.size() > request.size) {
        return None();
    }
    VkDeviceSize alignment = request.alignment;
    if (request.usage == BufferUploadClass::Uniform) {
        alignment = std::max(alignment, m_device.limits().minUniformBufferOffsetAlignment);
    } else if (request.usage == BufferUploadClass::Storage) {
        alignment = std::max(alignment, m_device.limits().minStorageBufferOffsetAlignment);
    }
    StagingBufferRef ref;
    auto*            buffer = m_buf->get();
    if (! buffer->allocateSubRef(request.size, ref, alignment)) return None();
    if (! data.empty() && ! buffer->writeToBuf(ref, data)) {
        buffer->unallocateSubRef(ref);
        return None();
    }

    m_dirty = true;
    return Some(BufferAllocation(this, ref));
}

bool BufferUploadPool::Update(BufferAllocation& allocation, std::span<const rstd::uint8_t> data) {
    if (m_buf.is_none()) {
        rstd_error("update buffer failed: upload pool is not initialized");
        return false;
    }
    if (allocation.m_owner != this) {
        rstd_error("update buffer failed: allocation belongs to another upload pool");
        return false;
    }
    if (! allocation.m_ref) {
        rstd_error("update buffer failed: staging allocation is unavailable");
        return false;
    }
    if (data.size() > allocation.m_ref.size) {
        rstd_error("update buffer failed: content size {} exceeds allocation size {}",
                   data.size(),
                   allocation.m_ref.size);
        return false;
    }
    if (data.empty()) return true;
    if (! m_buf->get()->writeToBuf(allocation.m_ref, data)) {
        return false;
    }
    m_dirty = true;
    return true;
}

void BufferUploadPool::Release(StagingBufferRef ref) {
    if (m_buf.is_some() && ref) m_buf->get()->unallocateSubRef(ref);
}

VkBuffer BufferUploadPool::gpuBuf() const {
    return m_buf.is_some() ? m_buf->as_ptr().as_raw_ptr()->gpuBuf() : VK_NULL_HANDLE;
}

bool BufferUploadPool::preparePendingUploads() {
    if (m_buf.is_none()) return false;
    return ! m_dirty || m_buf->get()->prepareGpuBuffer();
}

bool BufferUploadPool::recordPendingUploads(vvk::CommandBuffer& cmd) {
    if (m_buf.is_none() || ! m_dirty) return true;
    if (! m_buf->get()->recordUpload(cmd)) return false;
    m_dirty = false;
    return true;
}

} // namespace owe::vulkan
