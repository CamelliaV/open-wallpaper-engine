module;

#include <cassert>

#include <unistd.h>

// Vulkan loader dynamically — VK_NO_PROTOTYPES so we go through Dispatch.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "vk_mem_alloc.h"

#include "Type.hpp"
#include "Image.hpp"
#include "Core/Literals.hpp"
#include "Core/MapSet.hpp"
#include "Core/NoCopyMove.hpp"

#include "Utils/Logging.h"

// ExSwapchain / TripleSwapchain are classic public headers (consumed by
// SceneWallpaperSurface.hpp). LocalExSwapchain inherits from ExSwapchain;
// the base class stays global-attached, the derived is module-attached.
#include "Swapchain/ExSwapchain.hpp"
#include "Swapchain/TripleSwapchain.hpp"

// Macros only — VVK_CHECK family.
#include "vvk/macros.hpp"

export module wescene.vulkan;
import cppstd;
import wescene.utils;

// Re-export the host-only shader compile API. Lets existing consumers
// (VulkanRender/* etc.) keep their `import wescene.vulkan;` without
// caring that ShaderSpv / ShaderReflected / Preprocess / etc. now live
// in a separate module.
export import wescene.shader_compile;

// =================================================================
// Layer 1: vvk:: low-level Vulkan C++ wrapper
// =================================================================

export namespace vvk
{

// ---------- span.hpp ----------

template<typename T>
class Span {
public:
    using value_type             = T;
    using u32                    = uint32_t;
    using size_type              = std::size_t;
    using size_type_out          = u32;
    using difference_type        = std::ptrdiff_t;
    using reference              = T&;
    using const_reference        = const T&;
    using nonconst_reference     = std::remove_const_t<T>&;
    using pointer                = T*;
    using const_pointer          = const pointer;
    using iterator               = T*;
    using const_iterator         = const iterator;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    constexpr Span() noexcept = default;
    constexpr Span(std::nullptr_t) noexcept {}
    constexpr Span(reference value) noexcept: ptr { &value }, num { 1 } {}

    template<typename U = value_type, typename = std::enable_if_t<std::is_const_v<U>>>
    constexpr Span(nonconst_reference value) noexcept: ptr { &value }, num { 1 } {}

    template<typename Range>
    constexpr Span(const Range& range) noexcept
        : ptr { std::data(range) }, num { std::size(range) } {}

    template<typename Range>
    constexpr Span(Range& range) noexcept: ptr { std::data(range) }, num { std::size(range) } {}

    constexpr Span(pointer ptr_, size_type num_) noexcept: ptr { ptr_ }, num { num_ } {}

    constexpr pointer       data() const noexcept { return ptr; }
    constexpr size_type_out size() const noexcept { return static_cast<size_type_out>(num); }
    constexpr bool          empty() const noexcept { return num == 0; }
    constexpr reference     operator[](std::size_t index) const noexcept { return ptr[index]; }
    constexpr pointer       begin() const noexcept { return ptr; }
    constexpr pointer       end() const noexcept { return ptr + num; }
    constexpr pointer       cbegin() const noexcept { return ptr; }
    constexpr pointer       cend() const noexcept { return ptr + num; }

private:
    pointer   ptr { nullptr };
    size_type num { 0 };
};

template<typename T, typename R>
constexpr bool operator==(const Span<T>& lhs, const Span<R>& rhs) noexcept {
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

// ---------- handle.hpp ----------

template<typename Type, typename OwnerType, typename Dispatch>
class Handle : NoCopy {
public:
    using handle_type = Type;
    explicit Handle(Type handle_, OwnerType owner_, const Dispatch& dld_) noexcept
        : handle { handle_ }, owner { owner_ }, dld { &dld_ } {}

    Handle() = default;
    Handle(std::nullptr_t) {}

    Handle(Handle&& rhs) noexcept
        : handle { std::exchange(rhs.handle, nullptr) }, owner { rhs.owner }, dld { rhs.dld } {}
    Handle& operator=(Handle&& rhs) noexcept {
        Release();
        handle = std::exchange(rhs.handle, nullptr);
        owner  = rhs.owner;
        dld    = rhs.dld;
        return *this;
    }

    ~Handle() noexcept { Release(); }

    void reset() noexcept {
        Release();
        handle = nullptr;
    }

    const Type* address() const noexcept { return std::addressof(handle); }
    const Type& operator*() const noexcept { return handle; }
    explicit operator bool() const noexcept { return handle != nullptr; }

protected:
    Type            handle = nullptr;
    OwnerType       owner  = nullptr;
    const Dispatch* dld    = nullptr;

private:
    void Release() noexcept {
        if (handle) {
            Destroy(owner, handle, *dld);
        }
    }
};

struct NoOwner {};
struct NoOwnerLife {};

template<typename Type, typename Dispatch>
class Handle<Type, NoOwner, Dispatch> : NoCopy {
public:
    using handle_type = Type;
    explicit Handle(Type handle_, const Dispatch& dld_) noexcept
        : handle { handle_ }, dld { &dld_ } {}

    Handle() = default;
    Handle(std::nullptr_t) {}

    Handle(Handle&& rhs) noexcept: handle { std::exchange(rhs.handle, nullptr) }, dld { rhs.dld } {}
    Handle& operator=(Handle&& rhs) noexcept {
        Release();
        handle = std::exchange(rhs.handle, nullptr);
        dld    = rhs.dld;
        return *this;
    }

    ~Handle() noexcept { Release(); }

    void reset() noexcept {
        Release();
        handle = nullptr;
    }

    const Type* address() const noexcept { return std::addressof(handle); }
    const Type& operator*() const noexcept { return handle; }
    explicit operator bool() const noexcept { return handle != nullptr; }

protected:
    Type            handle = nullptr;
    const Dispatch* dld    = nullptr;

private:
    void Release() noexcept {
        if (handle) {
            Destroy(handle, *dld);
        }
    }
};

template<typename Type, typename Dispatch>
class Handle<Type, NoOwnerLife, Dispatch> {
public:
    using handle_type = Type;
    explicit Handle(Type handle_, const Dispatch& dld_) noexcept
        : handle { handle_ }, dld { &dld_ } {}

    Handle()  = default;
    ~Handle() = default;

    Handle(std::nullptr_t) {}

    void reset() noexcept { handle = nullptr; }

    const Type* address() const noexcept { return std::addressof(handle); }
    const Type& operator*() const noexcept { return handle; }
    explicit operator bool() const noexcept { return handle != nullptr; }

protected:
    Type            handle = nullptr;
    const Dispatch* dld    = nullptr;
};

// ---------- vulkan_wrapper.hpp ----------

const char* ToString(VkResult result) noexcept;
const char* ToString(VkFormat format) noexcept;
const char* ToString(VkColorSpaceKHR color) noexcept;

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr {};

    PFN_vkCreateInstance                       vkCreateInstance {};
    PFN_vkDestroyInstance                      vkDestroyInstance {};
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties {};
    PFN_vkEnumerateInstanceLayerProperties     vkEnumerateInstanceLayerProperties {};

    PFN_vkCreateDebugUtilsMessengerEXT            vkCreateDebugUtilsMessengerEXT {};
    PFN_vkCreateDevice                            vkCreateDevice {};
    PFN_vkDestroyDebugUtilsMessengerEXT           vkDestroyDebugUtilsMessengerEXT {};
    PFN_vkDestroyDevice                           vkDestroyDevice {};
    PFN_vkDestroySurfaceKHR                       vkDestroySurfaceKHR {};
    PFN_vkEnumerateDeviceExtensionProperties      vkEnumerateDeviceExtensionProperties {};
    PFN_vkEnumeratePhysicalDevices                vkEnumeratePhysicalDevices {};
    PFN_vkGetDeviceProcAddr                       vkGetDeviceProcAddr {};
    PFN_vkGetPhysicalDeviceFeatures2KHR           vkGetPhysicalDeviceFeatures2KHR {};
    PFN_vkGetPhysicalDeviceFormatProperties       vkGetPhysicalDeviceFormatProperties {};
    PFN_vkGetPhysicalDeviceMemoryProperties       vkGetPhysicalDeviceMemoryProperties {};
    PFN_vkGetPhysicalDeviceMemoryProperties2      vkGetPhysicalDeviceMemoryProperties2 {};
    PFN_vkGetPhysicalDeviceProperties             vkGetPhysicalDeviceProperties {};
    PFN_vkGetPhysicalDeviceProperties2KHR         vkGetPhysicalDeviceProperties2KHR {};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties  vkGetPhysicalDeviceQueueFamilyProperties {};
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR {};
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR      vkGetPhysicalDeviceSurfaceFormatsKHR {};
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR {};
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR      vkGetPhysicalDeviceSurfaceSupportKHR {};
    PFN_vkGetSwapchainImagesKHR                   vkGetSwapchainImagesKHR {};
    PFN_vkQueuePresentKHR                         vkQueuePresentKHR {};
};

struct DeviceDispatch : InstanceDispatch {
    PFN_vkAcquireNextImageKHR                    vkAcquireNextImageKHR {};
    PFN_vkAllocateCommandBuffers                 vkAllocateCommandBuffers {};
    PFN_vkAllocateDescriptorSets                 vkAllocateDescriptorSets {};
    PFN_vkAllocateMemory                         vkAllocateMemory {};
    PFN_vkBeginCommandBuffer                     vkBeginCommandBuffer {};
    PFN_vkBindBufferMemory                       vkBindBufferMemory {};
    PFN_vkBindImageMemory                        vkBindImageMemory {};
    PFN_vkCmdBeginDebugUtilsLabelEXT             vkCmdBeginDebugUtilsLabelEXT {};
    PFN_vkCmdBeginQuery                          vkCmdBeginQuery {};
    PFN_vkCmdBeginRenderPass                     vkCmdBeginRenderPass {};
    PFN_vkCmdBindDescriptorSets                  vkCmdBindDescriptorSets {};
    PFN_vkCmdBindIndexBuffer                     vkCmdBindIndexBuffer {};
    PFN_vkCmdBindPipeline                        vkCmdBindPipeline {};
    PFN_vkCmdBindVertexBuffers                   vkCmdBindVertexBuffers {};
    PFN_vkCmdBlitImage                           vkCmdBlitImage {};
    PFN_vkCmdClearColorImage                     vkCmdClearColorImage {};
    PFN_vkCmdClearAttachments                    vkCmdClearAttachments {};
    PFN_vkCmdCopyBuffer                          vkCmdCopyBuffer {};
    PFN_vkCmdCopyBufferToImage                   vkCmdCopyBufferToImage {};
    PFN_vkCmdCopyImage                           vkCmdCopyImage {};
    PFN_vkCmdCopyImageToBuffer                   vkCmdCopyImageToBuffer {};
    PFN_vkCmdDispatch                            vkCmdDispatch {};
    PFN_vkCmdDraw                                vkCmdDraw {};
    PFN_vkCmdDrawIndexed                         vkCmdDrawIndexed {};
    PFN_vkCmdEndDebugUtilsLabelEXT               vkCmdEndDebugUtilsLabelEXT {};
    PFN_vkCmdEndQuery                            vkCmdEndQuery {};
    PFN_vkCmdEndRenderPass                       vkCmdEndRenderPass {};
    PFN_vkCmdFillBuffer                          vkCmdFillBuffer {};
    PFN_vkCmdPipelineBarrier                     vkCmdPipelineBarrier {};
    PFN_vkCmdPushConstants                       vkCmdPushConstants {};
    PFN_vkCmdPushDescriptorSetKHR                vkCmdPushDescriptorSetKHR {};
    PFN_vkCmdPushDescriptorSetWithTemplateKHR    vkCmdPushDescriptorSetWithTemplateKHR {};
    PFN_vkCmdResolveImage                        vkCmdResolveImage {};
    PFN_vkCmdSetBlendConstants                   vkCmdSetBlendConstants {};
    PFN_vkCmdSetDepthBias                        vkCmdSetDepthBias {};
    PFN_vkCmdSetDepthBounds                      vkCmdSetDepthBounds {};
    PFN_vkCmdSetEvent                            vkCmdSetEvent {};
    PFN_vkCmdSetLineWidth                        vkCmdSetLineWidth {};
    PFN_vkCmdSetScissor                          vkCmdSetScissor {};
    PFN_vkCmdSetStencilCompareMask               vkCmdSetStencilCompareMask {};
    PFN_vkCmdSetStencilReference                 vkCmdSetStencilReference {};
    PFN_vkCmdSetStencilWriteMask                 vkCmdSetStencilWriteMask {};
    PFN_vkCmdSetViewport                         vkCmdSetViewport {};
    PFN_vkCmdWaitEvents                          vkCmdWaitEvents {};
    PFN_vkCreateBuffer                           vkCreateBuffer {};
    PFN_vkCreateBufferView                       vkCreateBufferView {};
    PFN_vkCreateCommandPool                      vkCreateCommandPool {};
    PFN_vkCreateComputePipelines                 vkCreateComputePipelines {};
    PFN_vkCreateDescriptorPool                   vkCreateDescriptorPool {};
    PFN_vkCreateDescriptorSetLayout              vkCreateDescriptorSetLayout {};
    PFN_vkCreateDescriptorUpdateTemplateKHR      vkCreateDescriptorUpdateTemplateKHR {};
    PFN_vkCreateEvent                            vkCreateEvent {};
    PFN_vkCreateFence                            vkCreateFence {};
    PFN_vkCreateFramebuffer                      vkCreateFramebuffer {};
    PFN_vkCreateGraphicsPipelines                vkCreateGraphicsPipelines {};
    PFN_vkCreateImage                            vkCreateImage {};
    PFN_vkCreateImageView                        vkCreateImageView {};
    PFN_vkCreatePipelineLayout                   vkCreatePipelineLayout {};
    PFN_vkCreateQueryPool                        vkCreateQueryPool {};
    PFN_vkCreateRenderPass                       vkCreateRenderPass {};
    PFN_vkCreateSampler                          vkCreateSampler {};
    PFN_vkCreateSemaphore                        vkCreateSemaphore {};
    PFN_vkCreateShaderModule                     vkCreateShaderModule {};
    PFN_vkCreateSwapchainKHR                     vkCreateSwapchainKHR {};
    PFN_vkDestroyBuffer                          vkDestroyBuffer {};
    PFN_vkDestroyBufferView                      vkDestroyBufferView {};
    PFN_vkDestroyCommandPool                     vkDestroyCommandPool {};
    PFN_vkDestroyDescriptorPool                  vkDestroyDescriptorPool {};
    PFN_vkDestroyDescriptorSetLayout             vkDestroyDescriptorSetLayout {};
    PFN_vkDestroyDescriptorUpdateTemplateKHR     vkDestroyDescriptorUpdateTemplateKHR {};
    PFN_vkDestroyEvent                           vkDestroyEvent {};
    PFN_vkDestroyFence                           vkDestroyFence {};
    PFN_vkDestroyFramebuffer                     vkDestroyFramebuffer {};
    PFN_vkDestroyImage                           vkDestroyImage {};
    PFN_vkDestroyImageView                       vkDestroyImageView {};
    PFN_vkDestroyPipeline                        vkDestroyPipeline {};
    PFN_vkDestroyPipelineLayout                  vkDestroyPipelineLayout {};
    PFN_vkDestroyQueryPool                       vkDestroyQueryPool {};
    PFN_vkDestroyRenderPass                      vkDestroyRenderPass {};
    PFN_vkDestroySampler                         vkDestroySampler {};
    PFN_vkDestroySemaphore                       vkDestroySemaphore {};
    PFN_vkDestroyShaderModule                    vkDestroyShaderModule {};
    PFN_vkDestroySwapchainKHR                    vkDestroySwapchainKHR {};
    PFN_vkDeviceWaitIdle                         vkDeviceWaitIdle {};
    PFN_vkEndCommandBuffer                       vkEndCommandBuffer {};
    PFN_vkFreeCommandBuffers                     vkFreeCommandBuffers {};
    PFN_vkFreeDescriptorSets                     vkFreeDescriptorSets {};
    PFN_vkFreeMemory                             vkFreeMemory {};
    PFN_vkGetBufferMemoryRequirements2           vkGetBufferMemoryRequirements2 {};
    PFN_vkGetDeviceQueue                         vkGetDeviceQueue {};
    PFN_vkGetEventStatus                         vkGetEventStatus {};
    PFN_vkGetFenceStatus                         vkGetFenceStatus {};
    PFN_vkGetImageMemoryRequirements             vkGetImageMemoryRequirements {};
    PFN_vkGetImageSubresourceLayout              vkGetImageSubresourceLayout {};
    PFN_vkGetMemoryFdKHR                         vkGetMemoryFdKHR {};
    PFN_vkGetSemaphoreFdKHR                      vkGetSemaphoreFdKHR {};
    PFN_vkGetImageDrmFormatModifierPropertiesEXT vkGetImageDrmFormatModifierPropertiesEXT {};
    PFN_vkGetPipelineExecutablePropertiesKHR     vkGetPipelineExecutablePropertiesKHR {};
    PFN_vkGetPipelineExecutableStatisticsKHR     vkGetPipelineExecutableStatisticsKHR {};
    PFN_vkGetQueryPoolResults                    vkGetQueryPoolResults {};
    PFN_vkGetSemaphoreCounterValueKHR            vkGetSemaphoreCounterValueKHR {};
    PFN_vkMapMemory                              vkMapMemory {};
    PFN_vkQueueSubmit                            vkQueueSubmit {};
    PFN_vkResetFences                            vkResetFences {};
    PFN_vkUnmapMemory                            vkUnmapMemory {};
    PFN_vkUpdateDescriptorSetWithTemplateKHR     vkUpdateDescriptorSetWithTemplateKHR {};
    PFN_vkUpdateDescriptorSets                   vkUpdateDescriptorSets {};
    PFN_vkWaitForFences                          vkWaitForFences {};
    PFN_vkWaitSemaphoresKHR                      vkWaitSemaphoresKHR {};

    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT {};
    PFN_vkSetDebugUtilsObjectTagEXT  vkSetDebugUtilsObjectTagEXT {};
};

template<typename THandle, typename Type = typename THandle::handle_type>
std::vector<Type> ToVector(std::span<THandle> handles) {
    std::vector<Type> res(handles.size());
    std::transform(handles.begin(), handles.end(), res.begin(), [](const auto& h) {
        return *h;
    });
    return res;
}

template<typename AllocationType, typename PoolType>
class PoolAllocations {
public:
    PoolAllocations() = default;

    explicit PoolAllocations(std::unique_ptr<AllocationType[]> allocations_, std::size_t num_,
                             VkDevice device_, PoolType pool_,
                             const DeviceDispatch& dld_) noexcept
        : allocations { std::move(allocations_) },
          num { num_ },
          device { device_ },
          pool { pool_ },
          dld { &dld_ } {}

    PoolAllocations(const PoolAllocations&)            = delete;
    PoolAllocations& operator=(const PoolAllocations&) = delete;

    PoolAllocations(PoolAllocations&& rhs) noexcept
        : allocations { std::move(rhs.allocations) },
          num { rhs.num },
          device { rhs.device },
          pool { rhs.pool },
          dld { rhs.dld } {}

    PoolAllocations& operator=(PoolAllocations&& rhs) noexcept {
        Release();
        allocations = std::move(rhs.allocations);
        num         = rhs.num;
        device      = rhs.device;
        pool        = rhs.pool;
        dld         = rhs.dld;
        return *this;
    }

    ~PoolAllocations() { Release(); }

    std::size_t           size() const noexcept { return num; }
    AllocationType const* data() const noexcept { return allocations.get(); }
    AllocationType        operator[](std::size_t index) const noexcept { return allocations[index]; }
    bool                  IsOutOfPoolMemory() const noexcept { return ! device; }

private:
    void Release() noexcept {
        if (! allocations) return;
        const Span<AllocationType> span(allocations.get(), num);
        VVK_CHECK(Free(device, pool, span, *dld));
    }

    std::unique_ptr<AllocationType[]> allocations;
    std::size_t                       num    = 0;
    VkDevice                          device = nullptr;
    PoolType                          pool   = nullptr;
    const DeviceDispatch*             dld    = nullptr;
};

VkResult LoadLibrary(utils::DynamicLibrary&, vvk::InstanceDispatch&);

bool Load(InstanceDispatch&) noexcept;
bool Load(VkInstance, InstanceDispatch&) noexcept;
bool Load(VkDevice, InstanceDispatch&) noexcept;
bool Load(VkDevice, DeviceDispatch&) noexcept;

void Destroy(VkInstance, const InstanceDispatch&) noexcept;
void Destroy(VkDevice, const InstanceDispatch&) noexcept;
void Destroy(VkInstance, VkDebugUtilsMessengerEXT, const InstanceDispatch&) noexcept;
void Destroy(VkInstance, VkSurfaceKHR, const InstanceDispatch&) noexcept;
void Destroy(VkDevice, VkCommandPool, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkPipeline, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkPipelineLayout, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkRenderPass, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkDescriptorSetLayout, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkImage, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkImageView, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkDeviceMemory, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkShaderModule, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkSwapchainKHR, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkSampler, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkSemaphore, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkFence, const DeviceDispatch&) noexcept;
void Destroy(VkDevice, VkFramebuffer, const DeviceDispatch&) noexcept;

VkResult Free(VkDevice, VkCommandPool, Span<VkCommandBuffer>, const DeviceDispatch&) noexcept;

using DebugUtilsMessenger = Handle<VkDebugUtilsMessengerEXT, VkInstance, InstanceDispatch>;
using DescriptorSetLayout = Handle<VkDescriptorSetLayout, VkDevice, DeviceDispatch>;
using SurfaceKHR          = Handle<VkSurfaceKHR, VkInstance, InstanceDispatch>;
using Pipeline            = Handle<VkPipeline, VkDevice, DeviceDispatch>;
using PipelineLayout      = Handle<VkPipelineLayout, VkDevice, DeviceDispatch>;
using RenderPass          = Handle<VkRenderPass, VkDevice, DeviceDispatch>;
using Sampler             = Handle<VkSampler, VkDevice, DeviceDispatch>;

using DescriptorSets = PoolAllocations<VkDescriptorSet, VkDescriptorPool>;
using CommandBuffers = PoolAllocations<VkCommandBuffer, VkCommandPool>;

class PhysicalDevice;

class Instance : public Handle<VkInstance, NoOwner, InstanceDispatch> {
    using Handle<VkInstance, NoOwner, InstanceDispatch>::Handle;

public:
    static VkResult Create(Instance&, const VkApplicationInfo&, Span<const char*> layers,
                           Span<const char*> extensions, InstanceDispatch&) noexcept;

    std::vector<PhysicalDevice> EnumeratePhysicalDevices() const noexcept;

    DebugUtilsMessenger
    CreateDebugUtilsMessenger(const VkDebugUtilsMessengerCreateInfoEXT&) const noexcept;

    const InstanceDispatch& Dispatch() const noexcept { return *dld; }
};

class Buffer : public Handle<VkBuffer, VkDevice, DeviceDispatch> {
    using Handle<VkBuffer, VkDevice, DeviceDispatch>::Handle;

public:
    VkResult BindMemory(VkDeviceMemory memory, VkDeviceSize offset) const noexcept;
};

class Image : public Handle<VkImage, VkDevice, DeviceDispatch> {
    using Handle<VkImage, VkDevice, DeviceDispatch>::Handle;

public:
    VkResult BindMemory(VkDeviceMemory memory, VkDeviceSize offset) const noexcept;
};

class ImageView : public Handle<VkImageView, VkDevice, DeviceDispatch> {
    using Handle<VkImageView, VkDevice, DeviceDispatch>::Handle;
};

class Queue : public Handle<VkQueue, NoOwnerLife, DeviceDispatch> {
    using Handle<VkQueue, NoOwnerLife, DeviceDispatch>::Handle;

public:
    VkResult Submit(Span<VkSubmitInfo> submit_infos,
                    VkFence            fence = VK_NULL_HANDLE) const noexcept {
        return dld->vkQueueSubmit(
            handle, (uint32_t)submit_infos.size(), submit_infos.data(), fence);
    }

    VkResult Present(const VkPresentInfoKHR& present_info) const noexcept {
        return dld->vkQueuePresentKHR(handle, &present_info);
    }
};

class SwapchainKHR : public Handle<VkSwapchainKHR, VkDevice, DeviceDispatch> {
    using Handle<VkSwapchainKHR, VkDevice, DeviceDispatch>::Handle;

public:
    VkResult GetImages(std::vector<VkImage>&) const;
};

class PhysicalDevice : public Handle<VkPhysicalDevice, NoOwnerLife, InstanceDispatch> {
    using Handle<VkPhysicalDevice, NoOwnerLife, InstanceDispatch>::Handle;

public:
    VkPhysicalDeviceProperties GetProperties() const noexcept;

    void GetProperties2KHR(VkPhysicalDeviceProperties2KHR&) const noexcept;

    VkPhysicalDeviceFeatures GetFeatures() const noexcept;

    void GetFeatures2KHR(VkPhysicalDeviceFeatures2KHR&) const noexcept;

    VkFormatProperties GetFormatProperties(VkFormat) const noexcept;

    VkResult EnumerateDeviceExtensionProperties(std::vector<VkExtensionProperties>&) const;

    std::vector<VkQueueFamilyProperties> GetQueueFamilyProperties() const;

    VkResult GetSurfaceSupportKHR(uint32_t queue_family_index, VkSurfaceKHR, bool&) const;

    VkResult GetSurfaceCapabilitiesKHR(VkSurfaceKHR, VkSurfaceCapabilitiesKHR&) const noexcept;

    VkResult GetSurfaceFormatsKHR(VkSurfaceKHR surface, std::vector<VkSurfaceFormatKHR>&) const;

    VkResult GetSurfacePresentModesKHR(VkSurfaceKHR surface,
                                       std::vector<VkPresentModeKHR>&) const;

    VkPhysicalDeviceMemoryProperties2
    GetMemoryProperties(void* next_structures = nullptr) const noexcept;
};

class CommandPool : public Handle<VkCommandPool, VkDevice, DeviceDispatch> {
    using Handle<VkCommandPool, VkDevice, DeviceDispatch>::Handle;

public:
    VkResult Allocate(std::size_t num_buffers, VkCommandBufferLevel level,
                      CommandBuffers&) const;
};

class DeviceMemory : public Handle<VkDeviceMemory, VkDevice, DeviceDispatch> {
    using Handle<VkDeviceMemory, VkDevice, DeviceDispatch>::Handle;

public:
    VkResult GetMemoryFdKHR(int*) const;

    VkResult Map(VkDeviceSize offset, VkDeviceSize size, uint8_t** data) const {
        return (dld->vkMapMemory(owner, handle, offset, size, 0, (void**)data));
    }

    void Unmap() const noexcept { dld->vkUnmapMemory(owner, handle); }
};

class Framebuffer : public Handle<VkFramebuffer, VkDevice, DeviceDispatch> {
    using Handle<VkFramebuffer, VkDevice, DeviceDispatch>::Handle;
};

class ShaderModule : public Handle<VkShaderModule, VkDevice, DeviceDispatch> {
    using Handle<VkShaderModule, VkDevice, DeviceDispatch>::Handle;
};

class Fence : public Handle<VkFence, VkDevice, DeviceDispatch> {
    using Handle<VkFence, VkDevice, DeviceDispatch>::Handle;

public:
    VkResult Wait(uint64_t timeout = std::numeric_limits<uint64_t>::max()) const noexcept {
        return dld->vkWaitForFences(owner, 1, &handle, true, timeout);
    }

    VkResult GetStatus() const noexcept { return dld->vkGetFenceStatus(owner, handle); }

    VkResult Reset() const { return dld->vkResetFences(owner, 1, &handle); }
};

class Semaphore : public Handle<VkSemaphore, VkDevice, DeviceDispatch> {
    using Handle<VkSemaphore, VkDevice, DeviceDispatch>::Handle;

public:
    VkResult GetCounter(uint64_t* value) const {
        return dld->vkGetSemaphoreCounterValueKHR(owner, handle, value);
    }

    VkResult Wait(uint64_t value, uint64_t timeout = std::numeric_limits<uint64_t>::max()) const {
        const VkSemaphoreWaitInfoKHR wait_info {
            .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR,
            .pNext          = nullptr,
            .flags          = 0,
            .semaphoreCount = 1,
            .pSemaphores    = &handle,
            .pValues        = &value,
        };
        return dld->vkWaitSemaphoresKHR(owner, &wait_info, timeout);
    }
};

class Device : public Handle<VkDevice, NoOwner, DeviceDispatch> {
    using Handle<VkDevice, NoOwner, DeviceDispatch>::Handle;

public:
    static VkResult Create(Device&, VkPhysicalDevice physical_device,
                           Span<const VkDeviceQueueCreateInfo> queues_ci,
                           Span<const char*> enabled_extensions, const void* next,
                           DeviceDispatch& dispatch);

    Queue GetQueue(uint32_t family_index) const noexcept;

    VkMemoryRequirements GetImageMemoryRequirements(VkImage image) const noexcept;

    VkSubresourceLayout GetImageSubresourceLayout(VkImage                   image,
                                                  const VkImageSubresource& subresource) const noexcept;

    VkResult GetImageDrmFormatModifierPropertiesEXT(
        VkImage image, VkImageDrmFormatModifierPropertiesEXT* props) const noexcept;

    VkResult AllocateMemory(const VkMemoryAllocateInfo& ai, DeviceMemory&) const noexcept;

    VkResult CreateCommandPool(const VkCommandPoolCreateInfo& ci, CommandPool&) const;
    VkResult CreateDescriptorSetLayout(const VkDescriptorSetLayoutCreateInfo& ci,
                                       DescriptorSetLayout&) const noexcept;
    VkResult CreateGraphicsPipeline(const VkGraphicsPipelineCreateInfo& ci,
                                    Pipeline&) const noexcept;

    VkResult CreateRenderPass(const VkRenderPassCreateInfo& ci, RenderPass&) const noexcept;

    VkResult CreatePipelineLayout(const VkPipelineLayoutCreateInfo& ci,
                                  PipelineLayout&) const noexcept;

    VkResult CreateSwapchainKHR(const VkSwapchainCreateInfoKHR& ci,
                                SwapchainKHR&) const noexcept;

    VkResult CreateShaderModule(const VkShaderModuleCreateInfo& ci, ShaderModule&) const noexcept;

    VkResult CreateSemaphore(const VkSemaphoreCreateInfo& ci, Semaphore&) const noexcept;
    VkResult GetSemaphoreFdKHR(const VkSemaphoreGetFdInfoKHR& gi, int* fd) const noexcept;

    VkResult CreateImage(const VkImageCreateInfo& ci, Image&) const noexcept;

    VkResult CreateImageView(const VkImageViewCreateInfo& ci, ImageView&) const noexcept;

    VkResult CreateFramebuffer(const VkFramebufferCreateInfo& ci, Framebuffer&) const noexcept;

    VkResult CreateFence(const VkFenceCreateInfo& ci, Fence&) const noexcept;

    VkResult CreateSampler(const VkSamplerCreateInfo& ci, Sampler&) const noexcept;

    VkResult WaitIdle() const noexcept { return dld->vkDeviceWaitIdle(handle); }

    VkResult AcquireNextImageKHR(VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                                 VkFence fence, uint32_t* image_index) const noexcept {
        return dld->vkAcquireNextImageKHR(
            handle, swapchain, timeout, semaphore, fence, image_index);
    }

    const DeviceDispatch& Dispatch() const noexcept { return *dld; }
};

class CommandBuffer : public Handle<VkCommandBuffer, NoOwnerLife, DeviceDispatch> {
    using Handle<VkCommandBuffer, NoOwnerLife, DeviceDispatch>::Handle;

public:
    VkResult Begin(const VkCommandBufferBeginInfo& begin_info) const {
        return dld->vkBeginCommandBuffer(handle, &begin_info);
    }

    VkResult End() const { return dld->vkEndCommandBuffer(handle); }

    void BeginRenderPass(const VkRenderPassBeginInfo& renderpass_bi,
                         VkSubpassContents            contents) const noexcept {
        dld->vkCmdBeginRenderPass(handle, &renderpass_bi, contents);
    }

    void EndRenderPass() const noexcept { dld->vkCmdEndRenderPass(handle); }

    void BeginQuery(VkQueryPool query_pool, uint32_t query,
                    VkQueryControlFlags flags) const noexcept {
        dld->vkCmdBeginQuery(handle, query_pool, query, flags);
    }

    void EndQuery(VkQueryPool query_pool, uint32_t query) const noexcept {
        dld->vkCmdEndQuery(handle, query_pool, query);
    }

    void BindDescriptorSets(VkPipelineBindPoint bind_point, VkPipelineLayout layout,
                            uint32_t              first,
                            Span<VkDescriptorSet> sets,
                            Span<uint32_t>        dynamic_offsets) const noexcept {
        dld->vkCmdBindDescriptorSets(handle,
                                     bind_point,
                                     layout,
                                     first,
                                     sets.size(),
                                     sets.data(),
                                     dynamic_offsets.size(),
                                     dynamic_offsets.data());
    }

    void PushDescriptorSetKHR(VkPipelineBindPoint bind_point, VkPipelineLayout layout,
                              uint32_t                         set,
                              Span<const VkWriteDescriptorSet> wsets) const noexcept {
        assert(wsets[0].sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        dld->vkCmdPushDescriptorSetKHR(
            handle, bind_point, layout, set, wsets.size(), wsets.data());
    }

    void PushDescriptorSetWithTemplateKHR(VkDescriptorUpdateTemplateKHR update_template,
                                          VkPipelineLayout layout, uint32_t set,
                                          const void* data) const noexcept {
        dld->vkCmdPushDescriptorSetWithTemplateKHR(handle, update_template, layout, set, data);
    }

    void BindPipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) const noexcept {
        dld->vkCmdBindPipeline(handle, bind_point, pipeline);
    }

    void BindIndexBuffer(VkBuffer buffer, VkDeviceSize offset,
                         VkIndexType index_type) const noexcept {
        dld->vkCmdBindIndexBuffer(handle, buffer, offset, index_type);
    }

    void BindVertexBuffers(uint32_t first, uint32_t count, const VkBuffer* buffers,
                           const VkDeviceSize* offsets) const noexcept {
        dld->vkCmdBindVertexBuffers(handle, first, count, buffers, offsets);
    }

    void BindVertexBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset) const noexcept {
        BindVertexBuffers(binding, 1, &buffer, &offset);
    }

    void Draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
              uint32_t first_instance) const noexcept {
        dld->vkCmdDraw(handle, vertex_count, instance_count, first_vertex, first_instance);
    }

    void DrawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                     int32_t vertex_offset, uint32_t first_instance) const noexcept {
        dld->vkCmdDrawIndexed(
            handle, index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    void ClearColorImage(VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor,
                         Span<const VkImageSubresourceRange> ranges) const noexcept {
        return dld->vkCmdClearColorImage(
            handle, image, imageLayout, pColor, ranges.size(), ranges.data());
    }

    void ClearAttachments(Span<VkClearAttachment> attachments,
                          Span<VkClearRect>       rects) const noexcept {
        dld->vkCmdClearAttachments(
            handle, attachments.size(), attachments.data(), rects.size(), rects.data());
    }

    void BlitImage(VkImage src_image, VkImageLayout src_layout, VkImage dst_image,
                   VkImageLayout dst_layout, Span<VkImageBlit> regions,
                   VkFilter filter) const noexcept {
        dld->vkCmdBlitImage(handle,
                            src_image,
                            src_layout,
                            dst_image,
                            dst_layout,
                            regions.size(),
                            regions.data(),
                            filter);
    }

    void ResolveImage(VkImage src_image, VkImageLayout src_layout, VkImage dst_image,
                      VkImageLayout dst_layout, Span<VkImageResolve> regions) {
        dld->vkCmdResolveImage(
            handle, src_image, src_layout, dst_image, dst_layout, regions.size(), regions.data());
    }

    void Dispatch(uint32_t x, uint32_t y, uint32_t z) const noexcept {
        dld->vkCmdDispatch(handle, x, y, z);
    }

    void PipelineBarrier(VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
                         VkDependencyFlags                 dependency_flags,
                         Span<const VkMemoryBarrier>       memory_barriers,
                         Span<const VkBufferMemoryBarrier> buffer_barriers,
                         Span<const VkImageMemoryBarrier>  image_barriers) const noexcept {
        dld->vkCmdPipelineBarrier(handle,
                                  src_stage_mask,
                                  dst_stage_mask,
                                  dependency_flags,
                                  memory_barriers.size(),
                                  memory_barriers.data(),
                                  buffer_barriers.size(),
                                  buffer_barriers.data(),
                                  image_barriers.size(),
                                  image_barriers.data());
    }

    void PipelineBarrier(VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
                         VkDependencyFlags dependency_flags = 0) const noexcept {
        PipelineBarrier(src_stage_mask, dst_stage_mask, dependency_flags, {}, {}, {});
    }

    void PipelineBarrier(VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
                         VkDependencyFlags      dependency_flags,
                         const VkMemoryBarrier& memory_barrier) const noexcept {
        PipelineBarrier(src_stage_mask, dst_stage_mask, dependency_flags, memory_barrier, {}, {});
    }

    void PipelineBarrier(VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
                         VkDependencyFlags            dependency_flags,
                         const VkBufferMemoryBarrier& buffer_barrier) const noexcept {
        PipelineBarrier(src_stage_mask, dst_stage_mask, dependency_flags, {}, buffer_barrier, {});
    }

    void PipelineBarrier(VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
                         VkDependencyFlags           dependency_flags,
                         const VkImageMemoryBarrier& image_barrier) const noexcept {
        PipelineBarrier(src_stage_mask, dst_stage_mask, dependency_flags, {}, {}, image_barrier);
    }

    void CopyBufferToImage(VkBuffer src_buffer, VkImage dst_image, VkImageLayout dst_image_layout,
                           Span<VkBufferImageCopy> regions) const noexcept {
        dld->vkCmdCopyBufferToImage(
            handle, src_buffer, dst_image, dst_image_layout, regions.size(), regions.data());
    }

    void CopyBuffer(VkBuffer src_buffer, VkBuffer dst_buffer,
                    Span<VkBufferCopy> regions) const noexcept {
        dld->vkCmdCopyBuffer(handle, src_buffer, dst_buffer, regions.size(), regions.data());
    }

    void CopyImage(VkImage src_image, VkImageLayout src_layout, VkImage dst_image,
                   VkImageLayout dst_layout, Span<VkImageCopy> regions) const noexcept {
        dld->vkCmdCopyImage(
            handle, src_image, src_layout, dst_image, dst_layout, regions.size(), regions.data());
    }

    void CopyImageToBuffer(VkImage src_image, VkImageLayout src_layout, VkBuffer dst_buffer,
                           Span<VkBufferImageCopy> regions) const noexcept {
        dld->vkCmdCopyImageToBuffer(
            handle, src_image, src_layout, dst_buffer, regions.size(), regions.data());
    }

    void FillBuffer(VkBuffer dst_buffer, VkDeviceSize dst_offset, VkDeviceSize size,
                    uint32_t data) const noexcept {
        dld->vkCmdFillBuffer(handle, dst_buffer, dst_offset, size, data);
    }

    void PushConstants(VkPipelineLayout layout, VkShaderStageFlags flags, uint32_t offset,
                       uint32_t size, const void* values) const noexcept {
        dld->vkCmdPushConstants(handle, layout, flags, offset, size, values);
    }

    template<typename T>
    void PushConstants(VkPipelineLayout layout, VkShaderStageFlags flags,
                       const T& data) const noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "<data> is not trivially copyable");
        dld->vkCmdPushConstants(
            handle, layout, flags, 0, static_cast<uint32_t>(sizeof(T)), &data);
    }

    void SetViewport(uint32_t first, Span<VkViewport> viewports) const noexcept {
        dld->vkCmdSetViewport(handle, first, viewports.size(), viewports.data());
    }

    void SetScissor(uint32_t first, Span<VkRect2D> scissors) const noexcept {
        dld->vkCmdSetScissor(handle, first, scissors.size(), scissors.data());
    }

    void SetBlendConstants(const float blend_constants[4]) const noexcept {
        dld->vkCmdSetBlendConstants(handle, blend_constants);
    }

    void SetStencilCompareMask(VkStencilFaceFlags face_mask,
                               uint32_t           compare_mask) const noexcept {
        dld->vkCmdSetStencilCompareMask(handle, face_mask, compare_mask);
    }

    void SetStencilReference(VkStencilFaceFlags face_mask, uint32_t reference) const noexcept {
        dld->vkCmdSetStencilReference(handle, face_mask, reference);
    }

    void SetStencilWriteMask(VkStencilFaceFlags face_mask, uint32_t write_mask) const noexcept {
        dld->vkCmdSetStencilWriteMask(handle, face_mask, write_mask);
    }

    void SetDepthBias(float constant_factor, float clamp, float slope_factor) const noexcept {
        dld->vkCmdSetDepthBias(handle, constant_factor, clamp, slope_factor);
    }

    void SetDepthBounds(float min_depth_bounds, float max_depth_bounds) const noexcept {
        dld->vkCmdSetDepthBounds(handle, min_depth_bounds, max_depth_bounds);
    }

    void SetEvent(VkEvent event, VkPipelineStageFlags stage_flags) const noexcept {
        dld->vkCmdSetEvent(handle, event, stage_flags);
    }

    void WaitEvents(Span<VkEvent> events, VkPipelineStageFlags src_stage_mask,
                    VkPipelineStageFlags        dst_stage_mask,
                    Span<VkMemoryBarrier>       memory_barriers,
                    Span<VkBufferMemoryBarrier> buffer_barriers,
                    Span<VkImageMemoryBarrier>  image_barriers) const noexcept {
        dld->vkCmdWaitEvents(handle,
                             events.size(),
                             events.data(),
                             src_stage_mask,
                             dst_stage_mask,
                             memory_barriers.size(),
                             memory_barriers.data(),
                             buffer_barriers.size(),
                             buffer_barriers.data(),
                             image_barriers.size(),
                             image_barriers.data());
    }

    void BeginDebugUtilsLabelEXT(const char* label, std::span<float, 4> color) const noexcept {
        const VkDebugUtilsLabelEXT label_info {
            .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
            .pNext      = nullptr,
            .pLabelName = label,
            .color { color[0], color[1], color[2], color[3] },
        };
        dld->vkCmdBeginDebugUtilsLabelEXT(handle, &label_info);
    }

    void EndDebugUtilsLabelEXT() const noexcept { dld->vkCmdEndDebugUtilsLabelEXT(handle); }
};

std::optional<std::vector<VkExtensionProperties>>
EnumerateInstanceExtensionProperties(const InstanceDispatch& dld);

std::optional<std::vector<VkLayerProperties>>
EnumerateInstanceLayerProperties(const InstanceDispatch& dld);

// ---------- vma_wrapper.hpp ----------

struct VmaOwner {
    VmaAllocator      allocator {};
    VmaAllocation     allocation {};
    VmaAllocationInfo allocationInfo {};

    VmaOwner()  = default;
    ~VmaOwner() = default;

    VmaOwner(std::nullptr_t) {}
    VmaOwner& operator=(std::nullptr_t) {
        allocator      = {};
        allocation     = {};
        allocationInfo = {};
        return *this;
    }
};

inline void Destroy(VmaAllocator allocator, int) { vmaDestroyAllocator(allocator); }
inline void Destroy(VmaOwner owner, VkBuffer handle, int) {
    vmaDestroyBuffer(owner.allocator, handle, owner.allocation);
}
inline void Destroy(VmaOwner owner, VkImage handle, int) {
    vmaDestroyImage(owner.allocator, handle, owner.allocation);
}

class VmaAllocatorHandle : NoCopy {
public:
    VmaAllocatorHandle() = default;
    VmaAllocatorHandle(VmaAllocator vma, int): m_allocator(vma) {}
    ~VmaAllocatorHandle() { Release(); }

    VmaAllocatorHandle(VmaAllocatorHandle&& o)
        : m_allocator(std::exchange(o.m_allocator, nullptr)) {}
    VmaAllocatorHandle& operator=(VmaAllocatorHandle&& o) noexcept {
        Release();
        m_allocator = std::exchange(o.m_allocator, nullptr);
        return *this;
    }

    const auto& operator*() const noexcept { return m_allocator; }

private:
    void Release() {
        if (m_allocator) Destroy(m_allocator, 0);
    }
    VmaAllocator m_allocator {};
};

class VmaBuffer : public Handle<VkBuffer, VmaOwner, int> {
    using Handle<VkBuffer, VmaOwner, int>::Handle;

public:
    VmaAllocation Allocation() const noexcept { return owner.allocation; }
    VkResult MapMemory(void** data) const {
        return vmaMapMemory(owner.allocator, owner.allocation, data);
    }
    void UnMapMemory() { vmaUnmapMemory(owner.allocator, owner.allocation); }
};

class VmaImage : public Handle<VkImage, VmaOwner, int> {
    using Handle<VkImage, VmaOwner, int>::Handle;

public:
    VmaAllocation Allocation() const noexcept { return owner.allocation; }
    VkResult MapMemory(void** data) const {
        return vmaMapMemory(owner.allocator, owner.allocation, data);
    }
    void UnMapMemory() { vmaUnmapMemory(owner.allocator, owner.allocation); }
};

constexpr inline int empty_int { 0 };

inline VkResult CreateVmaAllocator(const VmaAllocatorCreateInfo& ci,
                                   VmaAllocatorHandle&           allocator_h) noexcept {
    VmaAllocator object;
    auto         res = vmaCreateAllocator(&ci, &object);
    if (res == VK_SUCCESS) allocator_h = VmaAllocatorHandle(object, 0);
    return res;
}

inline VkResult CreateBuffer(const VmaAllocator& vma_allocator, const VkBufferCreateInfo& ci,
                             const VmaAllocationCreateInfo& vma_info,
                             VmaBuffer&                     buffer) noexcept {
    VkBuffer object;
    VmaOwner owner;
    owner.allocator = vma_allocator;

    auto res = vmaCreateBuffer(
        vma_allocator, &ci, &vma_info, &object, &owner.allocation, &owner.allocationInfo);
    if (res == VK_SUCCESS) buffer = VmaBuffer(object, owner, empty_int);
    return res;
}

inline VkResult CreateImage(const VmaAllocator& vma_allocator, const VkImageCreateInfo& ci,
                            const VmaAllocationCreateInfo& vma_info,
                            VmaImage&                      vma_img) noexcept {
    VkImage  object;
    VmaOwner owner;
    owner.allocator = vma_allocator;

    auto res = vmaCreateImage(
        vma_allocator, &ci, &vma_info, &object, &owner.allocation, &owner.allocationInfo);
    if (res == VK_SUCCESS) vma_img = VmaImage(object, owner, empty_int);
    return res;
}

} // namespace vvk (export)

// =================================================================
// Layer 2: wallpaper::vulkan:: high-level wrapper
// =================================================================

export namespace wallpaper
{

namespace vulkan
{

// ---------- Instance.hpp ----------

struct Extension {
    bool             required { false };
    std::string_view name;
};

using InstanceLayer = Extension;

using CheckGpuOp = std::function<bool(vvk::PhysicalDevice)>;

constexpr std::string_view VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";

constexpr uint32_t    WP_VULKAN_VERSION { VK_API_VERSION_1_1 };
constexpr const char* WP_APPLICATION_NAME { "scene render" };

class Device;
class Instance {
public:
    Instance()  = default;
    ~Instance() = default;

    void Destroy();

    static bool Create(Instance&, std::span<const Extension>, std::span<const InstanceLayer>);
    bool        ChoosePhysicalDevice(const CheckGpuOp&             checkgpu,
                                     std::span<const std::uint8_t> uuid = {});

    const vvk::Instance&       inst() const;
    const vvk::PhysicalDevice& gpu() const;
    const vvk::SurfaceKHR&     surface() const;

    bool offscreen() const;
    void setSurface(VkSurfaceKHR);
    bool supportExt(std::string_view) const;
    bool supportLayer(std::string_view) const;

private:
    utils::DynamicLibrary m_vklib;
    vvk::InstanceDispatch m_dld;
    vvk::Instance         m_vinst;

    vvk::DebugUtilsMessenger m_debug_utils;
    vvk::PhysicalDevice      m_gpu {};

    vvk::SurfaceKHR  m_surface {};
    Set<std::string> m_extensions;
    Set<std::string> m_layers;
};

// ShaderSpv / Uni_ShaderSpv now live in wescene.shader_compile (re-exported above).

// ---------- Parameters.hpp ----------

struct QueueParameters {
    vvk::Queue handle;
    uint32_t   family_index;
};

struct VmaBufferParameters {
    vvk::VmaBuffer handle;
    std::size_t    req_size;

    VmaBufferParameters();
    ~VmaBufferParameters();
    VmaBufferParameters(VmaBufferParameters&& o) noexcept;
    VmaBufferParameters& operator=(VmaBufferParameters&& o) noexcept;
};

struct BufferParameters {
    VkBuffer    handle;
    std::size_t req_size;
    BufferParameters()  = default;
    ~BufferParameters() = default;
    BufferParameters(const VmaBufferParameters& o) noexcept
        : handle(*o.handle), req_size(o.req_size) {}
};

struct VmaImageParameters : NoCopy {
    vvk::VmaImage  handle;
    vvk::ImageView view;
    vvk::Sampler   sampler;
    VkExtent3D     extent;
    uint           mipmap_level { 1 };

    VmaImageParameters();
    ~VmaImageParameters();
    VmaImageParameters(VmaImageParameters&& o) noexcept;
    VmaImageParameters& operator=(VmaImageParameters&& o) noexcept;
};

struct ExImageParameters : NoCopy {
    vvk::DeviceMemory    mem {};
    VkMemoryRequirements mem_reqs {};

    vvk::Image     handle;
    vvk::ImageView view;
    vvk::Sampler   sampler;
    VkExtent3D     extent;
    uint           mipmap_level { 1 };
    int            fd { 0 };

    uint32_t drm_fourcc { 0 };
    uint64_t drm_modifier { 0 };
    uint64_t plane0_offset { 0 };
    uint32_t plane0_stride { 0 };

    ExImageParameters();
    ~ExImageParameters();
    ExImageParameters(ExImageParameters&& o) noexcept;
    ExImageParameters& operator=(ExImageParameters&& o) noexcept;
};

// `ImageParameters` itself is global-attached (defined in classic
// Swapchain/ExSwapchain.hpp). These free helpers replace the conversion
// ctors that used to live on it — those ctors needed module-attached
// Vma/Ex types which can't be visible in classic purview.
inline ImageParameters ToImageParameters(const VmaImageParameters& o) noexcept {
    ImageParameters out;
    out.handle       = *o.handle;
    out.view         = *o.view;
    out.sampler      = *o.sampler;
    out.extent       = o.extent;
    out.mipmap_level = o.mipmap_level;
    return out;
}
inline ImageParameters ToImageParameters(const ExImageParameters& o) noexcept {
    ImageParameters out;
    out.handle       = *o.handle;
    out.view         = *o.view;
    out.sampler      = *o.sampler;
    out.extent       = o.extent;
    out.mipmap_level = o.mipmap_level;
    return out;
}

struct ImageSlots : NoCopy {
    std::vector<VmaImageParameters> slots;

    ImageSlots();
    ~ImageSlots();
    ImageSlots(ImageSlots&& o) noexcept;
    ImageSlots& operator=(ImageSlots&& o) noexcept;
};

struct ImageSlotsRef {
    std::vector<ImageParameters> slots;

    idx active { 0 };

    auto& getActive() const {
        if (active > 0 && active >= std::ssize(slots)) return slots[0];
        return slots[(usize)active];
    }
    ImageSlotsRef();
    ~ImageSlotsRef();
    ImageSlotsRef(const ImageSlots&);
};

// ---------- Swapchain.hpp ----------

class Swapchain {
public:
    static bool                      Create(Device&, VkSurfaceKHR, VkExtent2D, Swapchain&);
    const vvk::SwapchainKHR&         handle() const;
    VkFormat                         format() const;
    VkExtent2D                       extent() const;
    VkPresentModeKHR                 presentMode() const;
    std::span<const ImageParameters> images() const;

private:
    vvk::SwapchainKHR            m_handle;
    VkSurfaceFormatKHR           m_format;
    VkExtent2D                   m_extent;
    VkPresentModeKHR             m_present_mode;
    std::vector<ImageParameters> m_images;
    std::vector<vvk::ImageView>  m_imageviews;
};

// ---------- TextureCache.hpp ----------

VkFormat             ToVkType(TextureFormat);
VkSamplerAddressMode ToVkType(TextureWrap);
VkFilter             ToVkType(TextureFilter);

enum class TexUsage
{
    COLOR,
    DEPTH
};

using TexHash = std::size_t;

struct TextureKey {
    i32           width;
    i32           height;
    TexUsage      usage;
    TextureFormat format;
    TextureSample sample;
    uint          mipmap_level { 1 };

    static TexHash HashValue(const TextureKey&);
};

class TextureCache : NoCopy, NoMove {
public:
    TextureCache(const Device&);
    ~TextureCache();

    void Clear();

    std::optional<ExImageParameters> CreateExTex(uint32_t witdh, uint32_t height, VkFormat,
                                                 VkImageTiling);
    ImageSlotsRef                    CreateTex(Image&);

    std::optional<ImageParameters> Query(std::string_view key, TextureKey content_hash,
                                         bool persist = false);

    void MarkShareReady(std::string_view key);

    void RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const;

private:
    std::optional<VmaImageParameters> CreateTex(TextureKey);
    void                              allocateCmd();
    vvk::CommandBuffers               m_tex_cmds;
    vvk::CommandBuffer                m_tex_cmd;

    const Device&                m_device;
    Map<std::string, ImageSlots> m_tex_map;

    struct QueryTex {
        idx                index { 0 };
        bool               share_ready { false };
        bool               persist { false };
        TexHash            content_hash;
        VmaImageParameters image;
        Set<std::string>   query_keys;
    };
    std::vector<std::unique_ptr<QueryTex>> m_query_texs;
    Map<std::string, QueryTex*>            m_query_map;
};

// ---------- Device.hpp ----------

class PipelineParameters;

class Device : NoCopy, NoMove {
public:
    Device();
    ~Device();

    static bool Create(Instance&, std::span<const Extension> exts, VkExtent2D extent, Device&);
    static bool CheckGPU(vvk::PhysicalDevice gpu, std::span<const Extension> exts,
                         VkSurfaceKHR surface);

    void Destroy();

    const auto&   graphics_queue() const { return m_graphics_queue; }
    const auto&   present_queue() const { return m_present_queue; }
    const auto&   device() const { return m_device; }
    const auto&   handle() const { return m_device; }
    const auto&   gpu() const { return m_gpu; }
    const auto&   limits() const { return m_limits; }
    const auto&   vma_allocator() const { return *m_allocator; }
    const auto&   cmd_pool() const { return m_command_pool; }
    const auto&   swapchain() const { return m_swapchain; }
    const auto&   out_extent() const { return m_extent; }
    void          set_out_extent(VkExtent2D v) { m_extent = v; }

    bool supportExt(std::string_view) const;

    TextureCache& tex_cache() const { return *m_tex_cache; }

    VkDeviceSize GetUsage() const;

private:
    std::vector<VkDeviceQueueCreateInfo> ChooseDeviceQueue(VkSurfaceKHR = {});

    vvk::DeviceDispatch     dld;
    vvk::Device             m_device;
    vvk::PhysicalDevice     m_gpu;
    vvk::VmaAllocatorHandle m_allocator;

    VkPhysicalDeviceLimits m_limits;
    Set<std::string>       m_extensions;

    Swapchain m_swapchain;

    vvk::CommandPool m_command_pool;

    QueueParameters m_graphics_queue;
    QueueParameters m_present_queue;

    VkExtent2D m_extent { 1, 1 };

    std::unique_ptr<TextureCache> m_tex_cache;
};

// ---------- Util.hpp ----------

inline bool CreateStagingBuffer(VmaAllocator allocator, std::size_t size,
                                VmaBufferParameters& buffer) {
    VkBufferCreateInfo ci {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .size  = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    buffer.req_size = ci.size;

    VmaAllocationCreateInfo vma_info = {};
    vma_info.usage                   = VMA_MEMORY_USAGE_CPU_ONLY;
    VVK_CHECK_BOOL_RE(vvk::CreateBuffer(allocator, ci, vma_info, buffer.handle));
    return true;
}

// ---------- StagingBuffer.hpp ----------

class StagingBuffer;

class StagingBufferRef {
public:
    VkDeviceSize size { 0 };
    VkDeviceSize offset { 0 };

    operator bool() const { return m_allocation != VK_NULL_HANDLE; }

private:
    friend class StagingBuffer;
    VmaVirtualAllocation m_allocation {};
    size_t               m_virtual_index { 0 };
};

class StagingBuffer : NoCopy, NoMove {
public:
    StagingBuffer(const Device&, VkDeviceSize size, VkBufferUsageFlags);
    ~StagingBuffer();

    bool allocate();
    void destroy();

    bool allocateSubRef(VkDeviceSize size, StagingBufferRef&, VkDeviceSize alignment = 1);
    void unallocateSubRef(const StagingBufferRef&);
    bool writeToBuf(const StagingBufferRef&, std::span<uint8_t>, size_t offset = 0);
    bool fillBuf(const StagingBufferRef& ref, size_t offset, size_t size, uint8_t c);

    bool recordUpload(vvk::CommandBuffer&);

    VkBuffer gpuBuf() const;

private:
    struct VirtualBlock {
        VmaVirtualBlock handle {};
        bool            enabled { false };
        size_t          index { 0 };
        VkDeviceSize    offset { 0 };
        VkDeviceSize    size { 0 };
    };

    VkResult      mapStageBuf();
    VirtualBlock* newVirtualBlock(VkDeviceSize);
    bool          increaseBuf(VkDeviceSize);

    const Device& m_device;
    VkDeviceSize  m_size_step;

    VkBufferUsageFlags m_usage;

    void*                     m_stage_raw { nullptr };
    std::vector<VirtualBlock> m_virtual_blocks {};

    VmaBufferParameters m_stage_buf;
    VmaBufferParameters m_gpu_buf;
};

// ---------- GraphicsPipeline.hpp ----------

struct PipelineParameters {
    vvk::Pipeline       handle;
    vvk::PipelineLayout layout;
    vvk::RenderPass     pass;

    std::vector<vvk::DescriptorSetLayout> descriptor_layouts;
};

struct DescriptorSetInfo {
    bool push_descriptor { false };

    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

class GraphicsPipeline : NoCopy, NoMove {
public:
    GraphicsPipeline();
    ~GraphicsPipeline();

    void toDefault();
    bool create(const Device&, vvk::RenderPass&, PipelineParameters&);

    VkPipelineMultisampleStateCreateInfo   multisample {};
    VkPipelineRasterizationStateCreateInfo raster {};
    VkPipelineDepthStencilStateCreateInfo  depth {};

    ShaderSpv*  getShaderSpv(VkShaderStageFlagBits) const;
    const auto& pass() const { return m_pass; }

    GraphicsPipeline& setColorBlendStates(std::span<const VkPipelineColorBlendAttachmentState>);
    GraphicsPipeline& setLogicOp(bool enable, VkLogicOp);

    GraphicsPipeline& setRenderPass(vvk::RenderPass);
    GraphicsPipeline& addDescriptorSetInfo(std::span<const DescriptorSetInfo>);
    GraphicsPipeline& addStage(Uni_ShaderSpv&&);
    GraphicsPipeline&
    addInputAttributeDescription(std::span<const VkVertexInputAttributeDescription>);
    GraphicsPipeline& addInputBindingDescription(std::span<const VkVertexInputBindingDescription>);
    GraphicsPipeline& setTopology(VkPrimitiveTopology);

private:
    vvk::RenderPass m_pass;

    VkPipelineInputAssemblyStateCreateInfo         m_input_assembly {};
    std::vector<VkVertexInputBindingDescription>   m_input_bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> m_input_attr_descriptions;

    VkPipelineViewportStateCreateInfo                m_view;
    VkPipelineColorBlendStateCreateInfo              m_color;
    std::vector<VkDynamicState>                      m_dynamic_states;
    std::vector<VkPipelineColorBlendAttachmentState> m_color_attachments;
    std::vector<DescriptorSetInfo>                   m_descriptor_set_infos;
    Map<VkShaderStageFlagBits, Uni_ShaderSpv>        m_stage_spv_map;
};

// ShaderReflected / GenReflect / VulkanTarget / ShaderCompUnit / ShaderCompOpt /
// CompileAndLinkShaderUnits / Preprocess all live in wescene.shader_compile
// (re-exported above).

// ---------- VertexInputState.hpp ----------

struct VertexInputState {
    VkPipelineInputAssemblyStateCreateInfo         input_assembly;
    VkPipelineVertexInputStateCreateInfo           input;
    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
};

// ---------- LocalExSwapchain.hpp ----------

struct LocalExHandle : NoCopy {
    ExHandle          handle;
    ExImageParameters image;

    LocalExHandle()  = default;
    ~LocalExHandle() = default;
    LocalExHandle(LocalExHandle&& o) noexcept: handle(o.handle), image(std::move(o.image)) {}
    LocalExHandle& operator=(LocalExHandle&& o) noexcept {
        handle = o.handle;
        image  = std::move(o.image);
        return *this;
    }
};

// NB: this used to be `private TripleSwapchain<ExHandle>` in classic, but
// module-attached classes can't downcast `this` to a privately-inherited
// global-attached base — clang refuses the implicit `this` conversion at
// the qualified-call site. Public inheritance is semantically equivalent
// here (callers never reach for the base interface directly).
class LocalExSwapchain final : public ::wallpaper::ExSwapchain,
                               public ::wallpaper::TripleSwapchain<::wallpaper::ExHandle> {
public:
    LocalExSwapchain(std::array<LocalExHandle, 3> handles, VkExtent2D ext)
        : m_handles(std::move(handles)), m_extent(ext) {
        int index = 0;
        for (auto& h : m_handles) {
            auto& handle         = h.handle;
            handle               = ::wallpaper::ExHandle(index++);
            handle.width         = (i32)h.image.extent.width;
            handle.height        = (i32)h.image.extent.height;
            handle.fd            = h.image.fd;
            handle.size          = h.image.mem_reqs.size;
            handle.drm_fourcc    = h.image.drm_fourcc;
            handle.drm_modifier  = h.image.drm_modifier;
            handle.plane0_offset = h.image.plane0_offset;
            handle.plane0_stride = h.image.plane0_stride;
        }
        m_presented  = &m_handles[0].handle;
        m_ready      = &m_handles[1].handle;
        m_inprogress = &m_handles[2].handle;
    }

    ~LocalExSwapchain() override {
        int fd = m_last_sync_fd.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) ::close(fd);
    }

    bool acquireRenderTarget(ImageParameters& out) override {
        out = ToImageParameters(m_handles.at((std::size_t)(*this->inprogress()).id()).image);
        return true;
    }

    void submitRendered(int acquire_sync_fd) override {
        if (acquire_sync_fd >= 0) {
            int old = m_last_sync_fd.exchange(acquire_sync_fd, std::memory_order_acq_rel);
            if (old >= 0) ::close(old);
        }
        this->renderFrame();
    }

    int takeLastFrameSyncFd() override {
        return m_last_sync_fd.exchange(-1, std::memory_order_acq_rel);
    }

    ::wallpaper::ExHandle* eatFrame() override {
        return this->TripleSwapchain<::wallpaper::ExHandle>::eatFrame();
    }
    std::array<::wallpaper::ExHandle*, 3> snapshot_all_slots() override {
        return this->TripleSwapchain<::wallpaper::ExHandle>::snapshot_all_slots();
    }

    unsigned width() const override { return m_extent.width; }
    unsigned height() const override { return m_extent.height; }
    VkFormat format() const override { return VK_FORMAT_R8G8B8A8_UNORM; }

    VkImageLayout producerOutputLayout() const override { return VK_IMAGE_LAYOUT_GENERAL; }
    uint32_t      releaseTargetQueueFamily() const override { return VK_QUEUE_FAMILY_IGNORED; }
    bool          ready() const override { return true; }

    void setOnReadyChanged(
        std::function<void(const ::wallpaper::ExSwapchainReadyEvent&)> cb) override {
        if (cb) {
            ::wallpaper::ExSwapchainReadyEvent e {
                .ready  = true,
                .width  = m_extent.width,
                .height = m_extent.height,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
            };
            cb(e);
        }
    }

protected:
    std::atomic<::wallpaper::ExHandle*>& presented() override { return m_presented; }
    std::atomic<::wallpaper::ExHandle*>& ready() override { return m_ready; }
    std::atomic<::wallpaper::ExHandle*>& inprogress() override { return m_inprogress; }

private:
    std::array<LocalExHandle, 3>        m_handles;
    std::atomic<::wallpaper::ExHandle*> m_presented { nullptr };
    std::atomic<::wallpaper::ExHandle*> m_ready { nullptr };
    std::atomic<::wallpaper::ExHandle*> m_inprogress { nullptr };
    VkExtent2D                          m_extent;
    std::atomic<int>                    m_last_sync_fd { -1 };
};

inline std::unique_ptr<LocalExSwapchain> CreateLocalExSwapchain(const Device& device, unsigned w,
                                                                unsigned      h,
                                                                VkImageTiling tiling) {
    std::array<LocalExHandle, 3> handles;
    for (auto& handle : handles) {
        if (auto rv = device.tex_cache().CreateExTex(w, h, VK_FORMAT_R8G8B8A8_UNORM, tiling);
            rv.has_value())
            handle.image = std::move(rv.value());
        else
            return nullptr;
    }
    return std::make_unique<LocalExSwapchain>(std::move(handles), VkExtent2D { w, h });
}

} // namespace vulkan
} // namespace wallpaper (export)
