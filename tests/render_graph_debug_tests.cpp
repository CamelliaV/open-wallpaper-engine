#include <gtest/gtest.h>

import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.rgraph;
import wescene.vulkan_render;

using namespace rstd::prelude;

namespace
{

class DebugPass : public owe::rg::Pass {
public:
    struct Desc {};
    explicit DebugPass(const Desc&) {}
};

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

} // namespace

TEST(DependencyGraph, StoresHandlesAndDeduplicatesEdges) {
    owe::rg::DependencyGraph graph;
    auto                     first  = graph.AddNode();
    auto                     second = graph.AddNode();
    auto                     third  = graph.AddNode();

    EXPECT_TRUE(first.valid());
    EXPECT_TRUE(graph.Connect(first, second));
    EXPECT_TRUE(graph.Connect(first, second));
    EXPECT_TRUE(graph.Connect(second, third));
    EXPECT_FALSE(graph.Connect(first, owe::rg::NodeHandle {}));
    EXPECT_EQ(graph.NodeNum(), 3u);
    EXPECT_EQ(graph.EdgeNum(), 2u);
    EXPECT_EQ(graph.GetNodeOut(first).len(), 1u);
    EXPECT_EQ(graph.GetNodeIn(third).len(), 1u);

    auto order = graph.TopologicalOrder();
    ASSERT_EQ(order.len(), 3u);
    EXPECT_EQ(order[0], first);
    EXPECT_EQ(order[1], second);
    EXPECT_EQ(order[2], third);
    EXPECT_FALSE(graph.HasCycle());

    EXPECT_TRUE(graph.Connect(third, first));
    EXPECT_TRUE(graph.HasCycle());
}

TEST(RenderGraphDebug, GraphvizIncludesResourceRefsAndAccessLabels) {
    owe::rg::RenderGraph graph;

    graph.addPass<DebugPass>("draw/main",
                             owe::rg::PassNode::Type::CustomShader,
                             [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 auto input = builder.createTexture(owe::rg::TextureDesc {
                                     .name = String::make("albedo"),
                                     .key  = String::make("tex/albedo"),
                                     .kind = owe::rg::TextureKind::Imported,
                                 });
                                 builder.read(input);

                                 auto output = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = String::make("default"),
                                         .key  = String::make("_rt_default"),
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(output);
                             });

    graph.addPass<DebugPass>("draw/overlay",
                             owe::rg::PassNode::Type::CustomShader,
                             [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 auto output = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = String::make("default"),
                                         .key  = String::make("_rt_default"),
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(output);
                             });

    auto path = std::filesystem::temp_directory_path() / "owe-render-graph-debug-test.dot";
    graph.ToGraphviz(path.native());

    auto dot = ReadFile(path);
    EXPECT_NE(dot.find("ref=n"), std::string::npos);
    EXPECT_NE(dot.find("pass-ref=p"), std::string::npos);
    EXPECT_NE(dot.find("pass: draw/main"), std::string::npos);
    EXPECT_NE(dot.find("type=CustomShader"), std::string::npos);
    EXPECT_NE(dot.find("resource: albedo"), std::string::npos);
    EXPECT_NE(dot.find("key=tex/albedo"), std::string::npos);
    EXPECT_NE(dot.find("kind=Imported"), std::string::npos);
    EXPECT_NE(dot.find("resource: default"), std::string::npos);
    EXPECT_NE(dot.find("kind=Temp"), std::string::npos);
    EXPECT_NE(dot.find("version=1"), std::string::npos);
    EXPECT_NE(dot.find("access=read"), std::string::npos);
    EXPECT_NE(dot.find("access=read/version"), std::string::npos);
    EXPECT_NE(dot.find("access=write"), std::string::npos);
}

TEST(RenderGraphDebug, PassStateExposesPublicDebugRecord) {
    owe::rg::RenderGraph graph;

    auto pass = graph.addPass<DebugPass>("draw/main",
                                         owe::rg::PassNode::Type::CustomShader,
                                         [](owe::rg::RenderGraphBuilder&, DebugPass::Desc&) {
                                         });

    auto state = graph.passState(pass);
    ASSERT_TRUE(state.is_some());
    EXPECT_EQ(state->handle, pass);
    EXPECT_TRUE(state->pass.valid());
    EXPECT_TRUE(graph.getPass(state->pass).is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(state->name.as_str()), "draw/main");
    EXPECT_EQ(state->type, owe::rg::PassNode::Type::CustomShader);
    EXPECT_TRUE(graph.passState(owe::rg::NodeHandle {}).is_none());
    EXPECT_TRUE(graph.getPass(owe::rg::PassHandle {}).is_none());
}

TEST(RenderGraphResources, CompilesBackendNeutralTexturePlan) {
    owe::rg::RenderGraph graph;

    graph.addPass<DebugPass>(
        "draw/main",
        owe::rg::PassNode::Type::CustomShader,
        [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
            auto input = builder.createTexture(owe::rg::TextureDesc {
                .name    = String::make("albedo"),
                .key     = String::make("tex/albedo"),
                .kind    = owe::rg::TextureKind::Imported,
                .request = rstd::Some(owe::resource::TextureRequest {
                    .kind = owe::resource::TextureRequestKind::Imported,
                    .name = String::make("tex/albedo"),
                }),
            });
            builder.read(input);

            auto output = builder.createTexture(
                owe::rg::TextureDesc {
                    .name    = String::make("default"),
                    .key     = String::make("_rt_default"),
                    .kind    = owe::rg::TextureKind::Temp,
                    .request = rstd::Some(owe::resource::TextureRequest {
                        .kind       = owe::resource::TextureRequestKind::RenderTarget,
                        .name       = String::make("_rt_default"),
                        .definition = rstd::Some(owe::resource::TextureDefinition {
                            .width  = 1920,
                            .height = 1080,
                        }),
                    }),
                },
                true);
            builder.write(output);
        });

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), 2u);
    EXPECT_TRUE(plan.textures[0].handle.Valid());
    EXPECT_EQ(plan.textures[0].access, owe::resource::ResourceAccess::Read);
    EXPECT_EQ(plan.textures[0].request.kind, owe::resource::TextureRequestKind::Imported);
    EXPECT_TRUE(plan.textures[1].handle.Valid());
    EXPECT_EQ(plan.textures[1].access, owe::resource::ResourceAccess::Write);
    ASSERT_TRUE(plan.textures[1].request.definition.is_some());
    EXPECT_EQ(plan.textures[1].request.definition->width, 1920);
}

TEST(RenderGraphResources, UsesGraphKeyAsTextureRequestIdentity) {
    owe::rg::RenderGraph graph;
    graph.addPass<DebugPass>(
        "copy/snapshot",
        owe::rg::PassNode::Type::Copy,
        [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
            auto snapshot = builder.createTexture(
                owe::rg::TextureDesc {
                    .name    = String::make("_rt_default_1_copy"),
                    .key     = String::make("_rt_default_1_copy"),
                    .kind    = owe::rg::TextureKind::Temp,
                    .request = rstd::Some(owe::resource::TextureRequest {
                        .kind = owe::resource::TextureRequestKind::RenderTarget,
                        .name = String::make("_rt_default"),
                    }),
                },
                true);
            builder.write(snapshot);
        });

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), 1u);
    EXPECT_EQ(rstd::cppstd::as_string_view(plan.textures[0].request.name.as_str()),
              "_rt_default_1_copy");
}

TEST(VulkanRenderDiagnostics, EmptyBeforeProgramBuild) {
    owe::vulkan::VulkanRender render;
    EXPECT_TRUE(render.preparedPassDiagnostics().empty());
}
