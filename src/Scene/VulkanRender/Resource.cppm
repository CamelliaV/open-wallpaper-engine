module;


#include "Swapchain/ExSwapchain.hpp"

export module wescene.vulkan_render:resource;
import wescene.core;
import cppstd;
import wescene.vulkan;

export namespace owe::vulkan
{

struct RenderingResources {
    vvk::CommandBuffer command;

    vvk::Semaphore sem_swap_wait_image;
    vvk::Semaphore sem_export;
    vvk::Fence     fence_frame;

    StagingBuffer* vertex_buf;
    StagingBuffer* dyn_buf;
};

} // namespace owe::vulkan
