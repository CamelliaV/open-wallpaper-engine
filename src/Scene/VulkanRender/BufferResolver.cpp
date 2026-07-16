module wescene.vulkan_render;
import wescene.core;
import rstd;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace rstd::prelude;

namespace owe::vulkan
{

namespace
{

auto bytesOf(const float* data, usize size) -> rstd::slice<rstd::u8> {
    return rstd::slice<rstd::u8>::from_raw_parts(reinterpret_cast<const rstd::u8*>(data), size);
}

auto bytesOf(const u32* data, usize size) -> rstd::slice<u8> {
    return rstd::slice<rstd::u8>::from_raw_parts(reinterpret_cast<const rstd::u8*>(data), size);
}

u64 next_dynamic_allocation_generation() {
    static std::atomic_uint64_t next { 1 };
    return next.fetch_add(1, std::memory_order_relaxed);
}

u64 resolve_allocation_generation(const DrawBufferRequest& request, bool dynamic) {
    if (! dynamic) return 0;
    if (request.dynamic_allocation_generation != 0) return request.dynamic_allocation_generation;
    return next_dynamic_allocation_generation();
}

} // namespace

RenderBufferResolver::RenderBufferResolver(
    const resource_registry::PreparedResourceTable& resources)
    : m_resources(rstd::ref<resource_registry::PreparedResourceTable>::from_raw_parts(
          rstd::addressof(resources))) {}

std::vector<DrawBufferKey> BuildDrawBufferKeys(const DrawBufferRequest& request,
                                               u64                      allocation_generation) {
    std::vector<DrawBufferKey> keys;
    if (request.mesh == nullptr) return keys;
    const SceneMesh& mesh = *request.mesh;
    if (request.submesh_index >= mesh.Submeshes().size()) return keys;

    const auto& submesh = mesh.Submeshes()[request.submesh_index];
    keys.reserve(submesh.vertex_arrays.size() + (submesh.index_arrays.empty() ? 0u : 1u));
    const u64 resolved_allocation = mesh.Dynamic() ? allocation_generation : 0;

    for (usize i = 0; i < submesh.vertex_arrays.size(); ++i) {
        const auto& vertex = submesh.vertex_arrays[i];
        keys.push_back(DrawBufferKey { .render_item           = request.render_item,
                                       .role                  = DrawBufferRole::Vertex,
                                       .submesh_index         = request.submesh_index,
                                       .stream_index          = static_cast<u32>(i),
                                       .data_generation       = vertex.DataGeneration(),
                                       .allocation_generation = resolved_allocation });
    }

    if (! submesh.index_arrays.empty()) {
        const auto& index = submesh.index_arrays[0];
        keys.push_back(DrawBufferKey { .render_item           = request.render_item,
                                       .role                  = DrawBufferRole::Index,
                                       .submesh_index         = request.submesh_index,
                                       .stream_index          = 0,
                                       .data_generation       = index.DataGeneration(),
                                       .allocation_generation = resolved_allocation });
    }

    return keys;
}

Option<DrawBufferRefs> RenderBufferResolver::prepareDrawBuffers(const DrawBufferRequest& request) {
    if (request.mesh == nullptr) return None();
    SceneMesh& mesh          = *request.mesh;
    const auto submesh_index = request.submesh_index;
    if (mesh.Submeshes().empty() || submesh_index >= mesh.Submeshes().size()) return None();

    const auto& submesh = mesh.Submeshes()[submesh_index];

    DrawBufferRefs out;
    out.render_item           = request.render_item;
    out.dynamic               = mesh.Dynamic();
    out.allocation_generation = resolve_allocation_generation(request, out.dynamic);
    auto keys                 = BuildDrawBufferKeys(request, out.allocation_generation);
    out.vertex_keys.reserve(submesh.vertex_arrays.size());

    for (usize i = 0; i < submesh.vertex_arrays.size(); i++) {
        const auto& vertex = submesh.vertex_arrays[i];
        out.vertex_keys.push_back(keys[i]);
        out.draw_count += static_cast<u32>(vertex.DataSize() / vertex.OneSize());

        if (i >= request.buffer_uses.len()) return None();
        auto prepared = m_resources->Resolve(request.buffer_uses[i]);
        if (prepared.is_none()) return None();
        out.vertices.push(resource::BufferUseHandle(request.buffer_uses[i]));
    }

    if (! submesh.index_arrays.empty()) {
        const auto& index = submesh.index_arrays[0];
        out.draw_count    = static_cast<u32>(index.DataCount());
        out.index_key     = Some<DrawBufferKey>(keys.back());

        auto use_index = submesh.vertex_arrays.size();
        if (use_index >= request.buffer_uses.len()) return None();
        auto prepared = m_resources->Resolve(request.buffer_uses[use_index]);
        if (prepared.is_none()) return None();
        out.index = Some(resource::BufferUseHandle(request.buffer_uses[use_index]));
    }

    return Some(rstd::move(out));
}

bool RenderBufferResolver::updateDynamicDrawBuffers(
    const DrawBufferRequest& request, DrawBufferRefs& buffers,
    rstd::mut_ref<rstd::dyn<resource::BufferContentWriter>> writer) {
    if (! buffers.dynamic) return true;
    if (request.mesh == nullptr) return false;
    SceneMesh& mesh          = *request.mesh;
    const auto submesh_index = request.submesh_index;
    if ((mesh.DirtyFlags() & SceneMeshDirtyData) == 0) return true;
    if (submesh_index >= mesh.Submeshes().size()) return true;

    const auto& submesh           = mesh.Submeshes()[submesh_index];
    auto        require_reprepare = [&mesh] {
        mesh.SetLayoutDirty();
        return false;
    };
    if (buffers.vertices.len() != submesh.vertex_arrays.size()) return require_reprepare();
    if (buffers.vertex_keys.size() != submesh.vertex_arrays.size()) return require_reprepare();
    if (buffers.index.is_some() != ! submesh.index_arrays.empty()) return require_reprepare();

    for (usize i = 0; i < submesh.vertex_arrays.size(); i++) {
        const auto& vertex                     = submesh.vertex_arrays[i];
        buffers.vertex_keys[i].data_generation = vertex.DataGeneration();
        auto updated = writer->UpdateBuffer(buffers.vertices[i],
                                            bytesOf(vertex.Data(), vertex.DataSizeOf()),
                                            vertex.DataGeneration());
        if (updated.is_err()) return require_reprepare();
    }

    if (! submesh.index_arrays.empty()) {
        const auto& index  = submesh.index_arrays[0];
        buffers.draw_count = static_cast<u32>(index.RenderDataCount());
        if (buffers.index_key.is_some()) {
            buffers.index_key->data_generation = index.DataGeneration();
        }
        auto updated = writer->UpdateBuffer(
            *buffers.index, bytesOf(index.Data(), index.DataSizeOf()), index.DataGeneration());
        if (updated.is_err()) return require_reprepare();
    } else if (! submesh.vertex_arrays.empty()) {
        buffers.draw_count = static_cast<u32>(submesh.vertex_arrays[0].VertexCount());
    }

    (void)mesh.ConsumeDirtyFlags(SceneMeshDirtyData);
    return true;
}

} // namespace owe::vulkan
