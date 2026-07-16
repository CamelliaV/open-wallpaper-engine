module;

#include <unistd.h>
#include <cerrno>

// Macros only — VVK_CHECK family.
#include "vvk/macros.hpp"

export module wescene.vulkan;
import wescene.core;
import rstd.log;
import rstd.cppstd;
import wescene.types;

// Vulkan FFI: wavsen::ffi::vulkan exposes the full Vk symbol surface as
// a comprehensive FFI module. Re-exported so downstream consumers
// (wescene.vulkan_render etc.) that `import wescene.vulkan;` still see
// every Vk type / enumerator / PFN_* without needing their own
// `import vulkan;`.
export import vulkan;
export import wavsen.vvk;

// Re-export the host-only shader compile API. Lets existing consumers
// (VulkanRender/* etc.) keep their `import wescene.vulkan;` without
// caring that ShaderSpv / ShaderReflected / Preprocess / etc. now live
// in a separate module.
export import wescene.shader_compile;

export namespace owe
{

// ---------- ExSwapchain (formerly Swapchain/ExSwapchain.hpp) ----------

namespace vulkan
{

struct ImageParameters {
    VkImage     handle {};
    VkImageView view {};
    VkSampler   sampler {};
    VkExtent3D  extent {};
    uint32_t    mipmap_level { 1 };
    uint64_t    generation { 0 };

    ImageParameters()  = default;
    ~ImageParameters() = default;
};

} // namespace vulkan

// Snapshot of the negotiated buffer set the producer should render into.
// Fired by ExSwapchain implementations whenever readiness flips or the
// negotiated extent/format changes.
struct ExSwapchainReadyEvent {
    bool     ready;
    unsigned width;
    unsigned height;
    VkFormat format;
};

enum class FrameSurfaceAcquireKind
{
    QueueOrdered,
    BinarySemaphore,
    ExternalProtocol,
};

struct FrameSurfaceIdentity {
    uint64_t owner_generation { 0 };
    uint32_t image_index { 0 };
    uint64_t acquire_serial { 0 };

    bool valid() const noexcept { return owner_generation != 0 && acquire_serial != 0; }
    bool operator==(const FrameSurfaceIdentity&) const = default;
};

enum class FrameSurfaceReuseKind
{
    QueueOrdered,
    PresentationAcquired,
    NeverSubmitted,
    ConsumerReleased,
};

struct FrameSurfaceReuseProof {
    FrameSurfaceReuseKind kind { FrameSurfaceReuseKind::QueueOrdered };
    uint64_t              release_point { 0 };

    bool valid() const noexcept {
        return (kind == FrameSurfaceReuseKind::ConsumerReleased) == (release_point != 0);
    }
};

struct FrameSurfaceAcquireDependency {
    FrameSurfaceAcquireKind kind { FrameSurfaceAcquireKind::QueueOrdered };
    VkSemaphore             semaphore { VK_NULL_HANDLE };

    bool valid() const noexcept {
        return (kind == FrameSurfaceAcquireKind::BinarySemaphore) == (semaphore != VK_NULL_HANDLE);
    }
};

struct FrameSurfaceLease {
    FrameSurfaceIdentity          identity;
    FrameSurfaceReuseProof        reuse;
    vulkan::ImageParameters       image;
    VkFormat                      format { VK_FORMAT_UNDEFINED };
    VkImageLayout                 initial_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    uint32_t                      initial_queue_family { VK_QUEUE_FAMILY_IGNORED };
    FrameSurfaceAcquireDependency acquire;
    VkImageLayout                 final_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    uint32_t                      final_queue_family { VK_QUEUE_FAMILY_IGNORED };
    bool                          discard_content { true };

    bool valid() const noexcept {
        const bool acquire_matches_reuse =
            (acquire.kind == FrameSurfaceAcquireKind::QueueOrdered &&
             reuse.kind == FrameSurfaceReuseKind::QueueOrdered) ||
            (acquire.kind == FrameSurfaceAcquireKind::BinarySemaphore &&
             reuse.kind == FrameSurfaceReuseKind::PresentationAcquired) ||
            (acquire.kind == FrameSurfaceAcquireKind::ExternalProtocol &&
             (reuse.kind == FrameSurfaceReuseKind::NeverSubmitted ||
              reuse.kind == FrameSurfaceReuseKind::ConsumerReleased));
        return identity.valid() && reuse.valid() && acquire_matches_reuse &&
               image.handle != VK_NULL_HANDLE && image.extent.width != 0 &&
               image.extent.height != 0 && image.extent.depth != 0 &&
               format != VK_FORMAT_UNDEFINED && acquire.valid() && discard_content &&
               final_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
               initial_queue_family != VK_QUEUE_FAMILY_IGNORED &&
               final_queue_family != VK_QUEUE_FAMILY_IGNORED;
    }
};

enum class FrameSurfaceCompletionStatus
{
    Submitted,
    Aborted,
    NotPending,
    StaleIdentity,
    SessionLost,
    ProtocolError,
};

struct FrameSurfaceCompletionResult {
    FrameSurfaceCompletionStatus status { FrameSurfaceCompletionStatus::NotPending };
    FrameSurfaceIdentity         identity;
    int32_t                      error_code { 0 };

    bool completed() const noexcept {
        return status == FrameSurfaceCompletionStatus::Submitted ||
               status == FrameSurfaceCompletionStatus::Aborted;
    }
};

class ExSwapchain;

class FrameSurfaceCompletionCapability {
public:
    FrameSurfaceCompletionCapability() = default;
    ~FrameSurfaceCompletionCapability();

    FrameSurfaceCompletionCapability(const FrameSurfaceCompletionCapability&)            = delete;
    FrameSurfaceCompletionCapability& operator=(const FrameSurfaceCompletionCapability&) = delete;
    FrameSurfaceCompletionCapability(FrameSurfaceCompletionCapability&&) noexcept;
    FrameSurfaceCompletionCapability& operator=(FrameSurfaceCompletionCapability&&) noexcept;

    bool valid() const noexcept { return m_owner != nullptr && m_identity.valid(); }

    FrameSurfaceCompletionResult Submit(int producer_sync_fd);
    FrameSurfaceCompletionResult Abort();

private:
    friend class ExSwapchain;

    FrameSurfaceCompletionCapability(std::shared_ptr<ExSwapchain> owner,
                                     FrameSurfaceIdentity         identity)
        : m_owner(std::move(owner)), m_identity(identity) {}

    std::shared_ptr<ExSwapchain> m_owner;
    FrameSurfaceIdentity         m_identity;
};

enum class FrameSurfaceAcquireStatus
{
    Acquired,
    Suboptimal,
    Busy,
    NotReady,
    ForcedRelease,
    SessionLost,
    ProtocolError,
};

struct FrameSurfaceAcquireResult {
    FrameSurfaceAcquireStatus        status { FrameSurfaceAcquireStatus::NotReady };
    FrameSurfaceLease                lease;
    FrameSurfaceCompletionCapability completion;
    int32_t                          error_code { 0 };

    bool acquired() const noexcept {
        return (status == FrameSurfaceAcquireStatus::Acquired ||
                status == FrameSurfaceAcquireStatus::Suboptimal) &&
               lease.valid() && completion.valid();
    }
};

enum class TexTiling
{
    OPTIMAL,
    LINEAR
};

// Per-slot DMA-BUF descriptor surfaced to the IPC layer. Local backend
// fills these from its self-allocated `ExImageParameters`; bridge backend
// keeps them empty (the bridge already published the metadata via
// `bind_buffers` itself).
struct ExHandle {
    int         fd { -1 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    std::size_t size { 0 };

    uint32_t drm_fourcc { 0 };
    uint64_t drm_modifier { 0 };
    uint64_t plane0_offset { 0 };
    uint32_t plane0_stride { 0 };

    ExHandle() = default;
    ExHandle(int id): m_id(id) {};

    int32_t id() const { return m_id; }

private:
    int32_t m_id { 0 };
};

template<typename T>
class TripleSwapchain {
public:
    virtual ~TripleSwapchain() = default;

    TripleSwapchain(const TripleSwapchain&)            = delete;
    TripleSwapchain& operator=(const TripleSwapchain&) = delete;
    TripleSwapchain(TripleSwapchain&&)                 = delete;
    TripleSwapchain& operator=(TripleSwapchain&&)      = delete;

    T* eatFrame() {
        if (! dirty().exchange(false)) return nullptr;
        presented() = ready().exchange(presented());
        return presented();
    }
    void renderFrame() {
        inprogress() = ready().exchange(inprogress());
        dirty().exchange(true);
    }
    T* getInprogress() { return inprogress(); }

    std::array<T*, 3> snapshot_all_slots() {
        return { presented().load(), ready().load(), inprogress().load() };
    }

    virtual unsigned width() const  = 0;
    virtual unsigned height() const = 0;

protected:
    TripleSwapchain() = default;

    virtual std::atomic<T*>& presented()  = 0;
    virtual std::atomic<T*>& ready()      = 0;
    virtual std::atomic<T*>& inprogress() = 0;

private:
    std::atomic<bool>& dirty() { return m_dirty; };
    std::atomic<bool>  m_dirty { false };
};

// Producer-side abstraction over the offscreen swapchain. Two
// implementations exist:
//   - LocalExSwapchain: self-allocates 3 DMA-BUF-backed VkImages via the
//     Vulkan TextureCache; the standalone viewers and any in-process host
//     drive bind_buffers / frame_ready themselves and consume `eatFrame()`
//     / `snapshot_all_slots()`.
//   - BridgeExSwapchain: wraps a `ww_pool_t`; bridge owns the slot images
//     and completes submission through the acquired frame capability.
class ExSwapchain : public std::enable_shared_from_this<ExSwapchain> {
public:
    virtual ~ExSwapchain() = default;

    ExSwapchain(const ExSwapchain&)            = delete;
    ExSwapchain& operator=(const ExSwapchain&) = delete;
    ExSwapchain(ExSwapchain&&)                 = delete;
    ExSwapchain& operator=(ExSwapchain&&)      = delete;

    virtual void poll() {}

    virtual FrameSurfaceAcquireResult acquireRenderTarget() = 0;

    virtual int takeLastFrameSyncFd() { return -1; }

    virtual ExHandle*                eatFrame() { return nullptr; }
    virtual std::array<ExHandle*, 3> snapshot_all_slots() { return { nullptr, nullptr, nullptr }; }

    virtual unsigned width() const  = 0;
    virtual unsigned height() const = 0;
    virtual VkFormat format() const = 0;

    virtual bool ready() const = 0;

    virtual void setOnReadyChanged(std::function<void(const ExSwapchainReadyEvent&)>) = 0;

protected:
    ExSwapchain() = default;

    FrameSurfaceCompletionCapability MakeCompletionCapability(FrameSurfaceIdentity identity) {
        return FrameSurfaceCompletionCapability(shared_from_this(), identity);
    }

private:
    friend class FrameSurfaceCompletionCapability;

    virtual FrameSurfaceCompletionResult CompleteRendered(FrameSurfaceIdentity identity,
                                                          int producer_sync_fd)           = 0;
    virtual FrameSurfaceCompletionResult AbortRenderTarget(FrameSurfaceIdentity identity) = 0;
};

inline FrameSurfaceCompletionCapability::~FrameSurfaceCompletionCapability() {
    if (valid()) (void)Abort();
}

inline FrameSurfaceCompletionCapability::FrameSurfaceCompletionCapability(
    FrameSurfaceCompletionCapability&& other) noexcept
    : m_owner(std::move(other.m_owner)), m_identity(other.m_identity) {
    other.m_identity = {};
}

inline FrameSurfaceCompletionCapability&
FrameSurfaceCompletionCapability::operator=(FrameSurfaceCompletionCapability&& other) noexcept {
    if (this == &other) return *this;
    if (valid()) (void)Abort();
    m_owner          = std::move(other.m_owner);
    m_identity       = other.m_identity;
    other.m_identity = {};
    return *this;
}

inline FrameSurfaceCompletionResult FrameSurfaceCompletionCapability::Submit(int producer_sync_fd) {
    if (! valid()) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return {};
    }
    auto owner    = std::move(m_owner);
    auto identity = m_identity;
    m_identity    = {};
    return owner->CompleteRendered(identity, producer_sync_fd);
}

inline FrameSurfaceCompletionResult FrameSurfaceCompletionCapability::Abort() {
    if (! valid()) return {};
    auto owner    = std::move(m_owner);
    auto identity = m_identity;
    m_identity    = {};
    return owner->AbortRenderTarget(identity);
}

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

    static bool Create(Instance&, std::span<const Extension>, std::span<const InstanceLayer>,
                       std::uint32_t api_version = WP_VULKAN_VERSION);
    bool ChoosePhysicalDevice(const CheckGpuOp& checkgpu, std::span<const std::uint8_t> uuid = {});

    const vvk::Instance&         inst() const;
    const vvk::PhysicalDevice&   gpu() const;
    const vvk::SurfaceKHR&       surface() const;
    std::uint32_t                api_version() const { return m_api_version; }
    std::span<const std::string> enabled_extensions() const { return m_enabled_extensions; }

    bool offscreen() const;
    void setSurface(VkSurfaceKHR);
    bool supportExt(std::string_view) const;
    bool supportLayer(std::string_view) const;

private:
    vvk::InstanceDispatch m_dld;
    vvk::Instance         m_vinst;

    vvk::DebugUtilsMessenger m_debug_utils;
    vvk::PhysicalDevice      m_gpu {};
    std::uint32_t            m_api_version { WP_VULKAN_VERSION };

    vvk::SurfaceKHR          m_surface {};
    Set<std::string>         m_extensions;
    std::vector<std::string> m_enabled_extensions;
    Set<std::string>         m_layers;
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
    unsigned       mipmap_level { 1 };
    uint64_t       generation { 0 };

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
    unsigned       mipmap_level { 1 };
    uint64_t       generation { 0 };
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
    out.generation   = o.generation;
    return out;
}
inline ImageParameters ToImageParameters(const ExImageParameters& o) noexcept {
    ImageParameters out;
    out.handle       = *o.handle;
    out.view         = *o.view;
    out.sampler      = *o.sampler;
    out.extent       = o.extent;
    out.mipmap_level = o.mipmap_level;
    out.generation   = o.generation;
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
    i32                   width;
    i32                   height;
    TexUsage              usage;
    TextureFormat         format;
    TextureSample         sample;
    unsigned              mipmap_level { 1 };
    VkSampleCountFlagBits samples { VK_SAMPLE_COUNT_1_BIT };

    static TexHash HashValue(const TextureKey&);
};

class TextureCache : NoCopy, NoMove {
public:
    struct VideoDecodeOptions {
        std::string hwdec { "auto" };
        std::string render_node;
    };

    TextureCache(const Device&);
    ~TextureCache();

    void Clear();
    void ClearTransientGraphResources();

    void SetVideoDecodeOptions(VideoDecodeOptions);

    std::optional<ExImageParameters> CreateExTex(uint32_t witdh, uint32_t height, VkFormat,
                                                 VkImageTiling);
    ImageSlotsRef                    CreateTex(Image&);
    std::optional<ImageSlotsRef>     FindImportedTexture(std::string_view key) const;

    std::optional<ImageParameters> Query(std::string_view key, TextureKey content_hash,
                                         bool persist = false);

    void MarkShareReady(std::string_view key);

    void RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const;

    /* Per-frame hook: advance every registered video-tex by `dt_seconds`,
     * pull as many decoded frames as needed to catch up to wall PTS,
     * convert NV12→RGBA on the CPU, and upload to the slot's stable
     * VkImage. No-op if no video textures are registered. */
    void PumpVideoTextures(double dt_seconds);

    /* vkCmdCopyBufferToImage a sub-rect of `atlas` into the VkImage stored
     * under `key`. Returns false if `key` has no entry yet (the VkImage
     * hasn't been allocated — CreateTex hasn't run); caller may retry
     * next frame. */
    bool UploadFontAtlasRegion(const std::string& key, const std::uint8_t* atlas,
                               std::uint32_t atlas_w, std::uint32_t x, std::uint32_t y,
                               std::uint32_t w, std::uint32_t h);

private:
    std::optional<VmaImageParameters> CreateTex(TextureKey);
    uint64_t                          nextImageGeneration();
    void                              AssignImageGeneration(VmaImageParameters&);
    void                              AssignImageGeneration(ExImageParameters&);
    /* VIDEO-typed Image branch of CreateTex: registers a wavsen
     * VideoDecoder + stable RGBA8 VkImage and returns an ImageSlotsRef
     * pointing at that same VkImage so material binding is transparent. */
    ImageSlotsRef       CreateVideoTex(Image&);
    void                allocateCmd();
    vvk::CommandBuffers m_tex_cmds;
    vvk::CommandBuffer  m_tex_cmd;

    const Device&                m_device;
    Map<std::string, ImageSlots> m_tex_map;
    VideoDecodeOptions           m_video_decode_options;
    uint64_t                     m_next_image_generation { 1 };

    /* Opaque pImpl for the active video-tex set. Defined inside
     * TextureCache.cpp to keep wavsen.video out of the public
     * wescene.vulkan module interface. */
    struct VideoRegistry;
    std::unique_ptr<VideoRegistry> m_video_registry;

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

class MeshCache;

class Device : NoCopy, NoMove {
public:
    Device();
    ~Device();

    static bool Create(Instance&, std::span<const Extension> exts, VkExtent2D extent, Device&);
    static bool CheckGPU(vvk::PhysicalDevice gpu, std::span<const Extension> exts,
                         VkSurfaceKHR surface);

    void Destroy();

    const auto&                  graphics_queue() const { return m_graphics_queue; }
    const auto&                  present_queue() const { return m_present_queue; }
    const auto&                  device() const { return m_device; }
    const auto&                  handle() const { return m_device; }
    const auto&                  gpu() const { return m_gpu; }
    VkInstance                   instance_handle() const { return m_instance; }
    std::uint32_t                instance_api_version() const { return m_instance_api_version; }
    std::span<const std::string> enabled_instance_extensions() const {
        return m_enabled_instance_extensions;
    }
    std::span<const std::string> enabled_device_extensions() const {
        return m_enabled_device_extensions;
    }
    const auto& limits() const { return m_limits; }
    const auto& vma_allocator() const { return *m_allocator; }
    const auto& cmd_pool() const { return m_command_pool; }
    const auto& swapchain() const { return m_swapchain; }
    const auto& out_extent() const { return m_extent; }
    void        set_out_extent(VkExtent2D v) { m_extent = v; }

    bool supportExt(std::string_view) const;

    TextureCache& tex_cache() const { return *m_tex_cache; }
    MeshCache&    mesh_cache() const { return *m_mesh_cache; }

    VkDeviceSize GetUsage() const;

private:
    std::vector<VkDeviceQueueCreateInfo> ChooseDeviceQueue(VkSurfaceKHR = {});

    vvk::DeviceDispatch     dld;
    VkInstance              m_instance { VK_NULL_HANDLE };
    std::uint32_t           m_instance_api_version { WP_VULKAN_VERSION };
    vvk::Device             m_device;
    vvk::PhysicalDevice     m_gpu;
    vvk::VmaAllocatorHandle m_allocator;

    VkPhysicalDeviceLimits   m_limits;
    Set<std::string>         m_extensions;
    std::vector<std::string> m_enabled_instance_extensions;
    std::vector<std::string> m_enabled_device_extensions;

    Swapchain m_swapchain;

    vvk::CommandPool m_command_pool;

    QueueParameters m_graphics_queue;
    QueueParameters m_present_queue;

    VkExtent2D m_extent { 1, 1 };

    std::unique_ptr<TextureCache> m_tex_cache;
    std::unique_ptr<MeshCache>    m_mesh_cache;
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

// ---------- MeshCache.hpp ----------

// Key identifying one mesh data block (vertex array or index array). We key
// by pointer because SceneVertexArray/SceneIndexArray addresses are stable
// across the owning SceneMesh's lifetime, and SceneMesh::ChangeMeshDataFrom
// shares the underlying arrays via shared_ptr — so identical content shows
// up as the same pointer automatically.
struct MeshCacheKey {
    const void* array_ptr { nullptr };
    uint64_t    generation { 0 };

    bool operator==(const MeshCacheKey& o) const noexcept {
        return array_ptr == o.array_ptr && generation == o.generation;
    }
};

struct MeshCacheKeyHash {
    size_t operator()(const MeshCacheKey& k) const noexcept {
        return std::hash<const void*>()(k.array_ptr) ^
               (std::hash<uint64_t>()(k.generation) * 0x9E3779B97F4A7C15ULL);
    }
};

class MeshCache;

// RAII handle into MeshCache. Does NOT cache the VkBuffer — StagingBuffer's
// increaseBuf may invalidate it. Callers resolve buffer() at execute time.
class MeshBufferRef {
public:
    MeshBufferRef() = default;
    MeshBufferRef(MeshCache* owner, MeshCacheKey key, VkDeviceSize offset, VkDeviceSize size);
    ~MeshBufferRef();

    MeshBufferRef(const MeshBufferRef&)            = delete;
    MeshBufferRef& operator=(const MeshBufferRef&) = delete;

    MeshBufferRef(MeshBufferRef&& o) noexcept;
    MeshBufferRef& operator=(MeshBufferRef&& o) noexcept;

    explicit operator bool() const noexcept { return m_owner != nullptr && m_size > 0; }

    VkBuffer     buffer() const noexcept;
    VkDeviceSize offset() const noexcept { return m_offset; }
    VkDeviceSize size() const noexcept { return m_size; }

private:
    MeshCache*   m_owner { nullptr };
    MeshCacheKey m_key {};
    VkDeviceSize m_offset { 0 };
    VkDeviceSize m_size { 0 };
};

class MeshCache : NoCopy, NoMove {
public:
    explicit MeshCache(const Device&);
    ~MeshCache();

    bool init();
    void destroy();

    // Hit: refcount++, returns ref to existing sub-allocation.
    // Miss: allocateSubRef + writeToBuf, marks dirty, returns fresh ref.
    std::optional<MeshBufferRef> QueryOrUpload(MeshCacheKey key, std::span<const uint8_t> data,
                                               VkDeviceSize alignment = 4);

    // Called by ~MeshBufferRef; refcount--, no immediate free.
    void release(MeshCacheKey key);

    // Current GPU buffer handle. May change across increaseBuf in QueryOrUpload.
    VkBuffer gpuBuf() const;

    // Flushes any pending writes to GPU. No-op if nothing dirty since last flush.
    bool recordPendingUploads(vvk::CommandBuffer& cmd);

    // No-op for now; reserved as the hook downstream wires to clearLastRenderGraph.
    void onRenderGraphCleared();

    // Releases refcount==0 entries from the underlying StagingBuffer.
    void evictUnused();

private:
    struct Entry {
        StagingBufferRef ref;
        uint32_t         refcount { 0 };
    };

    const Device&                                             m_device;
    std::unique_ptr<StagingBuffer>                            m_buf;
    std::unordered_map<MeshCacheKey, Entry, MeshCacheKeyHash> m_map;
    bool                                                      m_dirty { false };
};

// ---------- GraphicsPipeline.hpp ----------

struct PipelineParameters {
    vvk::Pipeline                    handle;
    vvk::PipelineLayout              layout;
    std::shared_ptr<vvk::RenderPass> pass;

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
    bool create(const Device&, VkRenderPass, PipelineParameters&);

    VkPipelineMultisampleStateCreateInfo   multisample {};
    VkPipelineRasterizationStateCreateInfo raster {};
    VkPipelineDepthStencilStateCreateInfo  depth {};

    ShaderSpv*  getShaderSpv(VkShaderStageFlagBits) const;
    const auto& pass() const { return m_pass; }

    GraphicsPipeline& setColorBlendStates(std::span<const VkPipelineColorBlendAttachmentState>);
    GraphicsPipeline& setColorBlendOptions(VkPipelineColorBlendStateCreateFlags,
                                           const std::array<float, 4>&);
    GraphicsPipeline& setLogicOp(bool enable, VkLogicOp);

    GraphicsPipeline& setRenderPass(vvk::RenderPass);
    GraphicsPipeline& addDescriptorSetInfo(std::span<const DescriptorSetInfo>);
    GraphicsPipeline& addStage(Uni_ShaderSpv&&);
    GraphicsPipeline&
        addInputAttributeDescription(std::span<const VkVertexInputAttributeDescription>);
    GraphicsPipeline& addInputBindingDescription(std::span<const VkVertexInputBindingDescription>);
    GraphicsPipeline& setCreateInfoOptions(VkPipelineCreateFlags flags, uint32_t subpass);
    GraphicsPipeline& setTopology(VkPrimitiveTopology);
    GraphicsPipeline& setPrimitiveRestartEnable(bool);
    GraphicsPipeline& setViewportScissorCount(uint32_t viewport_count, uint32_t scissor_count);
    GraphicsPipeline& setDynamicStates(std::span<const VkDynamicState>);
    GraphicsPipeline& setSampleCount(VkSampleCountFlagBits);

private:
    vvk::RenderPass m_pass;

    VkPipelineCreateFlags m_create_flags { 0 };
    uint32_t              m_subpass { 0 };

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
class LocalExSwapchain final : public ::owe::ExSwapchain,
                               public ::owe::TripleSwapchain<::owe::ExHandle> {
public:
    LocalExSwapchain(std::array<LocalExHandle, 3> handles, VkExtent2D ext, uint32_t queue_family)
        : m_handles(std::move(handles)), m_extent(ext), m_queue_family(queue_family) {
        int index = 0;
        for (auto& h : m_handles) {
            auto& handle         = h.handle;
            handle               = ::owe::ExHandle(index++);
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

    ::owe::FrameSurfaceAcquireResult acquireRenderTarget() override {
        if (m_surface_pending) {
            return { .status = ::owe::FrameSurfaceAcquireStatus::Busy };
        }
        const uint32_t slot_index     = static_cast<uint32_t>(this->getInprogress()->id());
        const uint64_t acquire_serial = m_next_acquire_serial++;
        if (acquire_serial == 0) {
            return { .status     = ::owe::FrameSurfaceAcquireStatus::ProtocolError,
                     .error_code = -EOVERFLOW };
        }
        ::owe::FrameSurfaceLease lease {
            .identity = { .owner_generation = 1,
                          .image_index      = slot_index,
                          .acquire_serial   = acquire_serial },
            .reuse    = { .kind = ::owe::FrameSurfaceReuseKind::QueueOrdered },
            .image    = ToImageParameters(m_handles.at(static_cast<std::size_t>(slot_index)).image),
            .format   = VK_FORMAT_R8G8B8A8_UNORM,
            .initial_layout       = VK_IMAGE_LAYOUT_GENERAL,
            .initial_queue_family = m_queue_family,
            .acquire              = { .kind = ::owe::FrameSurfaceAcquireKind::QueueOrdered },
            .final_layout         = VK_IMAGE_LAYOUT_GENERAL,
            .final_queue_family   = m_queue_family,
            .discard_content      = true,
        };
        if (! lease.valid()) {
            return { .status     = ::owe::FrameSurfaceAcquireStatus::ProtocolError,
                     .error_code = -EINVAL };
        }
        m_surface_pending  = true;
        m_pending_identity = lease.identity;
        auto completion    = MakeCompletionCapability(lease.identity);
        return { .status     = ::owe::FrameSurfaceAcquireStatus::Acquired,
                 .lease      = std::move(lease),
                 .completion = std::move(completion) };
    }

    int takeLastFrameSyncFd() override {
        return m_last_sync_fd.exchange(-1, std::memory_order_acq_rel);
    }

    ::owe::ExHandle* eatFrame() override {
        return this->TripleSwapchain<::owe::ExHandle>::eatFrame();
    }
    std::array<::owe::ExHandle*, 3> snapshot_all_slots() override {
        return this->TripleSwapchain<::owe::ExHandle>::snapshot_all_slots();
    }

    unsigned width() const override { return m_extent.width; }
    unsigned height() const override { return m_extent.height; }
    VkFormat format() const override { return VK_FORMAT_R8G8B8A8_UNORM; }

    bool ready() const override { return true; }

    void setOnReadyChanged(std::function<void(const ::owe::ExSwapchainReadyEvent&)> cb) override {
        if (cb) {
            ::owe::ExSwapchainReadyEvent e {
                .ready  = true,
                .width  = m_extent.width,
                .height = m_extent.height,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
            };
            cb(e);
        }
    }

protected:
    std::atomic<::owe::ExHandle*>& presented() override { return m_presented; }
    std::atomic<::owe::ExHandle*>& ready() override { return m_ready; }
    std::atomic<::owe::ExHandle*>& inprogress() override { return m_inprogress; }

private:
    ::owe::FrameSurfaceCompletionResult CompleteRendered(::owe::FrameSurfaceIdentity identity,
                                                         int acquire_sync_fd) override {
        if (! m_surface_pending) {
            if (acquire_sync_fd >= 0) ::close(acquire_sync_fd);
            return { .status   = ::owe::FrameSurfaceCompletionStatus::NotPending,
                     .identity = identity };
        }
        if (identity != m_pending_identity) {
            if (acquire_sync_fd >= 0) ::close(acquire_sync_fd);
            return { .status   = ::owe::FrameSurfaceCompletionStatus::StaleIdentity,
                     .identity = identity };
        }
        m_surface_pending  = false;
        m_pending_identity = {};
        if (acquire_sync_fd >= 0) {
            int old = m_last_sync_fd.exchange(acquire_sync_fd, std::memory_order_acq_rel);
            if (old >= 0) ::close(old);
        }
        this->renderFrame();
        return { .status = ::owe::FrameSurfaceCompletionStatus::Submitted, .identity = identity };
    }

    ::owe::FrameSurfaceCompletionResult
    AbortRenderTarget(::owe::FrameSurfaceIdentity identity) override {
        if (! m_surface_pending) {
            return { .status   = ::owe::FrameSurfaceCompletionStatus::NotPending,
                     .identity = identity };
        }
        if (identity != m_pending_identity) {
            return { .status   = ::owe::FrameSurfaceCompletionStatus::StaleIdentity,
                     .identity = identity };
        }
        m_surface_pending  = false;
        m_pending_identity = {};
        return { .status = ::owe::FrameSurfaceCompletionStatus::Aborted, .identity = identity };
    }

    std::array<LocalExHandle, 3>  m_handles;
    std::atomic<::owe::ExHandle*> m_presented { nullptr };
    std::atomic<::owe::ExHandle*> m_ready { nullptr };
    std::atomic<::owe::ExHandle*> m_inprogress { nullptr };
    VkExtent2D                    m_extent;
    uint32_t                      m_queue_family { VK_QUEUE_FAMILY_IGNORED };
    std::atomic<int>              m_last_sync_fd { -1 };
    uint64_t                      m_next_acquire_serial { 1 };
    ::owe::FrameSurfaceIdentity   m_pending_identity;
    bool                          m_surface_pending { false };
};

inline std::unique_ptr<LocalExSwapchain> CreateLocalExSwapchain(const Device& device, unsigned w,
                                                                unsigned h, VkImageTiling tiling) {
    std::array<LocalExHandle, 3> handles;
    for (auto& handle : handles) {
        if (auto rv = device.tex_cache().CreateExTex(w, h, VK_FORMAT_R8G8B8A8_UNORM, tiling);
            rv.has_value())
            handle.image = std::move(rv.value());
        else
            return nullptr;
    }
    return std::make_unique<LocalExSwapchain>(
        std::move(handles), VkExtent2D { w, h }, device.graphics_queue().family_index);
}

} // namespace vulkan
} // namespace owe
