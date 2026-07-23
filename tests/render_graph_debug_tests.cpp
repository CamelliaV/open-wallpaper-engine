#include <gtest/gtest.h>

import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.rgraph;
import wescene.vulkan_render;

using namespace rstd::prelude;
using namespace rstd::literals;

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
    EXPECT_EQ(graph.NodeNum(), rstd::usize(3));
    EXPECT_EQ(graph.EdgeNum(), rstd::usize(2));
    EXPECT_EQ(graph.GetNodeOut(first).len(), rstd::usize(1));
    EXPECT_EQ(graph.GetNodeIn(third).len(), rstd::usize(1));

    auto order = graph.TopologicalOrder();
    ASSERT_EQ(order.len(), rstd::usize(3));
    EXPECT_EQ(order[rstd::usize()], first);
    EXPECT_EQ(order[rstd::usize(1)], second);
    EXPECT_EQ(order[rstd::usize(2)], third);
    EXPECT_FALSE(graph.HasCycle());

    EXPECT_TRUE(graph.Connect(third, first));
    EXPECT_TRUE(graph.HasCycle());
}

TEST(RenderGraphDebug, GraphvizIncludesResourceRefsAndAccessLabels) {
    owe::rg::RenderGraph graph;

    graph.addPass<DebugPass>("draw/main"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 auto input = builder.createTexture(owe::rg::TextureDesc {
                                     .name = String::make("albedo"_str),
                                     .key  = String::make("tex/albedo"_str),
                                     .kind = owe::rg::TextureKind::Imported,
                                 });
                                 builder.read(input);

                                 auto output = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = String::make("default"_str),
                                         .key  = String::make("_rt_default"_str),
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(output);
                             });

    graph.addPass<DebugPass>("draw/overlay"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 auto output = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = String::make("default"_str),
                                         .key  = String::make("_rt_default"_str),
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(output);
                             });

    auto path = std::filesystem::temp_directory_path() / "owe-render-graph-debug-test.dot";
    graph.ToGraphviz(rstd::cppstd::as_str(path.native()).unwrap());

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

    auto pass = graph.addPass<DebugPass>("draw/main"_str,
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
        "draw/main"_str,
        owe::rg::PassNode::Type::CustomShader,
        [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
            auto input = builder.createTexture(owe::rg::TextureDesc {
                .name    = String::make("albedo"_str),
                .key     = String::make("tex/albedo"_str),
                .kind    = owe::rg::TextureKind::Imported,
                .request = rstd::Some(owe::resource::TextureRequest {
                    .kind = owe::resource::TextureRequestKind::Imported,
                    .name = String::make("tex/albedo"_str),
                }),
            });
            builder.read(input);

            auto output = builder.createTexture(
                owe::rg::TextureDesc {
                    .name    = String::make("default"_str),
                    .key     = String::make("_rt_default"_str),
                    .kind    = owe::rg::TextureKind::Temp,
                    .request = rstd::Some(owe::resource::TextureRequest {
                        .kind       = owe::resource::TextureRequestKind::RenderTarget,
                        .name       = String::make("_rt_default"_str),
                        .definition = rstd::Some(owe::resource::TextureDefinition {
                            .width  = rstd::i32(1920),
                            .height = rstd::i32(1080),
                        }),
                    }),
                },
                true);
            builder.write(output);
        });

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), rstd::usize(2));
    EXPECT_TRUE(plan.textures[rstd::usize()].handle.Valid());
    EXPECT_EQ(plan.textures[rstd::usize()].access, owe::resource::ResourceAccess::Read);
    EXPECT_EQ(plan.textures[rstd::usize()].request.kind,
              owe::resource::TextureRequestKind::Imported);
    EXPECT_TRUE(plan.textures[rstd::usize(1)].handle.Valid());
    EXPECT_EQ(plan.textures[rstd::usize(1)].access, owe::resource::ResourceAccess::Write);
    ASSERT_TRUE(plan.textures[rstd::usize(1)].request.definition.is_some());
    EXPECT_EQ(plan.textures[rstd::usize(1)].request.definition->width, rstd::i32(1920));
}

TEST(RenderGraphResources, UsesGraphKeyAsTextureRequestIdentity) {
    owe::rg::RenderGraph graph;
    graph.addPass<DebugPass>(
        "copy/snapshot"_str,
        owe::rg::PassNode::Type::Copy,
        [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
            auto snapshot = builder.createTexture(
                owe::rg::TextureDesc {
                    .name    = String::make("_rt_default_1_copy"_str),
                    .key     = String::make("_rt_default_1_copy"_str),
                    .kind    = owe::rg::TextureKind::Temp,
                    .request = rstd::Some(owe::resource::TextureRequest {
                        .kind = owe::resource::TextureRequestKind::RenderTarget,
                        .name = String::make("_rt_default"_str),
                    }),
                },
                true);
            builder.write(snapshot);
        });

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), rstd::usize(1));
    EXPECT_EQ(rstd::cppstd::as_string_view(plan.textures[rstd::usize()].request.name.as_str()),
              "_rt_default_1_copy");
}

TEST(RenderGraphResources, PreservesFrameBoundaryTextureVersions) {
    owe::rg::RenderGraph graph;
    auto                 history      = owe::rg::TextureNodeRef {};
    auto                 history_desc = [] {
        return owe::rg::TextureDesc {
            .name    = String::make("history"_str),
            .key     = String::make("history"_str),
            .kind    = owe::rg::TextureKind::Temp,
            .request = rstd::Some(owe::resource::TextureRequest {
                .kind       = owe::resource::TextureRequestKind::RenderTarget,
                .name       = String::make("history"_str),
                .definition = rstd::Some(owe::resource::TextureDefinition {
                    .width  = rstd::i32(1920),
                    .height = rstd::i32(1080),
                }),
                .lifetime   = owe::resource::TextureLifetimeClass::FrameLocal,
            }),
        };
    };

    graph.addPass<DebugPass>("motion/accumulate"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 history = builder.createTexture(history_desc());
                                 builder.markVirtualWrite(history);
                                 builder.read(history);
                             });
    graph.addPass<DebugPass>("motion/store"_str,
                             owe::rg::PassNode::Type::Copy,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 auto next = builder.createTexture(history_desc(), true);
                                 builder.write(next);
                             });

    auto plan  = graph.resourcePlan();
    auto order = graph.topologicalOrder();
    ASSERT_EQ(order.len(), rstd::usize(2));
    EXPECT_EQ(rstd::cppstd::as_string_view(graph.passState(order[rstd::usize()])->name.as_str()),
              "motion/accumulate");
    EXPECT_EQ(rstd::cppstd::as_string_view(graph.passState(order[rstd::usize(1)])->name.as_str()),
              "motion/store");
    ASSERT_EQ(plan.textures.len(), rstd::usize(2));
    for (const auto& entry : plan.textures) {
        EXPECT_EQ(entry.request.lifetime, owe::resource::TextureLifetimeClass::Retained);
        EXPECT_NE(entry.request.content & owe::resource::TextureContentFlag(
                                              owe::resource::TextureContent::PreserveAcrossFrames),
                  rstd::u32());
    }
}

TEST(VulkanRenderDiagnostics, EmptyBeforeProgramBuild) {
    owe::vulkan::VulkanRender render;
    EXPECT_TRUE(render.preparedPassDiagnostics().empty());
}
