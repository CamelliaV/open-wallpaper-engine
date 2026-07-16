module;

export module wescene.vulkan_render:buffer_resolver;
import wescene.core;
import rstd;
import rstd.cppstd;
import wescene.resource_registry;
import wescene.vulkan;
import wescene.scene;

export namespace owe::vulkan
{

enum class DrawBufferRole
{
    Vertex,
    Index,
};

struct DrawBufferKey {
    RenderItemId   render_item;
    DrawBufferRole role { DrawBufferRole::Vertex };
    uint32_t       submesh_index { 0 };
    uint32_t       stream_index { 0 };
    uint64_t       data_generation { 0 };
    uint64_t       allocation_generation { 0 };
};

struct DrawBufferRefs {
    RenderItemId render_item;
    uint64_t     allocation_generation { 0 };
    bool         dynamic { false };
    u32          draw_count { 0 };

    std::vector<DrawBufferKey>   vertex_keys;
    std::optional<DrawBufferKey> index_key;

    rstd::vec::Vec<resource_registry::PreparedBuffer> static_vertices;
    rstd::Option<resource_registry::PreparedBuffer>   static_index;

    std::vector<StagingBufferRef> dynamic_vertices;
    StagingBufferRef              dynamic_index;

    DrawBufferRefs()                                     = default;
    DrawBufferRefs(const DrawBufferRefs&)                = delete;
    DrawBufferRefs& operator=(const DrawBufferRefs&)     = delete;
    DrawBufferRefs(DrawBufferRefs&&) noexcept            = default;
    DrawBufferRefs& operator=(DrawBufferRefs&&) noexcept = default;

    bool hasIndex() const {
        return dynamic ? static_cast<bool>(dynamic_index) : static_index.is_some();
    }
};

struct DrawBufferRequest {
    RenderItemId render_item;
    SceneMesh*   mesh { nullptr };
    uint32_t     submesh_index { 0 };
    uint64_t     dynamic_allocation_generation { 0 };
};

std::vector<DrawBufferKey> BuildDrawBufferKeys(const DrawBufferRequest&,
                                               uint64_t allocation_generation = 0);

class RenderBufferResolver {
public:
    RenderBufferResolver(resource_registry::BufferRegistry&, MeshCache&, StagingBuffer&);

    std::optional<DrawBufferRefs> prepareDrawBuffers(const DrawBufferRequest&);
    bool updateDynamicDrawBuffers(const DrawBufferRequest&, DrawBufferRefs&);
    void releaseDynamicDrawBuffers(DrawBufferRefs&);

private:
    rstd::mut_ref<resource_registry::BufferRegistry> m_buffers;
    rstd::mut_ref<MeshCache>                         m_mesh_cache;
    StagingBuffer&                                   m_dynamic_buffer;
};

} // namespace owe::vulkan
