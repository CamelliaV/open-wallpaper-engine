#include <gtest/gtest.h>

#include "RenderGraph/Pass.hpp"

import rstd.cppstd;
import wescene.rgraph;
import wescene.vulkan_render;

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

TEST(RenderGraphDebug, GraphvizIncludesResourceRefsAndAccessLabels) {
    owe::rg::RenderGraph graph;

    graph.addPass<DebugPass>("draw/main",
                             owe::rg::PassNode::Type::CustomShader,
                             [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 auto input = builder.createTexture(owe::rg::TextureDesc {
                                     .name = "albedo",
                                     .key  = "tex/albedo",
                                     .kind = owe::rg::TextureKind::Imported,
                                 });
                                 builder.read(input);

                                 auto output = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = "default",
                                         .key  = "_rt_default",
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
                                         .name = "default",
                                         .key  = "_rt_default",
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(output);
                             });

    auto path = std::filesystem::temp_directory_path() / "owe-render-graph-debug-test.dot";
    graph.ToGraphviz(path.native());

    auto dot = ReadFile(path);
    EXPECT_NE(dot.find("ref=n"), std::string::npos);
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

    auto* pass = graph.addPass<DebugPass>("draw/main",
                                          owe::rg::PassNode::Type::CustomShader,
                                          [](owe::rg::RenderGraphBuilder&, DebugPass::Desc&) {
                                          });

    auto state = graph.passState(pass->ID());
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->id, pass->ID());
    EXPECT_EQ(state->name, "draw/main");
    EXPECT_EQ(state->type, owe::rg::PassNode::Type::CustomShader);
    EXPECT_FALSE(graph.passState(std::numeric_limits<owe::rg::NodeID>::max()).has_value());
}

TEST(VulkanRenderDiagnostics, EmptyBeforeProgramBuild) {
    owe::vulkan::VulkanRender render;
    EXPECT_TRUE(render.preparedPassDiagnostics().empty());
}
