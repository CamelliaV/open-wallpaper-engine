#include <gtest/gtest.h>

import rstd.cppstd;
import wescene.types;
import wescene.scene;
import wescene.vulkan_render;

namespace
{

owe::SceneMesh::Submesh MakeSubmesh() {
    std::vector<owe::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position", .type = owe::VertexType::FLOAT3 },
    };

    owe::SceneVertexArray vertices(attrs, 2);
    std::array<float, 6>  positions { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    (void)vertices.SetVertex("a_Position", positions);

    owe::SceneIndexArray    indices(3);
    std::array<uint32_t, 3> tri { 0, 1, 2 };
    indices.Assign(0, tri);

    owe::SceneMesh::Submesh submesh;
    submesh.vertex_arrays.push_back(std::move(vertices));
    submesh.index_arrays.push_back(std::move(indices));
    return submesh;
}

} // namespace

TEST(DrawBufferResourceName, UsesStableSceneDrawIdentity) {
    owe::SceneDrawItemId draw { .index = 7, .generation = 11 };

    auto vertex0 =
        owe::vulkan::BuildDrawBufferResourceName(draw, owe::vulkan::DrawBufferRole::Vertex, 0);
    auto vertex1 =
        owe::vulkan::BuildDrawBufferResourceName(draw, owe::vulkan::DrawBufferRole::Vertex, 1);
    auto index = owe::vulkan::BuildDrawBufferResourceName(draw, owe::vulkan::DrawBufferRole::Index);
    auto uniform =
        owe::vulkan::BuildDrawBufferResourceName(draw, owe::vulkan::DrawBufferRole::Uniform);

    EXPECT_EQ(rstd::cppstd::as_string_view(vertex0.as_str()), "draw:11:7:vertex:0");
    EXPECT_EQ(rstd::cppstd::as_string_view(vertex1.as_str()), "draw:11:7:vertex:1");
    EXPECT_EQ(rstd::cppstd::as_string_view(index.as_str()), "draw:11:7:index:0");
    EXPECT_EQ(rstd::cppstd::as_string_view(uniform.as_str()), "draw:11:7:uniform:0");
    EXPECT_TRUE(owe::vulkan::BuildDrawBufferResourceName({}, owe::vulkan::DrawBufferRole::Vertex)
                    .is_empty());
}

TEST(DrawBufferKey, BuildsStaticKeysFromRenderItemAndGeometryGeneration) {
    owe::SceneMesh mesh;
    mesh.Submeshes().push_back(MakeSubmesh());

    owe::RenderItemId              render_item { .index = 7, .generation = 11 };
    owe::vulkan::DrawBufferRequest request { .render_item   = render_item,
                                             .mesh          = &mesh,
                                             .submesh_index = 0 };

    auto keys = owe::vulkan::BuildDrawBufferKeys(request, 99);
    ASSERT_EQ(keys.size(), 2u);

    const auto& vertex = mesh.Submeshes()[0].vertex_arrays[0];
    EXPECT_EQ(keys[0].render_item.index, render_item.index);
    EXPECT_EQ(keys[0].render_item.generation, render_item.generation);
    EXPECT_EQ(keys[0].role, owe::vulkan::DrawBufferRole::Vertex);
    EXPECT_EQ(keys[0].submesh_index, 0u);
    EXPECT_EQ(keys[0].stream_index, 0u);
    EXPECT_EQ(keys[0].data_generation, vertex.DataGeneration());
    EXPECT_EQ(keys[0].allocation_generation, 0u);

    const auto& index = mesh.Submeshes()[0].index_arrays[0];
    EXPECT_EQ(keys[1].role, owe::vulkan::DrawBufferRole::Index);
    EXPECT_EQ(keys[1].data_generation, index.DataGeneration());
    EXPECT_EQ(keys[1].allocation_generation, 0u);
}

TEST(DrawBufferKey, KeepsDynamicAllocationGenerationSeparateFromDataGeneration) {
    owe::SceneMesh mesh(true);
    mesh.Submeshes().push_back(MakeSubmesh());

    owe::RenderItemId              render_item { .index = 3, .generation = 5 };
    owe::vulkan::DrawBufferRequest request { .render_item   = render_item,
                                             .mesh          = &mesh,
                                             .submesh_index = 0 };

    auto keys = owe::vulkan::BuildDrawBufferKeys(request, 77);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0].allocation_generation, 77u);
    EXPECT_EQ(keys[1].allocation_generation, 77u);
    EXPECT_EQ(keys[0].data_generation, mesh.Submeshes()[0].vertex_arrays[0].DataGeneration());
    EXPECT_EQ(keys[1].data_generation, mesh.Submeshes()[0].index_arrays[0].DataGeneration());
}

TEST(DrawBufferKey, ReturnsEmptyForInvalidRequest) {
    owe::SceneMesh mesh;
    EXPECT_TRUE(owe::vulkan::BuildDrawBufferKeys({ .mesh = nullptr }, 1).empty());
    EXPECT_TRUE(owe::vulkan::BuildDrawBufferKeys({ .mesh = &mesh, .submesh_index = 1 }, 1).empty());
}
