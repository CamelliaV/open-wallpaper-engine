#include <gtest/gtest.h>

import eigen;
import rstd.cppstd;
import wescene.fs;
import wescene.pkg_fs;
import wescene.pkg.parse;
import wescene.scene;
import wescene.spec_names;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

std::uint32_t MaxMeshIndex(const owe::WPMdl::Mesh& mesh) {
    std::uint32_t max_index = 0;
    for (const auto& tri : mesh.indices) {
        for (std::uint32_t idx : tri) max_index = std::max(max_index, idx);
    }
    return max_index;
}

std::uint32_t CountUvSeamTriangles(const owe::WPMdl::Mesh& mesh) {
    std::uint32_t seam_triangles = 0;
    for (const auto& tri : mesh.indices) {
        float min_u = std::numeric_limits<float>::max();
        float max_u = std::numeric_limits<float>::lowest();
        for (std::uint32_t idx : tri) {
            min_u = std::min(min_u, mesh.texcoords[usize(idx)][usize(0)]);
            max_u = std::max(max_u, mesh.texcoords[usize(idx)][usize(0)]);
        }
        if (max_u - min_u > 0.5f) ++seam_triangles;
    }
    return seam_triangles;
}

} // namespace

TEST(WPPuppet, ArcOwnedLayerExposesBorrowedTransforms) {
    auto                puppet = Arc<owe::WPPuppet>::make();
    owe::WPPuppet::Bone bone;
    bone.name = String::make("root"_str);
    puppet->bones.push(rstd::move(bone));
    puppet->prepared();

    owe::WPPuppetLayer layer(puppet.clone());
    layer.prepared(slice<owe::WPPuppetLayer::AnimationLayer> {});

    EXPECT_EQ(layer.boneIndex("root"_str), 1u);
    EXPECT_EQ(layer.boneIndex("missing"_str), 0u);
    EXPECT_TRUE(layer.boneTransform(0u, 0.0).is_none());
    auto transform = layer.boneTransform(1u, 0.0);
    ASSERT_TRUE(transform.is_some());
    EXPECT_TRUE(transform->matrix().isApprox(Eigen::Matrix4f::Identity()));
}

TEST(WPMdlMesh, KeepsPuppetPositionsInMdlLocalSpace) {
    owe::WPMdl::Mesh source;
    source.positions.push(array<float, 3> { 244.0f, 349.5f, 0.0f });
    source.texcoords.push(array<float, 2> { 0.25f, 0.75f });
    source.indices.push(array<std::uint32_t, 3> { 0u, 0u, 0u });

    owe::SceneMesh::Submesh submesh;
    owe::WPMdlParser::GenMeshFromMdl(submesh, source);

    ASSERT_EQ(submesh.vertex_arrays.size(), 1u);
    const auto& vertices = submesh.vertex_arrays.front();
    ASSERT_NE(vertices.Data(), nullptr);
    EXPECT_FLOAT_EQ(vertices.Data()[0], 244.0f);
    EXPECT_FLOAT_EQ(vertices.Data()[1], 349.5f);
    EXPECT_FLOAT_EQ(vertices.Data()[2], 0.0f);
}

TEST(WPMdlMesh, Mdlv23LargeStaticMeshUsesUint32GlobalIndices) {
    const std::filesystem::path pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3557068717" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) {
        GTEST_SKIP() << "workshop 3557068717 is not available";
    }

    owe::fs::VFS vfs;
    auto         assets_fs = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    if (assets_fs.is_ok()) {
        ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets_fs).unwrap_unchecked()).is_ok());
    }
    auto pkg_fs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path.string()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());

    owe::WPMdl mdl;
    ASSERT_TRUE(owe::WPMdlParser::Parse("models/球体01/球体01.mdl"_str, vfs, mdl));
    ASSERT_FALSE(mdl.meshes.is_empty());

    const auto& mesh = mdl.meshes[usize()];
    ASSERT_EQ(mdl.header.mdlv, 23);
    ASSERT_EQ(mesh.positions.len(), usize(520192));
    ASSERT_EQ(mesh.texcoords.len(), mesh.positions.len());
    ASSERT_EQ(mesh.indices.len(), usize(260096));
    ASSERT_LT(MaxMeshIndex(mesh), mesh.positions.len().to_primitive());
    EXPECT_EQ(mesh.indices[usize(0)], (array<std::uint32_t, 3> { 0u, 1u, 2u }));
    EXPECT_EQ(mesh.indices[usize(1)], (array<std::uint32_t, 3> { 0u, 2u, 3u }));
    EXPECT_EQ(mesh.indices[mesh.indices.len() - usize(1)],
              (array<std::uint32_t, 3> { 520188u, 520190u, 520191u }));
    EXPECT_EQ(CountUvSeamTriangles(mesh), 0u);

    owe::SceneMesh::Submesh submesh;
    owe::WPMdlParser::GenMeshFromMdl(submesh, mesh);
    ASSERT_EQ(submesh.vertex_arrays.size(), 1u);
    ASSERT_EQ(submesh.index_arrays.size(), 1u);
    EXPECT_TRUE(submesh.draw_ranges.empty());

    const auto& index_array = submesh.index_arrays.front();
    ASSERT_EQ(index_array.DataCount(), rstd::usize(780288));
    EXPECT_EQ(index_array.Data()[0], 0u);
    EXPECT_EQ(index_array.Data()[1], 1u);
    EXPECT_EQ(index_array.Data()[2], 2u);
    EXPECT_EQ(index_array.Data()[780285], 520188u);
    EXPECT_EQ(index_array.Data()[780286], 520190u);
    EXPECT_EQ(index_array.Data()[780287], 520191u);
}
