#include <gtest/gtest.h>

import rstd;
import rstd.cppstd;
import eigen;
import wescene.types;
import wescene.scene;

namespace
{

std::shared_ptr<owe::SceneMesh> MakeSingleSubmesh(std::string name) {
    auto mesh = std::make_shared<owe::SceneMesh>();

    owe::SceneMaterial material;
    material.name = std::move(name);
    mesh->AddMaterial(std::move(material));

    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = 0;
    mesh->Submeshes().push_back(std::move(submesh));
    return mesh;
}

class FakeImageParser : public owe::IImageParser {
public:
    std::shared_ptr<owe::Image> Parse(const std::string&) override { return {}; }
    owe::ImageHeader            ParseHeader(const std::string&) override {
        owe::ImageHeader header;
        header.width  = 64;
        header.height = 32;
        return header;
    }
};

} // namespace

TEST(SceneResourceIndex, ResolvesDrawItemsAndNamedResources) {
    owe::Scene scene;
    scene.sceneGraph->ID()             = 1;
    scene.textures["tex/main"]         = owe::SceneTexture { .url = "tex/main" };
    scene.renderTargets["_rt_default"] = owe::SceneRenderTarget { .width = 1920, .height = 1080 };
    scene.cameras["default"] = std::make_shared<owe::SceneCamera>(1920, 1080, -1.0f, 1.0f);

    auto child      = rstd::sync::Arc<owe::SceneNode>::make();
    child->ID()     = 2;
    auto child_mesh = MakeSingleSubmesh("child-material");
    child->AddMesh(child_mesh);
    scene.sceneGraph->AppendChild(child.clone());

    auto post_node  = rstd::sync::Arc<owe::SceneNode>::make();
    post_node->ID() = 3;
    auto post_mesh  = MakeSingleSubmesh("post-material");
    post_node->AddMesh(post_mesh);

    auto post = std::make_shared<owe::ScenePostProcess>();
    post->steps.push_back(
        owe::ScenePostProcessPass { .node = post_node.clone(), .output = "_rt_post" });
    scene.post_processes.push_back(std::move(post));

    scene.RebuildResourceIndex();
    const auto& index = scene.ResourceIndex();

    auto child_node_id = index.nodeId(*child.as_ptr());
    ASSERT_TRUE(child_node_id.is_some());
    EXPECT_EQ(index.node(*child_node_id), child.as_ptr());

    auto child_draw_id = index.drawItemFor(*child_node_id, 0);
    ASSERT_TRUE(child_draw_id.is_some());

    auto child_draw = index.resolve(*child_draw_id);
    ASSERT_TRUE(child_draw.is_some());
    EXPECT_EQ(child_draw->node, child.as_ptr());
    EXPECT_EQ(child_draw->mesh, child_mesh.get());
    EXPECT_EQ(child_draw->material, child_mesh->MaterialSlots()[0].get());
    EXPECT_EQ(child_draw->submesh, &child_mesh->Submeshes()[0]);

    auto post_node_id = index.nodeId(*post_node.as_ptr());
    ASSERT_TRUE(post_node_id.is_some());
    auto post_draw_id = index.drawItemFor(*post_node_id, 0);
    ASSERT_TRUE(post_draw_id.is_some());
    EXPECT_EQ(index.resolve(*post_draw_id)->material, post_mesh->MaterialSlots()[0].get());

    auto texture_id = index.textureId("tex/main");
    ASSERT_TRUE(texture_id.is_some());
    EXPECT_EQ(index.texture(*texture_id)->url, "tex/main");

    auto rt_id = index.renderTargetId("_rt_default");
    ASSERT_TRUE(rt_id.is_some());
    EXPECT_EQ(index.renderTarget(*rt_id)->width, 1920);
    EXPECT_EQ(index.mutableRenderTarget(*rt_id)->height, 1080);

    auto camera_id = index.cameraId("default");
    ASSERT_TRUE(camera_id.is_some());
    EXPECT_EQ(index.camera(*camera_id), scene.cameras["default"].get());

    owe::Scene other_scene;
    other_scene.RebuildResourceIndex();
    EXPECT_EQ(other_scene.ResourceIndex().node(*child_node_id), nullptr);
    EXPECT_TRUE(other_scene.ResourceIndex().resolve(*child_draw_id).is_none());
}

TEST(SceneResourceIndex, RebuildPicksUpNewRenderTargets) {
    owe::Scene scene;
    scene.RebuildResourceIndex();
    EXPECT_TRUE(scene.ResourceIndex().renderTargetId("_rt_link_7").is_none());

    scene.renderTargets["_rt_link_7"] = owe::SceneRenderTarget { .width = 64, .height = 32 };
    scene.RebuildResourceIndex();

    auto id = scene.ResourceIndex().renderTargetId("_rt_link_7");
    ASSERT_TRUE(id.is_some());
    ASSERT_NE(scene.ResourceIndex().mutableRenderTarget(*id), nullptr);
    EXPECT_EQ(scene.ResourceIndex().mutableRenderTarget(*id)->width, 64);
    EXPECT_EQ(scene.ResourceIndex().renderTarget(*id)->height, 32);
}

TEST(SceneTextures, EnsureTextureDescriptorRegistersImportedTexture) {
    owe::Scene scene;
    EXPECT_FALSE(scene.EnsureTextureDescriptor("tex/runtime"));

    scene.imageParser = std::make_unique<FakeImageParser>();
    EXPECT_TRUE(scene.EnsureTextureDescriptor("tex/runtime"));
    ASSERT_TRUE(scene.textures.contains("tex/runtime"));
    EXPECT_EQ(scene.textures.at("tex/runtime").url, "tex/runtime");
    EXPECT_TRUE(scene.EnsureTextureDescriptor("_rt_default"));
    EXPECT_FALSE(scene.textures.contains("_rt_default"));
}

TEST(SceneUserPropertyDiagnostics, StoresAndClearsByKey) {
    owe::Scene scene;
    scene.AddUserPropertyDiagnostic(owe::SceneUserPropertyDiagnostic {
        .key      = "combo_a",
        .code     = owe::SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue,
        .material = "mat_a",
        .combo    = "USE_A",
        .message  = "bad value",
    });
    scene.AddUserPropertyDiagnostic(owe::SceneUserPropertyDiagnostic {
        .key      = "combo_b",
        .code     = owe::SceneUserPropertyDiagnosticCode::ShaderComboCompileFailed,
        .material = "mat_b",
        .combo    = "USE_B",
        .message  = "compile failed",
    });

    auto diagnostics = scene.UserPropertyDiagnostics();
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(diagnostics[0].key, "combo_a");
    EXPECT_EQ(diagnostics[0].code,
              owe::SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue);

    scene.ClearUserPropertyDiagnostics("combo_a");
    diagnostics = scene.UserPropertyDiagnostics();
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].key, "combo_b");

    scene.ClearUserPropertyDiagnostics({});
    EXPECT_TRUE(scene.UserPropertyDiagnostics().empty());
}

TEST(SceneMaterialRuntimeMutation, UpdatesShaderValuesAndTextureSlotsThroughSceneOwner) {
    owe::Scene scene;
    scene.sceneGraph->ID() = 1;
    scene.imageParser      = std::make_unique<FakeImageParser>();

    auto node  = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID() = 2;
    auto mesh  = MakeSingleSubmesh("material");
    node->AddMesh(mesh);
    scene.sceneGraph->AppendChild(node.clone());

    auto* material = mesh->MaterialSlots()[0].get();
    ASSERT_NE(material, nullptr);

    auto shader = std::make_shared<owe::SceneShader>();
    shader->default_uniforms["u_Color"] =
        owe::ShaderValue(std::array<float, 4> { 0.0f, 0.0f, 0.0f, 0.0f });
    material->customShader.shader = std::move(shader);

    EXPECT_TRUE(scene.SetMaterialShaderValue(*material, "u_Color", owe::ShaderValue(0.5f)));
    auto color_it = material->customShader.constValues.find("u_Color");
    ASSERT_NE(color_it, material->customShader.constValues.end());
    ASSERT_EQ(color_it->second.size(), 4u);
    EXPECT_FLOAT_EQ(color_it->second[0], 0.5f);
    EXPECT_FLOAT_EQ(color_it->second[3], 0.5f);
    EXPECT_TRUE(material->customShader.dirty);

    material->customShader.dirty = false;
    EXPECT_FALSE(scene.SetMaterialShaderValue(*material, "", owe::ShaderValue(1.0f)));
    EXPECT_FALSE(material->customShader.dirty);

    auto mutation = scene.SetMaterialTextureSlot(*material, 0, "tex/runtime");
    EXPECT_TRUE(mutation.changed);
    ASSERT_TRUE(mutation.material.is_some());
    ASSERT_TRUE(scene.textures.contains("tex/runtime"));
    EXPECT_EQ(material->textures[0], "tex/runtime");

    auto unchanged = scene.SetMaterialTextureSlot(*material, 0, "tex/runtime");
    EXPECT_FALSE(unchanged.changed);
    EXPECT_TRUE(unchanged.material.is_none());

    auto spec = scene.SetMaterialTextureSlot(*material, 1, "_rt_default");
    EXPECT_TRUE(spec.changed);
    EXPECT_FALSE(scene.textures.contains("_rt_default"));
    ASSERT_GE(material->textures.size(), 2u);
    EXPECT_EQ(material->textures[1], "_rt_default");
}

TEST(SceneMaterialShaderVariant, CarriesCompileDescriptorThroughMaterialMove) {
    owe::SceneMaterial material;
    material.name = "variant";

    owe::SceneShaderVariantDesc variant;
    variant.scene_id        = "scene";
    variant.shader_name     = "genericimage";
    variant.input_combos    = { { "BLENDMODE", "1" } };
    variant.resolved_combos = { { "BLENDMODE", "1" }, { "TEX0FORMAT", "FORMAT_R8" } };
    variant.uniform_aliases = { { "brightness", "u_Brightness" } };
    variant.default_textures.push_back(
        owe::SceneShaderDefaultTexture { .slot = 0, .texture = "tex/default" });
    variant.texture_infos.push_back(owe::SceneShaderTextureCompileInfo {
        .enabled    = true,
        .components = { true, false, true },
    });
    variant.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/genericimage.vert",
        .source     = "vertex source",
    });
    material.customShader.variant = std::move(variant);

    auto mesh = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(std::move(material));
    auto* moved = mesh->MaterialSlots()[0].get();
    ASSERT_NE(moved, nullptr);
    ASSERT_TRUE(moved->customShader.variant.has_value());

    const auto& stored = *moved->customShader.variant;
    EXPECT_TRUE(stored.Valid());
    EXPECT_EQ(stored.scene_id, "scene");
    EXPECT_EQ(stored.shader_name, "genericimage");
    EXPECT_EQ(stored.resolved_combos.at("TEX0FORMAT"), "FORMAT_R8");
    ASSERT_EQ(stored.default_textures.size(), 1u);
    EXPECT_EQ(stored.default_textures[0].texture, "tex/default");
    ASSERT_EQ(stored.texture_infos.size(), 1u);
    EXPECT_TRUE(stored.texture_infos[0].components[2]);
    ASSERT_EQ(stored.stages.size(), 1u);
    EXPECT_EQ(stored.stages[0].source_key, "/assets/shaders/genericimage.vert");
}

TEST(SceneMaterialShaderVariant, AppliesCompiledVariantThroughSceneOwner) {
    owe::Scene scene;
    scene.sceneGraph->ID() = 1;

    auto node  = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID() = 2;
    auto mesh  = MakeSingleSubmesh("variant");
    node->AddMesh(mesh);
    scene.sceneGraph->AppendChild(node.clone());

    auto* material = mesh->MaterialSlots()[0].get();
    ASSERT_NE(material, nullptr);

    EXPECT_FALSE(scene.SetMaterialShaderVariant(*material, {}).changed);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);

    auto shader  = std::make_shared<owe::SceneShader>();
    shader->name = "variant-next";
    shader->codes.push_back({ 1u, 2u, 3u });

    owe::SceneShaderVariantDesc variant;
    variant.scene_id        = "scene";
    variant.shader_name     = "variant-next";
    variant.resolved_combos = { { "USE_COLOR", "1" } };
    variant.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/variant-next.vert",
        .source     = "source",
    });

    auto mutation = scene.SetMaterialShaderVariant(*material,
                                                   owe::SceneShaderVariantMutation {
                                                       .shader  = shader,
                                                       .variant = variant,
                                                   });

    EXPECT_TRUE(mutation.changed);
    ASSERT_TRUE(mutation.material.is_some());
    EXPECT_EQ(material->customShader.shader, shader);
    ASSERT_TRUE(material->customShader.variant.has_value());
    EXPECT_EQ(material->customShader.variant->resolved_combos.at("USE_COLOR"), "1");
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyGraph);

    auto events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].material.index, mutation.material->index);
    EXPECT_EQ(events[0].flags, owe::SceneMaterialDirtyGraph);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);
}

TEST(SceneMaterialShaderVariant, ClassifiesVariantImpactAndAppliesActiveTextureSlots) {
    owe::Scene scene;
    scene.sceneGraph->ID() = 1;

    auto node  = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID() = 2;
    auto mesh  = MakeSingleSubmesh("variant");
    node->AddMesh(mesh);
    scene.sceneGraph->AppendChild(node.clone());

    auto* material = mesh->MaterialSlots()[0].get();
    ASSERT_NE(material, nullptr);
    material->textures            = { "tex/a", "" };
    material->customShader.shader = std::make_shared<owe::SceneShader>();

    owe::SceneShaderVariantDesc current;
    current.scene_id               = "scene";
    current.shader_name            = "variant";
    current.texture_slots          = { "tex/a", "tex/b" };
    current.resolved_combos        = { { "USE_B", "0" } };
    current.descriptor_layout_hash = 1000u;
    current.stages.push_back(owe::SceneShaderVariantStage {
        .stage                = owe::ShaderType::FRAGMENT,
        .source_key           = "/assets/shaders/variant.frag",
        .source               = "source",
        .active_texture_slots = { 0u },
        .uniforms             = { { "u_Color", "float4" } },
        .code_hash            = 100u,
    });
    material->customShader.variant = current;

    auto hash_only                = current;
    hash_only.stages[0].code_hash = 101u;
    auto hash_shader              = std::make_shared<owe::SceneShader>();
    hash_shader->name             = "variant";
    hash_shader->codes            = { { 101u } };
    auto hash_rt                  = scene.SetMaterialShaderVariant(*material,
                                                                   owe::SceneShaderVariantMutation {
                                                                       .shader  = hash_shader,
                                                                       .variant = hash_only,
                                                                   });

    EXPECT_TRUE(hash_rt.changed);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyPipeline);

    auto hash_events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(hash_events.size(), 1u);
    EXPECT_EQ(hash_events[0].flags, owe::SceneMaterialDirtyPipeline);

    auto layout_only                   = hash_only;
    layout_only.descriptor_layout_hash = 2000u;
    auto layout_shader                 = std::make_shared<owe::SceneShader>();
    layout_shader->name                = "variant";
    layout_shader->codes               = { { 102u } };
    auto layout_rt = scene.SetMaterialShaderVariant(*material,
                                                    owe::SceneShaderVariantMutation {
                                                        .shader  = layout_shader,
                                                        .variant = layout_only,
                                                    });

    EXPECT_TRUE(layout_rt.changed);
    EXPECT_EQ(material->DirtyFlags(),
              owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);

    auto layout_events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(layout_events.size(), 1u);
    EXPECT_EQ(layout_events[0].flags,
              owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);

    auto same_slots                     = layout_only;
    same_slots.resolved_combos["USE_B"] = "2";
    same_slots.stages[0].code_hash      = 103u;
    auto shader                         = std::make_shared<owe::SceneShader>();
    shader->name                        = "variant";
    shader->codes                       = { { 1u } };
    auto pipeline_rt = scene.SetMaterialShaderVariant(*material,
                                                      owe::SceneShaderVariantMutation {
                                                          .shader  = shader,
                                                          .variant = same_slots,
                                                      });

    EXPECT_TRUE(pipeline_rt.changed);
    EXPECT_EQ(material->DirtyFlags(),
              owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);
    ASSERT_EQ(material->textures.size(), 2u);
    EXPECT_EQ(material->textures[0], "tex/a");
    EXPECT_TRUE(material->textures[1].empty());

    auto events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].flags, owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);

    auto graph_slots                           = same_slots;
    graph_slots.resolved_combos["USE_B"]       = "1";
    graph_slots.stages[0].active_texture_slots = { 1u };
    auto graph_shader                          = std::make_shared<owe::SceneShader>();
    graph_shader->name                         = "variant";
    graph_shader->codes                        = { { 2u } };
    auto graph_rt = scene.SetMaterialShaderVariant(*material,
                                                   owe::SceneShaderVariantMutation {
                                                       .shader  = graph_shader,
                                                       .variant = graph_slots,
                                                   });

    EXPECT_TRUE(graph_rt.changed);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyGraph);
    ASSERT_EQ(material->textures.size(), 2u);
    EXPECT_TRUE(material->textures[0].empty());
    EXPECT_EQ(material->textures[1], "tex/b");
}

TEST(SceneVisibility, VisibleRuntimeChangeClearsOnlyVisibilityElideReason) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID()      = 7;
    node->SetVisible(false);
    scene.sceneGraph->AppendChild(node.clone());

    scene.MarkLayerVisibilityElidable(owe::WallpaperLayerId { .value = 7 });
    ASSERT_EQ(scene.elidable_layer_ids.count(7), 1u);

    EXPECT_TRUE(scene.SetNodeVisible(*node.as_ptr(), true));
    EXPECT_TRUE(node->Visible());
    EXPECT_EQ(scene.visibility_elidable_layer_ids.count(7), 0u);
    EXPECT_EQ(scene.elidable_layer_ids.count(7), 0u);

    scene.MarkLayerStaticElidable(owe::WallpaperLayerId { .value = 7 });
    scene.MarkLayerVisibilityElidable(owe::WallpaperLayerId { .value = 7 });
    EXPECT_FALSE(scene.SetNodeVisible(*node.as_ptr(), true));
    EXPECT_EQ(scene.visibility_elidable_layer_ids.count(7), 0u);
    EXPECT_EQ(scene.static_elidable_layer_ids.count(7), 1u);
    EXPECT_EQ(scene.elidable_layer_ids.count(7), 1u);
}

TEST(SceneRenderTargets, EnsureLinkRenderTargetCreatesOwnedDescriptor) {
    owe::Scene scene;
    scene.ortho[0] = 1920;
    scene.ortho[1] = 1080;

    owe::SceneNode sized;
    sized.SetSize({ 64.0f, 32.0f });
    auto key = scene.EnsureLinkRenderTarget(owe::WallpaperLayerId { .value = 7 }, sized);
    EXPECT_EQ(key, "_rt_link_7");
    ASSERT_TRUE(scene.renderTargets.contains(key));
    EXPECT_EQ(scene.renderTargets.at(key).width, 64);
    EXPECT_EQ(scene.renderTargets.at(key).height, 32);

    owe::SceneNode fallback;
    auto           fallback_key =
        scene.EnsureLinkRenderTarget(owe::WallpaperLayerId { .value = 8 }, fallback);
    EXPECT_EQ(scene.renderTargets.at(fallback_key).width, 1920);
    EXPECT_EQ(scene.renderTargets.at(fallback_key).height, 1080);
}

TEST(SceneMaterialTextureDependency, ClassifiesPreparedRefreshCompatibility) {
    EXPECT_EQ(owe::ClassifySceneMaterialTexture(""), owe::SceneMaterialTextureDependency::Empty);
    EXPECT_EQ(owe::ClassifySceneMaterialTexture("tex/main"),
              owe::SceneMaterialTextureDependency::Imported);
    EXPECT_EQ(owe::ClassifySceneMaterialTexture("_rt_default"),
              owe::SceneMaterialTextureDependency::RenderTarget);
    EXPECT_EQ(owe::ClassifySceneMaterialTexture("_rt_link_7"),
              owe::SceneMaterialTextureDependency::LinkRenderTarget);
    EXPECT_EQ(owe::ClassifySceneMaterialTexture("_rt_MipMappedFrameBuffer"),
              owe::SceneMaterialTextureDependency::MipMappedFramebuffer);

    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("", "tex/main"));
    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("tex/a", "tex/b"));
    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("tex/a", ""));
    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("_rt_link_7", "_rt_link_7"));
    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("_rt_default", "_rt_default"));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("tex/a", "_rt_default"));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("_rt_link_7", "tex/a"));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("tex/a", "_rt_link_7"));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("tex/a", "_rt_MipMappedFrameBuffer"));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("_rt_default", "tex/a"));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("tex/a", "_rt_default", "_rt_default"));
}

TEST(RenderSceneSnapshot, ExtractsDescriptorsAndRenderItems) {
    owe::Scene scene;
    scene.sceneGraph->ID()             = 1;
    scene.textures["tex/main"]         = owe::SceneTexture { .url = "tex/main" };
    scene.renderTargets["_rt_default"] = owe::SceneRenderTarget { .width = 1920, .height = 1080 };
    scene.renderTargets["_rt_mask"]    = owe::SceneRenderTarget { .width = 256, .height = 256 };

    auto child                           = rstd::sync::Arc<owe::SceneNode>::make();
    child->ID()                          = 42;
    auto mesh                            = MakeSingleSubmesh("child-material");
    mesh->Submeshes()[0].output_override = "_rt_mask";
    mesh->MaterialSlots()[0]->textures.push_back("_rt_link_7");
    child->AddMesh(mesh);
    scene.sceneGraph->AppendChild(child.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    EXPECT_GT(snapshot.Version().value, 0u);
    ASSERT_EQ(snapshot.RenderItems().size(), 1u);
    ASSERT_EQ(snapshot.TextureDescs().size(), 1u);
    ASSERT_EQ(snapshot.RenderTargetDescs().size(), 2u);

    auto node_id = scene.ResourceIndex().nodeId(*child.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, 0);
    ASSERT_TRUE(draw_id.is_some());

    auto render_item_id = snapshot.renderItemFor(*draw_id);
    ASSERT_TRUE(render_item_id.is_some());
    const auto* render_item = snapshot.renderItem(*render_item_id);
    ASSERT_NE(render_item, nullptr);
    EXPECT_EQ(render_item->scene_draw_item.index, draw_id->index);
    EXPECT_EQ(render_item->scene_node.index, node_id->index);
    EXPECT_EQ(render_item->source_layer.value, 42);
    ASSERT_TRUE(render_item->output_override.is_some());

    const auto* mask_desc = snapshot.renderTargetDesc(*render_item->output_override);
    ASSERT_NE(mask_desc, nullptr);
    EXPECT_EQ(mask_desc->key, "_rt_mask");
    EXPECT_EQ(mask_desc->desc.width, 256);
    EXPECT_EQ(mask_desc->desc.height, 256);

    auto tex_desc_id = snapshot.textureDescId("tex/main");
    ASSERT_TRUE(tex_desc_id.is_some());
    const auto* tex_desc = snapshot.textureDesc(*tex_desc_id);
    ASSERT_NE(tex_desc, nullptr);
    EXPECT_EQ(tex_desc->key, "tex/main");
    EXPECT_EQ(tex_desc->desc.url, "tex/main");

    auto layer_items = snapshot.renderItemsFor(owe::WallpaperLayerId { .value = 42 });
    ASSERT_EQ(layer_items.size(), 1u);
    EXPECT_EQ(layer_items[0].index, render_item_id->index);
    EXPECT_EQ(layer_items[0].generation, render_item_id->generation);

    auto material_items = snapshot.renderItemsFor(render_item->scene_material);
    ASSERT_EQ(material_items.size(), 1u);
    EXPECT_EQ(material_items[0].index, render_item_id->index);
    EXPECT_EQ(material_items[0].generation, render_item_id->generation);

    auto mesh_items = snapshot.renderItemsFor(render_item->scene_mesh);
    ASSERT_EQ(mesh_items.size(), 1u);
    EXPECT_EQ(mesh_items[0].index, render_item_id->index);
    EXPECT_EQ(mesh_items[0].generation, render_item_id->generation);

    EXPECT_TRUE(snapshot.HasLinkConsumer(owe::WallpaperLayerId { .value = 7 }));
    EXPECT_FALSE(snapshot.HasLinkConsumer(owe::WallpaperLayerId { .value = 42 }));
    EXPECT_EQ(snapshot.LinkedLayerIds().count(7), 1u);

    auto rebuilt = owe::ExtractRenderSceneSnapshot(scene);
    EXPECT_GT(rebuilt.Version().value, snapshot.Version().value);
    EXPECT_EQ(rebuilt.renderItem(*render_item_id), nullptr);
}

TEST(RenderSceneSnapshot, PlansLinkRenderTargetForElidableLinkedSource) {
    owe::Scene scene;
    scene.ortho[0]                     = 1920;
    scene.ortho[1]                     = 1080;
    scene.renderTargets["_rt_default"] = owe::SceneRenderTarget { .width = 1920, .height = 1080 };
    scene.sceneGraph->ID()             = 1;
    scene.elidable_layer_ids.insert(7);

    auto source  = rstd::sync::Arc<owe::SceneNode>::make();
    source->ID() = 7;
    source->SetSize({ 64.0f, 32.0f });
    source->AddMesh(MakeSingleSubmesh("source-material"));
    scene.sceneGraph->AppendChild(source.clone());

    auto consumer      = rstd::sync::Arc<owe::SceneNode>::make();
    consumer->ID()     = 42;
    auto consumer_mesh = MakeSingleSubmesh("consumer-material");
    consumer_mesh->MaterialSlots()[0]->textures.push_back("_rt_link_7");
    consumer->AddMesh(consumer_mesh);
    scene.sceneGraph->AppendChild(consumer.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);

    ASSERT_TRUE(scene.renderTargets.contains("_rt_link_7"));
    EXPECT_EQ(scene.renderTargets.at("_rt_link_7").width, 64);
    EXPECT_EQ(scene.renderTargets.at("_rt_link_7").height, 32);

    auto* link_source = snapshot.linkSource(owe::WallpaperLayerId { .value = 7 });
    ASSERT_NE(link_source, nullptr);
    EXPECT_EQ(link_source->render_target_key, "_rt_link_7");

    const auto* link_desc = snapshot.renderTargetDesc(link_source->render_target);
    ASSERT_NE(link_desc, nullptr);
    EXPECT_EQ(link_desc->key, "_rt_link_7");
    EXPECT_EQ(link_desc->desc.width, 64);
    EXPECT_EQ(link_desc->desc.height, 32);
}

TEST(SceneGeometryDataGeneration, IncrementsWhenGeometryDataChanges) {
    std::vector<owe::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position", .type = owe::VertexType::FLOAT3 },
    };
    owe::SceneVertexArray vertices(attrs, 2);
    auto                  vertex_generation = vertices.DataGeneration();

    std::array<float, 6> positions { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    ASSERT_TRUE(vertices.SetVertex("a_Position", positions));
    EXPECT_GT(vertices.DataGeneration(), vertex_generation);

    vertex_generation = vertices.DataGeneration();
    vertices.ResetSize();
    EXPECT_GT(vertices.DataGeneration(), vertex_generation);

    owe::SceneIndexArray    indices(6);
    auto                    index_generation = indices.DataGeneration();
    std::array<uint32_t, 3> tri { 0, 1, 2 };
    indices.Assign(0, tri);
    EXPECT_GT(indices.DataGeneration(), index_generation);
}

TEST(SceneVertexArray, AddVertexAppendsAndMoveKeepsOwnedState) {
    std::vector<owe::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position", .type = owe::VertexType::FLOAT3 },
        { .name = "a_TexCoord", .type = owe::VertexType::FLOAT2 },
    };
    owe::SceneVertexArray vertices(attrs, 2);
    vertices.SetOption("dynamic", true);

    std::array<float, 5> a { 1.0f, 2.0f, 3.0f, 0.25f, 0.5f };
    std::array<float, 5> b { 4.0f, 5.0f, 6.0f, 0.75f, 1.0f };
    ASSERT_TRUE(vertices.AddVertex(a.data()));
    ASSERT_TRUE(vertices.AddVertex(b.data()));

    owe::SceneVertexArray moved(std::move(vertices));
    EXPECT_TRUE(moved.GetOption("dynamic"));
    ASSERT_EQ(moved.VertexCount(), 2u);

    auto       offsets    = moved.GetAttrOffsetMap();
    const auto pos_offset = offsets.at("a_Position").offset / sizeof(float);
    const auto uv_offset  = offsets.at("a_TexCoord").offset / sizeof(float);
    EXPECT_FLOAT_EQ(moved.Data()[pos_offset + 0], 1.0f);
    EXPECT_FLOAT_EQ(moved.Data()[pos_offset + moved.OneSize() + 0], 4.0f);
    EXPECT_FLOAT_EQ(moved.Data()[uv_offset + 0], 0.25f);
    EXPECT_FLOAT_EQ(moved.Data()[uv_offset + moved.OneSize() + 0], 0.75f);

    owe::SceneVertexArray assigned(attrs, 1);
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.GetOption("dynamic"));
    ASSERT_EQ(assigned.VertexCount(), 2u);
    EXPECT_FLOAT_EQ(assigned.Data()[pos_offset + assigned.OneSize() + 1], 5.0f);
}

TEST(SceneNodeFieldAnimation, AlphaAnimationTicksThroughScene) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    node->SetBaseColor({ 1.0f, 1.0f, 1.0f }, 0.3f);

    owe::SceneAnimationCurve curve;
    curve.fps    = 30.0f;
    curve.length = 180;
    curve.mode   = "single";
    curve.c0     = {
        { .frame = 0, .value = 0.0f },
        { .frame = 60, .value = 0.5f },
        { .frame = 100, .value = 0.0f },
    };
    node->SetAlphaAnimation(std::move(curve));
    scene.sceneGraph->AppendChild(node.clone());

    scene.elapsingTime = 0.0;
    scene.TickNodeFieldAnimations();
    EXPECT_TRUE(node->IsAlphaOverridden());
    EXPECT_FLOAT_EQ(node->EffectiveAlpha(), 0.0f);

    scene.elapsingTime = 2.0;
    scene.TickNodeFieldAnimations();
    EXPECT_FLOAT_EQ(node->EffectiveAlpha(), 0.5f);

    scene.elapsingTime = 8.8;
    scene.TickNodeFieldAnimations();
    EXPECT_FLOAT_EQ(node->EffectiveAlpha(), 0.0f);
}

TEST(SceneMeshDirtyEvents, RoutesDataAndLayoutDirtyByOwner) {
    owe::Scene scene;
    scene.sceneGraph->ID() = 1;

    auto static_node  = rstd::sync::Arc<owe::SceneNode>::make();
    static_node->ID() = 2;
    auto static_mesh  = MakeSingleSubmesh("static");
    static_node->AddMesh(static_mesh);
    scene.sceneGraph->AppendChild(static_node.clone());

    auto dynamic_node  = rstd::sync::Arc<owe::SceneNode>::make();
    dynamic_node->ID() = 3;
    auto dynamic_mesh  = std::make_shared<owe::SceneMesh>(true);
    dynamic_mesh->Submeshes().push_back(owe::SceneMesh::Submesh {});
    dynamic_mesh->AddMaterial(owe::SceneMaterial {});
    dynamic_node->AddMesh(dynamic_mesh);
    scene.sceneGraph->AppendChild(dynamic_node.clone());

    scene.RebuildResourceIndex();
    auto static_id = scene.ResourceIndex().meshId(*static_mesh);
    ASSERT_TRUE(static_id.is_some());

    static_mesh->SetDirty();
    dynamic_mesh->SetDirty();
    auto events = scene.ConsumePreparedMeshDirtyEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].mesh.index, static_id->index);
    EXPECT_EQ(events[0].flags, owe::SceneMeshDirtyData);
    EXPECT_EQ(static_mesh->DirtyFlags(), owe::SceneMeshDirtyNone);
    EXPECT_EQ(dynamic_mesh->DirtyFlags(), owe::SceneMeshDirtyData);

    dynamic_mesh->SetLayoutDirty();
    events = scene.ConsumePreparedMeshDirtyEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].flags, owe::SceneMeshDirtyLayout);
    EXPECT_EQ(dynamic_mesh->DirtyFlags(), owe::SceneMeshDirtyNone);
}

TEST(SceneMaterialDirtyEvents, RoutesMaterialDirtyByOwner) {
    owe::Scene scene;
    scene.sceneGraph->ID() = 1;

    auto node  = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID() = 2;
    auto mesh  = MakeSingleSubmesh("material");
    node->AddMesh(mesh);
    scene.sceneGraph->AppendChild(node.clone());

    scene.RebuildResourceIndex();
    auto* material = mesh->MaterialSlots()[0].get();
    ASSERT_NE(material, nullptr);
    auto material_id = scene.ResourceIndex().materialId(*material);
    ASSERT_TRUE(material_id.is_some());

    EXPECT_TRUE(material->SetBlendMode(owe::BlendMode::Normal));
    EXPECT_FALSE(material->SetBlendMode(owe::BlendMode::Normal));
    auto events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].material.index, material_id->index);
    EXPECT_EQ(events[0].flags, owe::SceneMaterialDirtyPipeline);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);

    material->SetResourceDirty();
    material->SetCullMode(owe::CullMode::Back);
    events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].flags, owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);

    material->SetResourceDirty();
    material->SetGraphDirty();
    events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].flags, owe::SceneMaterialDirtyGraph);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);
}
