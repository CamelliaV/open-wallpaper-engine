module;

#include <unistd.h>
#include <cerrno>

// Macros only — VVK_CHECK family.
#include "vvk/macros.hpp"

export module wescene.vulkan;
import wescene.core;
import rstd;
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

using namespace rstd::prelude;

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
    u32         mipmap_level { 1 };
    u64         generation { 0 };

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
    u64 owner_generation { 0 };
    u32 image_index { 0 };
    u64 acquire_serial { 0 };

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
    u64                   release_point { 0 };

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
    u32                           initial_queue_family { VK_QUEUE_FAMILY_IGNORED };
    FrameSurfaceAcquireDependency acquire;
    VkImageLayout                 final_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    u32                           final_queue_family { VK_QUEUE_FAMILY_IGNORED };
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
    i32                          error_code { 0 };

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
    i32                              error_code { 0 };

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
    int   fd { -1 };
    i32   width { 0 };
    i32   height { 0 };
    usize size { 0 };

    u32 drm_fourcc { 0 };
    u64 drm_modifier { 0 };
    u64 plane0_offset { 0 };
    u32 plane0_stride { 0 };

    ExHandle() = default;
    ExHandle(int id): m_id(id) {};

    i32 id() const { return m_id; }

private:
    i32 m_id { 0 };
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

    rstd::array<T*, 3> snapshot_all_slots() {
        return rstd::array<T*, 3> { presented().load(), ready().load(), inprogress().load() };
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

    virtual ExHandle*                 eatFrame() { return nullptr; }
    virtual rstd::array<ExHandle*, 3> snapshot_all_slots() {
        return rstd::array<ExHandle*, 3> { nullptr, nullptr, nullptr };
    }

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

constexpr u32         WP_VULKAN_VERSION { VK_API_VERSION_1_1 };
constexpr const char* WP_APPLICATION_NAME { "scene render" };

class Device;
class Instance {
public:
    Instance()  = default;
    ~Instance() = default;

    void Destroy();

    static bool Create(Instance&, std::span<const Extension>, std::span<const InstanceLayer>,
                       u32 api_version = WP_VULKAN_VERSION);
    bool        ChoosePhysicalDevice(const CheckGpuOp& checkgpu, std::span<const u8> uuid = {});

    const vvk::Instance&         inst() const;
    const vvk::PhysicalDevice&   gpu() const;
    const vvk::SurfaceKHR&       surface() const;
    u32                          api_version() const { return m_api_version; }
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
    u32                      m_api_version { WP_VULKAN_VERSION };

    vvk::SurfaceKHR          m_surface {};
    Set<std::string>         m_extensions;
    std::vector<std::string> m_enabled_extensions;
    Set<std::string>         m_layers;
};

// ShaderSpv / Uni_ShaderSpv now live in wescene.shader_compile (re-exported above).

// ---------- Parameters.hpp ----------

struct QueueParameters {
    vvk::Queue handle;
    u32        family_index;
};

struct VmaBufferParameters {
    vvk::VmaBuffer handle;
    usize          req_size;

    VmaBufferParameters();
    ~VmaBufferParameters();
    VmaBufferParameters(VmaBufferParameters&& o) noexcept;
    VmaBufferParameters& operator=(VmaBufferParameters&& o) noexcept;
};

struct BufferParameters {
    VkBuffer handle;
    usize    req_size;
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
    u64            generation { 0 };

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
    u64            generation { 0 };
    int            fd { 0 };

    u32 drm_fourcc { 0 };
    u64 drm_modifier { 0 };
    u64 plane0_offset { 0 };
    u32 plane0_stride { 0 };

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

class TextureAllocation {
public:
    explicit TextureAllocation(ImageSlots slots): m_slots(rstd::move(slots)) {}

    auto View() const -> ImageSlotsRef { return ImageSlotsRef(m_slots); }

private:
    ImageSlots m_slots;
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

using TexHash = usize;

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

    void SetVideoDecodeOptions(VideoDecodeOptions);

    Option<ExImageParameters> CreateExTex(u32 witdh, u32 height, VkFormat, VkImageTiling);
    rstd::Option<rstd::sync::Arc<TextureAllocation>> CreateTex(Image&);
    rstd::Option<rstd::sync::Arc<TextureAllocation>> AllocateTexture(TextureKey);

    /* Per-frame hook: advance every registered video-tex by `dt_seconds`,
     * pull as many decoded frames as needed to catch up to wall PTS,
     * convert NV12→RGBA on the CPU, and upload to the slot's stable
     * VkImage. No-op if no video textures are registered. */
    void PumpVideoTextures(double dt_seconds);

    /* vkCmdCopyBufferToImage a sub-rect of `atlas` into the supplied texture. */
    bool UploadFontAtlasRegion(ref<TextureAllocation> texture, const u8* atlas, u32 atlas_w, u32 x,
                               u32 y, u32 w, u32 h);

private:
    Option<VmaImageParameters> CreateTex(TextureKey);
    u64                        nextImageGeneration();
    void                       AssignImageGeneration(VmaImageParameters&);
    void                       AssignImageGeneration(ExImageParameters&);
    /* VIDEO-typed Image branch of CreateTex: registers a wavsen
     * VideoDecoder + stable RGBA8 VkImage and returns an ImageSlotsRef
     * pointing at that same VkImage so material binding is transparent. */
    rstd::Option<rstd::sync::Arc<TextureAllocation>> CreateVideoTex(Image&);
    void                                             allocateCmd();
    vvk::CommandBuffers                              m_tex_cmds;
    vvk::CommandBuffer                               m_tex_cmd;

    const Device&      m_device;
    VideoDecodeOptions m_video_decode_options;
    u64                m_next_image_generation { 1 };

    /* Opaque pImpl for the active video-tex set. Defined inside
     * TextureCache.cpp to keep wavsen.video out of the public
     * wescene.vulkan module interface. */
    struct VideoRegistry;
    Option<Box<VideoRegistry>> m_video_registry;
};

struct ImagePrepareBackend {
    using Trait                  = ImagePrepareBackend;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ImagePrepareBackend;

        auto CreateImportedTexture(rstd::mut_ref<Image> image)
            -> rstd::Option<rstd::sync::Arc<TextureAllocation>> {
            return rstd::trait_call<0>(this, image);
        }

        auto AllocateTexture(TextureKey key) -> rstd::Option<rstd::sync::Arc<TextureAllocation>> {
            return rstd::trait_call<1>(this, key);
        }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::CreateImportedTexture, &T::AllocateTexture>;
};

void RecordGenerateMipmaps(vvk::CommandBuffer&, const ImageParameters&);

// ---------- Device.hpp ----------

struct DeviceCapabilities {
    bool timeline_semaphore { false };
    bool synchronization2 { false };
    bool push_descriptor { false };
    bool memory_budget { false };
    bool external_memory_fd { false };
    bool external_memory_dma_buf { false };
    bool drm_format_modifier { false };
    bool foreign_queue { false };
    u32  graphics_queue_family { VK_QUEUE_FAMILY_IGNORED };
    u32  present_queue_family { VK_QUEUE_FAMILY_IGNORED };
};

struct MemoryBudgetSnapshot {
    VkDeviceSize usage { 0 };
    VkDeviceSize budget { 0 };

    VkDeviceSize available() const noexcept { return budget > usage ? budget - usage : 0; }
};

struct MemoryBudgetSource {
    using Trait                  = MemoryBudgetSource;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = MemoryBudgetSource;

        auto MemoryBudget() const -> MemoryBudgetSnapshot { return rstd::trait_call<0>(this); }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::MemoryBudget>;
};

struct PipelineParameters;

class BufferUploadPool;

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
    u32                          instance_api_version() const { return m_instance_api_version; }
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
    const auto& capabilities() const { return m_capabilities; }
    void        set_out_extent(VkExtent2D v) { m_extent = v; }

    bool supportExt(std::string_view) const;

    VkDeviceSize GetUsage() const;
    auto         MemoryBudget() const -> MemoryBudgetSnapshot;

private:
    std::vector<VkDeviceQueueCreateInfo> ChooseDeviceQueue(VkSurfaceKHR = {});

    vvk::DeviceDispatch     dld;
    VkInstance              m_instance { VK_NULL_HANDLE };
    u32                     m_instance_api_version { WP_VULKAN_VERSION };
    vvk::Device             m_device;
    vvk::PhysicalDevice     m_gpu;
    vvk::VmaAllocatorHandle m_allocator;

    VkPhysicalDeviceLimits   m_limits;
    DeviceCapabilities       m_capabilities;
    Set<std::string>         m_extensions;
    std::vector<std::string> m_enabled_instance_extensions;
    std::vector<std::string> m_enabled_device_extensions;

    Swapchain m_swapchain;

    vvk::CommandPool m_command_pool;

    QueueParameters m_graphics_queue;
    QueueParameters m_present_queue;

    VkExtent2D m_extent { 1, 1 };
};

// ---------- Util.hpp ----------

inline bool CreateStagingBuffer(VmaAllocator allocator, usize size, VmaBufferParameters& buffer) {
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
    usize                m_virtual_index { 0 };
};

class StagingBuffer : NoCopy, NoMove {
public:
    StagingBuffer(const Device&, VkDeviceSize size, VkBufferUsageFlags);
    ~StagingBuffer();

    bool allocate();
    void destroy();

    bool allocateSubRef(VkDeviceSize size, StagingBufferRef&, VkDeviceSize alignment = 1);
    void unallocateSubRef(const StagingBufferRef&);
    bool writeToBuf(const StagingBufferRef&, std::span<u8>, usize offset = 0);
    bool fillBuf(const StagingBufferRef& ref, usize offset, usize size, u8 c);

    bool recordUpload(vvk::CommandBuffer&);

    VkBuffer gpuBuf() const;

private:
    struct VirtualBlock {
        VmaVirtualBlock handle {};
        bool            enabled { false };
        usize           index { 0 };
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

// ---------- BufferUploadPool.hpp ----------

class BufferUploadPool;

class BufferAllocation {
public:
    BufferAllocation() = default;
    BufferAllocation(BufferUploadPool* owner, StagingBufferRef ref);
    ~BufferAllocation();

    BufferAllocation(const BufferAllocation&)            = delete;
    BufferAllocation& operator=(const BufferAllocation&) = delete;

    BufferAllocation(BufferAllocation&& o) noexcept;
    BufferAllocation& operator=(BufferAllocation&& o) noexcept;

    explicit operator bool() const noexcept { return m_owner != nullptr && m_ref.size > 0; }

    VkBuffer     buffer() const noexcept;
    VkDeviceSize offset() const noexcept { return m_ref.offset; }
    VkDeviceSize size() const noexcept { return m_ref.size; }

private:
    friend class BufferUploadPool;
    BufferUploadPool* m_owner { nullptr };
    StagingBufferRef  m_ref;
};

enum class BufferUploadClass
{
    Vertex,
    Index,
    Uniform,
    Storage,
    Transfer,
};

struct BufferUploadRequest {
    rstd::usize       size { 0 };
    rstd::usize       alignment { 1 };
    BufferUploadClass usage { BufferUploadClass::Vertex };
};

class BufferUploadPool : NoCopy, NoMove {
public:
    explicit BufferUploadPool(const Device&);
    ~BufferUploadPool();

    bool init();
    void destroy();

    Option<BufferAllocation> Upload(std::span<const u8> data, const BufferUploadRequest& request);
    bool                     Update(BufferAllocation&, std::span<const u8> data);
    void                     Release(StagingBufferRef ref);
    VkBuffer                 gpuBuf() const;
    bool                     recordPendingUploads(vvk::CommandBuffer& cmd);

private:
    const Device&              m_device;
    Option<Box<StagingBuffer>> m_buf;
    bool                       m_dirty { false };
};

struct BufferUploadBackend {
    using Trait                  = BufferUploadBackend;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = BufferUploadBackend;

        auto UploadBuffer(rstd::slice<rstd::u8> content, const BufferUploadRequest& request)
            -> rstd::Option<BufferAllocation> {
            return rstd::trait_call<0>(this, content, request);
        }

        bool UpdateBuffer(rstd::mut_ref<BufferAllocation> allocation,
                          rstd::slice<rstd::u8>           content) {
            return rstd::trait_call<1>(this, allocation, content);
        }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::UploadBuffer, &T::UpdateBuffer>;
};

// ---------- GraphicsPipeline.hpp ----------

struct PipelineParameters {
    vvk::Pipeline       handle;
    vvk::PipelineLayout layout;
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

    const ShaderSpv* getShaderSpv(VkShaderStageFlagBits) const;
    const auto&      pass() const { return m_pass; }

    GraphicsPipeline& setColorBlendStates(std::span<const VkPipelineColorBlendAttachmentState>);
    GraphicsPipeline& setColorBlendOptions(VkPipelineColorBlendStateCreateFlags,
                                           const rstd::array<float, 4>&);
    GraphicsPipeline& setLogicOp(bool enable, VkLogicOp);

    GraphicsPipeline& setRenderPass(vvk::RenderPass);
    GraphicsPipeline& setDescriptorSetLayouts(std::span<const VkDescriptorSetLayout>);
    GraphicsPipeline& addStage(Uni_ShaderSpv&&);
    GraphicsPipeline&
        addInputAttributeDescription(std::span<const VkVertexInputAttributeDescription>);
    GraphicsPipeline& addInputBindingDescription(std::span<const VkVertexInputBindingDescription>);
    GraphicsPipeline& setCreateInfoOptions(VkPipelineCreateFlags flags, u32 subpass);
    GraphicsPipeline& setTopology(VkPrimitiveTopology);
    GraphicsPipeline& setPrimitiveRestartEnable(bool);
    GraphicsPipeline& setViewportScissorCount(u32 viewport_count, u32 scissor_count);
    GraphicsPipeline& setDynamicStates(std::span<const VkDynamicState>);
    GraphicsPipeline& setSampleCount(VkSampleCountFlagBits);

private:
    vvk::RenderPass m_pass;

    VkPipelineCreateFlags m_create_flags { 0 };
    u32                   m_subpass { 0 };

    VkPipelineInputAssemblyStateCreateInfo         m_input_assembly {};
    std::vector<VkVertexInputBindingDescription>   m_input_bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> m_input_attr_descriptions;

    VkPipelineViewportStateCreateInfo                m_view;
    VkPipelineColorBlendStateCreateInfo              m_color;
    std::vector<VkDynamicState>                      m_dynamic_states;
    std::vector<VkPipelineColorBlendAttachmentState> m_color_attachments;
    std::vector<VkDescriptorSetLayout>               m_descriptor_set_layouts;
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
    LocalExSwapchain(rstd::array<LocalExHandle, 3> handles, VkExtent2D ext, u32 queue_family)
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
        const u32 slot_index     = static_cast<u32>(this->getInprogress()->id());
        const u64 acquire_serial = m_next_acquire_serial++;
        if (acquire_serial == 0) {
            return { .status     = ::owe::FrameSurfaceAcquireStatus::ProtocolError,
                     .error_code = -EOVERFLOW };
        }
        ::owe::FrameSurfaceLease lease {
            .identity       = { .owner_generation = 1,
                                .image_index      = slot_index,
                                .acquire_serial   = acquire_serial },
            .reuse          = { .kind = ::owe::FrameSurfaceReuseKind::QueueOrdered },
            .image          = ToImageParameters(m_handles.at(static_cast<usize>(slot_index)).image),
            .format         = VK_FORMAT_R8G8B8A8_UNORM,
            .initial_layout = VK_IMAGE_LAYOUT_GENERAL,
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
    rstd::array<::owe::ExHandle*, 3> snapshot_all_slots() override {
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

    rstd::array<LocalExHandle, 3> m_handles;
    std::atomic<::owe::ExHandle*> m_presented { nullptr };
    std::atomic<::owe::ExHandle*> m_ready { nullptr };
    std::atomic<::owe::ExHandle*> m_inprogress { nullptr };
    VkExtent2D                    m_extent;
    u32                           m_queue_family { VK_QUEUE_FAMILY_IGNORED };
    std::atomic<int>              m_last_sync_fd { -1 };
    u64                           m_next_acquire_serial { 1 };
    ::owe::FrameSurfaceIdentity   m_pending_identity;
    bool                          m_surface_pending { false };
};

inline std::shared_ptr<LocalExSwapchain> CreateLocalExSwapchain(const Device& device,
                                                                TextureCache& textures, unsigned w,
                                                                unsigned h, VkImageTiling tiling) {
    rstd::array<LocalExHandle, 3> handles;
    for (auto& handle : handles) {
        if (auto rv = textures.CreateExTex(w, h, VK_FORMAT_R8G8B8A8_UNORM, tiling); rv.is_some())
            handle.image = rstd::move(rv).unwrap();
        else
            return nullptr;
    }
    return std::make_shared<LocalExSwapchain>(
        std::move(handles), VkExtent2D { w, h }, device.graphics_queue().family_index);
}

} // namespace vulkan
} // namespace owe

export namespace rstd
{

template<>
struct Impl<owe::vulkan::MemoryBudgetSource, owe::vulkan::Device> : ImplBase<owe::vulkan::Device> {
    auto MemoryBudget() const -> owe::vulkan::MemoryBudgetSnapshot {
        return this->self().MemoryBudget();
    }
};

template<>
struct Impl<owe::vulkan::BufferUploadBackend, owe::vulkan::BufferUploadPool>
    : ImplBase<owe::vulkan::BufferUploadPool> {
    auto UploadBuffer(slice<u8> content, const owe::vulkan::BufferUploadRequest& request)
        -> Option<owe::vulkan::BufferAllocation> {
        auto uploaded =
            this->self().Upload(std::span<const u8>(content.as_raw_ptr(), content.len()), request);
        if (uploaded.is_none()) return None();
        return Some(rstd::move(uploaded).unwrap());
    }

    bool UpdateBuffer(mut_ref<owe::vulkan::BufferAllocation> allocation, slice<u8> content) {
        return this->self().Update(*allocation,
                                   std::span<const u8>(content.as_raw_ptr(), content.len()));
    }
};

template<>
struct Impl<owe::vulkan::ImagePrepareBackend, owe::vulkan::TextureCache>
    : ImplBase<owe::vulkan::TextureCache> {
    auto CreateImportedTexture(mut_ref<owe::Image> image)
        -> Option<sync::Arc<owe::vulkan::TextureAllocation>> {
        return this->self().CreateTex(*image);
    }

    auto AllocateTexture(owe::vulkan::TextureKey key)
        -> Option<sync::Arc<owe::vulkan::TextureAllocation>> {
        return this->self().AllocateTexture(rstd::move(key));
    }
};

} // namespace rstd
