module;
#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

export module wescene.vulkan_render:program;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.resource_registry;
import wescene.vulkan;
import wescene.scene;
import wescene.spec_names;
import wescene.rgraph;
import :vulkan_pass;
import :resource;
import :pass_common;
import :pre_pass;
import :fin_pass;
import :shader_reflection_cache;

using namespace rstd::prelude;

export namespace owe::vulkan
{
class DeclaredShaderArtifactProvider {
public:
    explicit DeclaredShaderArtifactProvider(const ResourceDeclarationContext& declarations)
        : m_declarations(rstd::ref<ResourceDeclarationContext>::from_raw_parts(
              rstd::addressof(declarations))) {}

    auto LoadShader(const resource::ShaderRequest& request)
        -> rstd::Result<resource::ShaderArtifact, resource::ResourceError> {
        auto artifact = m_declarations->ShaderArtifact(request);
        if (artifact.is_none()) {
            return rstd::Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingContent,
                .message = rstd::format("shader artifact {} unavailable", request.name),
            });
        }
        return rstd::Ok((**artifact).clone());
    }

private:
    rstd::ref<ResourceDeclarationContext> m_declarations;
};

inline bool SameProgramRenderItemId(owe::RenderItemId lhs, owe::RenderItemId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

struct PreparedPassDiagnostic {
    bool                                      frame_pass { false };
    Option<rg::NodeHandle>                    graph_node;
    std::string                               pass_name;
    Option<rg::PassNode::Type>                pass_type;
    Option<RenderItemId>                      render_item;
    PassInvalidationFlags                     invalidation_flags { PassInvalidationNone };
    std::optional<PipelineCacheKey>           pipeline_cache_key;
    bool                                      pipeline_cache_hit { false };
    u64                                       pipeline_cache_observed_count { 0 };
    std::optional<RenderPassCacheKey>         render_pass_cache_key;
    bool                                      render_pass_cache_hit { false };
    u64                                       render_pass_cache_observed_count { 0 };
    std::optional<FramebufferCacheKey>        framebuffer_cache_key;
    bool                                      framebuffer_cache_hit { false };
    u64                                       framebuffer_cache_observed_count { 0 };
    std::vector<std::string>                  release_textures;
    std::vector<PassTextureRequestDiagnostic> texture_requests;
    bool                                      prepared { false };
};

struct RenderProgram {
    enum class PreparedPassKind
    {
        Graph,
        Frame,
    };

    struct ProgramPassHandle {
        PreparedPassKind kind { PreparedPassKind::Graph };
        rg::PassHandle   graph;
        rstd::usize      frame_index { 0 };
    };

    struct PreparedPassRecord {
        PreparedPassKind                kind { PreparedPassKind::Graph };
        Option<owe::rg::NodeHandle>     graph_node;
        String                          pass_name;
        Option<owe::rg::PassNode::Type> pass_type;
        ProgramPassHandle               pass;
        rstd::vec::Vec<String>          release_textures;
        PassResourceUses                resources;
        PassInvalidationFlags           invalidation_flags { PassInvalidationNone };

        bool invalidated() const { return invalidation_flags != 0; }

        void invalidate(PassInvalidationFlags flags) { invalidation_flags |= flags; }

        void invalidate(PassInvalidation invalidation) {
            invalidate(ToPassInvalidationFlags(invalidation));
        }

        void invalidateResources() { invalidate(PassInvalidation::Resources); }

        void invalidatePipeline() { invalidate(PassInvalidation::Pipeline); }

        void invalidateFramebuffer() { invalidate(PassInvalidation::Framebuffer); }

        void invalidateAll() { invalidate(PassInvalidationAll); }

        void clearInvalidation() { invalidation_flags = PassInvalidationNone; }

        bool needsPrepare(const VulkanPass& resolved) const {
            return ! resolved.prepared() || invalidated();
        }

        void resetPrepared(VulkanPass& resolved, const Device& device) {
            if (resolved.prepared()) {
                resolved.destory(device);
            }
            resolved.resetPrepared();
        }

        void prepareIfNeeded(VulkanPass& resolved, owe::Scene& scene, const Device& device,
                             PassPrepareContext& context) {
            if (invalidated() && resolved.prepared()) {
                resetPrepared(resolved, device);
            }
            if (! resolved.prepared()) {
                resolved.prepare(scene, device, context);
            }
            if (resolved.prepared()) clearInvalidation();
        }
    };

    struct RenderPassScope {
        rstd::Option<rstd::usize>   single;
        rstd::vec::Vec<rstd::usize> scoped_passes;
    };

    rstd::vec::Vec<PreparedPassRecord>                pass_records;
    rstd::vec::Vec<RenderPassScope>                   scopes;
    owe::resource::ResourcePlan                       resource_plan;
    rstd::Option<rstd::mut_ref<owe::rg::RenderGraph>> graph;
    rstd::vec::Vec<rstd::mut_ref<VulkanPass>>         frame_passes;
    rstd::Option<rstd::mut_ref<PrePass>>              frame_prepass;
    rstd::Option<rstd::mut_ref<FinPass>>              frame_finpass;
    bool                                              loaded { false };

    void clear() {
        scopes.clear();
        pass_records.clear();
        resource_plan = {};
        graph         = rstd::None();
        frame_passes.clear();
        frame_prepass = rstd::None();
        frame_finpass = rstd::None();
        loaded        = false;
    }

    auto resolve(const PreparedPassRecord& record) -> rstd::Option<VulkanPass&> {
        if (record.pass.kind == PreparedPassKind::Frame) {
            if (record.pass.frame_index >= frame_passes.len()) return rstd::None();
            return rstd::Some<VulkanPass&>(*frame_passes[record.pass.frame_index]);
        }
        if (graph.is_none()) return rstd::None();
        auto resolved = (*graph)->getPass(record.pass.graph);
        if (resolved.is_none()) return rstd::None();
        return rstd::Some<VulkanPass&>(static_cast<VulkanPass&>(*resolved));
    }

    auto resolve(const PreparedPassRecord& record) const -> rstd::Option<const VulkanPass&> {
        if (record.pass.kind == PreparedPassKind::Frame) {
            if (record.pass.frame_index >= frame_passes.len()) return rstd::None();
            return rstd::Some<const VulkanPass&>(*frame_passes[record.pass.frame_index]);
        }
        if (graph.is_none()) return rstd::None();
        auto resolved =
            static_cast<const owe::rg::RenderGraph&>(**graph).getPass(record.pass.graph);
        if (resolved.is_none()) return rstd::None();
        return rstd::Some<const VulkanPass&>(static_cast<const VulkanPass&>(*resolved));
    }

    void buildFromGraph(owe::rg::RenderGraph& graph) {
        auto nodes             = graph.topologicalOrder();
        auto node_release_texs = graph.getLastReadTextures(nodes.as_slice());
        auto plan              = graph.resourcePlan();

        clear();
        this->graph =
            rstd::Some(rstd::mut_ref<owe::rg::RenderGraph>::from_raw_parts(rstd::addressof(graph)));
        resource_plan = rstd::move(plan);
        pass_records  = rstd::vec::Vec<PreparedPassRecord>::with_capacity(nodes.len() + 2);

        for (rstd::usize i = 0; i < nodes.len(); ++i) {
            auto id    = nodes[i];
            auto state = graph.passState(id);
            rstd_assert(state.is_some());
            if (state.is_none()) continue;
            rstd_assert(graph.getPass(state->pass).is_some());

            PreparedPassRecord record {
                .kind       = PreparedPassKind::Graph,
                .graph_node = Some(id),
                .pass_name  = state->name.clone(),
                .pass_type  = Some(state->type),
                .pass =
                    ProgramPassHandle {
                        .kind  = PreparedPassKind::Graph,
                        .graph = state->pass,
                    },
            };
            for (const auto& tex : node_release_texs[i]) {
                record.release_textures.push(tex.desc.key.clone());
            }
            pass_records.push(rstd::move(record));
        }
    }

    void injectFramePasses(PrePass& prepass, FinPass& finpass) {
        frame_prepass =
            rstd::Some(rstd::mut_ref<PrePass>::from_raw_parts(rstd::addressof(prepass)));
        frame_finpass =
            rstd::Some(rstd::mut_ref<FinPass>::from_raw_parts(rstd::addressof(finpass)));
        frame_passes.clear();
        frame_passes.push(rstd::mut_ref<VulkanPass>::from_raw_parts(rstd::addressof(prepass)));
        frame_passes.push(rstd::mut_ref<VulkanPass>::from_raw_parts(rstd::addressof(finpass)));
        auto combined = rstd::vec::Vec<PreparedPassRecord>::with_capacity(pass_records.len() + 2);
        combined.push(PreparedPassRecord {
            .kind      = PreparedPassKind::Frame,
            .pass_name = String::make("frame/pre"),
            .pass =
                ProgramPassHandle {
                    .kind        = PreparedPassKind::Frame,
                    .frame_index = 0,
                },
        });
        for (auto& record : pass_records) combined.push(rstd::move(record));
        combined.push(PreparedPassRecord {
            .kind      = PreparedPassKind::Frame,
            .pass_name = String::make("frame/fin"),
            .pass =
                ProgramPassHandle {
                    .kind        = PreparedPassKind::Frame,
                    .frame_index = 1,
                },
        });
        pass_records = rstd::move(combined);
    }

    std::vector<PreparedPassDiagnostic> diagnostics() const {
        std::vector<PreparedPassDiagnostic> out;
        out.reserve(pass_records.len());
        for (const auto& record : pass_records) {
            auto pass        = resolve(record);
            auto render_item = Option<RenderItemId> {};
            if (pass.is_some()) {
                auto id = pass->renderItemId();
                if (id.is_some()) render_item = Some<RenderItemId>(*id);
            }
            auto release_textures = std::vector<std::string> {};
            release_textures.reserve(record.release_textures.len());
            for (const auto& texture : record.release_textures) {
                release_textures.push_back(rstd::cppstd::to_string(texture.as_str()));
            }
            out.push_back(PreparedPassDiagnostic {
                .frame_pass                    = record.kind == PreparedPassKind::Frame,
                .graph_node                    = record.graph_node,
                .pass_name                     = rstd::cppstd::to_string(record.pass_name.as_str()),
                .pass_type                     = record.pass_type,
                .render_item                   = render_item,
                .invalidation_flags            = record.invalidation_flags,
                .pipeline_cache_key            = pass ? pass->pipelineCacheKey() : std::nullopt,
                .pipeline_cache_hit            = pass && pass->pipelineCacheHit(),
                .pipeline_cache_observed_count = pass ? pass->pipelineCacheObservedCount() : 0,
                .render_pass_cache_key         = pass ? pass->renderPassCacheKey() : std::nullopt,
                .render_pass_cache_hit         = pass && pass->renderPassCacheHit(),
                .render_pass_cache_observed_count = pass ? pass->renderPassCacheObservedCount() : 0,
                .framebuffer_cache_key = pass ? pass->framebufferCacheKey() : std::nullopt,
                .framebuffer_cache_hit = pass && pass->framebufferCacheHit(),
                .framebuffer_cache_observed_count =
                    pass ? pass->framebufferCacheObservedCount() : 0,
                .release_textures = rstd::move(release_textures),
                .texture_requests = pass ? pass->textureRequestDiagnostics()
                                         : std::vector<PassTextureRequestDiagnostic> {},
                .prepared         = pass && pass->prepared(),
            });
        }
        return out;
    }

    void invalidatePass(ProgramPassHandle pass, PassInvalidationFlags flags) {
        if (flags == PassInvalidationNone) return;
        for (auto& record : pass_records) {
            auto same =
                record.pass.kind == pass.kind &&
                (pass.kind == PreparedPassKind::Frame ? record.pass.frame_index == pass.frame_index
                                                      : record.pass.graph == pass.graph);
            if (! same) continue;
            record.invalidate(flags);
            loaded = false;
            return;
        }
    }

    void invalidateRenderItems(std::span<const owe::RenderItemId> render_items,
                               PassInvalidationFlags              flags) {
        if (flags == PassInvalidationNone || render_items.empty()) return;
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            auto pass_render_item = pass->renderItemId();
            if (pass_render_item.is_none()) continue;
            auto matched = std::any_of(render_items.begin(), render_items.end(), [&](auto id) {
                return SameProgramRenderItemId(*pass_render_item, id);
            });
            if (! matched) continue;
            record.invalidate(flags);
            loaded = false;
        }
    }

    bool refreshMaterialTextureBindings(const owe::RenderSceneSnapshot&    render_scene,
                                        std::span<const owe::RenderItemId> render_items) {
        if (render_items.empty()) return false;

        bool requires_graph_rebuild = false;
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            auto pass_render_item = pass->renderItemId();
            if (pass_render_item.is_none()) continue;
            auto matched = std::any_of(render_items.begin(), render_items.end(), [&](auto id) {
                return SameProgramRenderItemId(*pass_render_item, id);
            });
            if (! matched) continue;

            auto refresh = pass->refreshMaterialTextureBindings(render_scene);
            if (refresh.requires_graph_rebuild) {
                requires_graph_rebuild = true;
            }
            if (refresh.invalidation_flags == PassInvalidationNone) continue;
            record.invalidate(refresh.invalidation_flags);
            loaded = false;
        }
        return requires_graph_rebuild;
    }

    void finalizeRenderTargetSizes(owe::Scene& scene, VkExtent2D extent,
                                   VkSampleCountFlagBits msaa_samples) {
        for (auto& item : scene.renderTargets) {
            auto& rt = item.second;
            if (rt.bind.enable && rt.bind.screen) {
                rt.width  = static_cast<owe::i32>(rt.bind.scale * extent.width);
                rt.height = static_cast<owe::i32>(rt.bind.scale * extent.height);
            }
        }
        for (auto& item : scene.renderTargets) {
            auto& rt = item.second;
            if (rt.bind.screen || ! rt.bind.enable) continue;
            auto bind_rt = scene.renderTargets.find(rt.bind.name);
            if (rt.bind.name.empty() || bind_rt == scene.renderTargets.end()) {
                rstd_error("unknonw render target bind: {}", rt.bind.name);
                continue;
            }
            rt.width  = static_cast<owe::i32>(rt.bind.scale * bind_rt->second.width);
            rt.height = static_cast<owe::i32>(rt.bind.scale * bind_rt->second.height);
        }
        for (auto& item : scene.renderTargets) {
            auto& rt = item.second;
            if (! item.first.empty() && (rt.width <= 0 || rt.height <= 0)) {
                rstd_error("wrong size for render target: {}", item.first);
            } else if (rt.has_mipmap) {
                rt.mipmap_level = std::max(3u,
                                           static_cast<unsigned>(std::floor(
                                               std::log2(std::min(rt.width, rt.height))))) -
                                  2u;
            }
        }
        if (msaa_samples != VK_SAMPLE_COUNT_1_BIT) {
            auto it = scene.renderTargets.find(std::string(owe::SpecTex_Default));
            if (it != scene.renderTargets.end()) {
                it->second.sample_count = static_cast<unsigned>(msaa_samples);
            }
        }
        scene.shaderValueUpdater->SetScreenSize(static_cast<owe::i32>(extent.width),
                                                static_cast<owe::i32>(extent.height));
    }

    void finalizeFramePassRequests(owe::Scene& scene) {
        if (frame_prepass.is_none() || frame_finpass.is_none()) return;

        auto& prepass        = **frame_prepass;
        auto& finpass        = **frame_finpass;
        auto  prepass_handle = ProgramPassHandle {
            .kind        = PreparedPassKind::Frame,
            .frame_index = 0,
        };
        auto finpass_handle = ProgramPassHandle {
            .kind        = PreparedPassKind::Frame,
            .frame_index = 1,
        };

        const std::string key(owe::SpecTex_Default);
        auto              it = scene.renderTargets.find(key);
        if (it == scene.renderTargets.end()) {
            if (prepass.setResultRequest(rstd::None())) {
                invalidatePass(prepass_handle,
                               ToPassInvalidationFlags(PassInvalidation::Resources) |
                                   ToPassInvalidationFlags(PassInvalidation::Framebuffer));
            }
            if (finpass.setResultRequest(rstd::None())) {
                invalidatePass(finpass_handle,
                               ToPassInvalidationFlags(PassInvalidation::Resources));
            }
            return;
        }

        auto&                        rt = it->second;
        rstd::Option<TextureRequest> msaa_request;
        auto                         samples = TextureSampleCount(rt.sample_count);
        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            auto twin_name = MsaaTwinName(key, samples);
            msaa_request   = rstd::Some(MakeMsaaTextureRequest(twin_name, rt, samples));
        }

        if (prepass.setResultRequest(rstd::Some(MakeRenderTargetNoMipTextureRequest(key, rt)),
                                     std::move(msaa_request))) {
            invalidatePass(prepass_handle,
                           ToPassInvalidationFlags(PassInvalidation::Resources) |
                               ToPassInvalidationFlags(PassInvalidation::Framebuffer));
        }
        if (finpass.setResultRequest(rstd::Some(MakeRenderTargetTextureRequest(key, rt)))) {
            invalidatePass(finpass_handle, ToPassInvalidationFlags(PassInvalidation::Resources));
        }
    }

    void destroyPasses(const Device& device) {
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass) record.resetPrepared(*pass, device);
        }
    }

    void finalizeResourceRequests(owe::Scene& scene) {
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            auto flags = pass->finalizeResourceRequests(scene);
            if (flags == PassInvalidationNone) continue;
            record.invalidate(flags);
            loaded = false;
        }
    }

    bool prepare(owe::Scene& scene, const Device& device, RenderingResources& rr,
                 const owe::RenderSceneSnapshot& render_scene) {
        loaded = false;
        resource_plan.buffers.clear();
        resource_plan.shaders.clear();
        if (rr.shader_reflection_cache.is_none()) {
            rstd_error("shader artifact compiler unavailable");
            return false;
        }
        ResourceDeclarationContext declarations(resource_plan, **rr.shader_reflection_cache);
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass) pass->declareResources(declarations);
        }

        SnapshotImportedTextureProvider imported_textures(render_scene, scene.imageParser.get());
        auto                            content =
            rstd::dyn<owe::resource::TextureContentProvider>::from_ref(imported_textures);
        auto buffer_content =
            rstd::dyn<owe::resource::BufferContentProvider>::from_ref(declarations);
        DeclaredShaderArtifactProvider declared_shaders(declarations);
        auto                           shader_artifacts =
            rstd::dyn<owe::resource::ShaderArtifactProvider>::from_ref(declared_shaders);
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass && pass->prepared()) record.resetPrepared(*pass, device);
        }
        auto prepared =
            rr.resources.PreparePlan(resource_plan,
                                     owe::resource_registry::ResourceContentProviders {
                                         .texture = rstd::Some(content),
                                         .buffer  = rstd::Some(buffer_content.as_mut_ref()),
                                         .shader  = rstd::Some(shader_artifacts.as_mut_ref()),
                                     });
        if (prepared.is_err()) {
            auto error = rstd::move(prepared).unwrap_err_unchecked();
            rstd_error("prepare resource plan failed: {}", error.message);
            return false;
        }
        if (! rr.resources.PreparePendingUploads()) {
            rstd_error("prepare buffer upload backing failed");
            return false;
        }
        rstd::Option<owe::resource::TextureUseHandle> frame_result_use = rstd::None();
        rstd::Option<owe::resource::TextureUseHandle> frame_msaa_use   = rstd::None();
        std::string                                   frame_msaa_name;
        auto frame_target = scene.renderTargets.find(std::string(owe::SpecTex_Default));
        if (frame_target != scene.renderTargets.end()) {
            auto samples = TextureSampleCount(frame_target->second.sample_count);
            if (samples != VK_SAMPLE_COUNT_1_BIT) {
                frame_msaa_name = MsaaTwinName(owe::SpecTex_Default, samples);
            }
        }
        for (const auto& entry : resource_plan.textures) {
            auto name = rstd::cppstd::as_string_view(entry.request.name.as_str());
            auto use  = owe::resource::TextureUseHandle {
                .index      = entry.handle.index,
                .generation = entry.handle.generation,
            };
            if (name == owe::SpecTex_Default) {
                frame_result_use = rstd::Some(use);
            } else if (! frame_msaa_name.empty() && name == frame_msaa_name) {
                frame_msaa_use = rstd::Some(use);
            }
        }
        if (frame_prepass) {
            (*frame_prepass)->setResultUse(frame_result_use);
            (*frame_prepass)->setResultMsaaUse(frame_msaa_use);
        }
        if (frame_finpass) (*frame_finpass)->setResultUse(frame_result_use);
        auto state_preparer = rstd::dyn<owe::resource_registry::TextureStatePreparer>::from_ref(
            rr.resources.States());
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            if (! pass->prepareResourceStates(state_preparer.as_mut_ref())) {
                rstd_error("prepare resource states failed for {}", record.pass_name);
                return false;
            }
        }
        auto graphics =
            rstd::dyn<owe::resource_registry::GraphicsResourcePreparer>::from_ref(rr.resources);
        PassPrepareContext prepare_context {
            .resources = rstd::ref<owe::resource_registry::PreparedResourceTable>::from_raw_parts(
                rstd::addressof(rr.resources.Prepared())),
            .graphics = graphics.as_mut_ref(),
        };
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass) {
                record.prepareIfNeeded(*pass, scene, device, prepare_context);
                if (! pass->prepared()) {
                    rstd_error("prepare pass failed for {}", record.pass_name);
                    return false;
                }
                record.resources = pass->resourceUses();
            }
        }
        return true;
    }

    void invalidateAllPreparedPasses() {
        for (auto& record : pass_records) record.invalidateAll();
        loaded = false;
    }

    u64 commitUploads(const Device& device, RenderingResources& rr,
                      vvk::CommandBuffer& upload_cmd) {
        VVK_CHECK_ACT(return 0,
                             upload_cmd.Begin(VkCommandBufferBeginInfo {
                                 .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 .pNext = nullptr,
                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                             }));
        if (! rr.resources.RecordPendingUploads(upload_cmd)) return 0;
        VVK_CHECK_ACT(return 0, upload_cmd.End());
        {
            auto                          ready        = rr.resources.ReserveUpload();
            const u64                     signal_value = ready.value;
            VkTimelineSemaphoreSubmitInfo timeline_info {
                .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .pNext                     = nullptr,
                .waitSemaphoreValueCount   = 0,
                .pWaitSemaphoreValues      = nullptr,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues    = &signal_value,
            };
            VkSubmitInfo sub_info {
                .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext                = &timeline_info,
                .commandBufferCount   = 1,
                .pCommandBuffers      = upload_cmd.address(),
                .signalSemaphoreCount = 1,
                .pSignalSemaphores    = rr.sem_upload.address(),
            };
            VVK_CHECK_ACT(return 0, device.graphics_queue().handle.Submit(sub_info, {}));
            if (! rr.resources.MarkUploadSubmitted(ready)) return 0;
            loaded = true;
            return signal_value;
        }
        return 0;
    }

    void rebuildScopes() {
        scopes.clear();
        auto pending_scope_passes = rstd::vec::Vec<rstd::usize>::make();

        auto flushScopePasses = [&]() {
            if (pending_scope_passes.is_empty()) return;
            RenderPassScope scope;
            scope.scoped_passes = rstd::move(pending_scope_passes);
            scopes.push(rstd::move(scope));
            pending_scope_passes = rstd::vec::Vec<rstd::usize>::make();
        };

        for (rstd::usize index = 0; index < pass_records.len(); ++index) {
            auto& record = pass_records[index];
            auto  pass   = resolve(record);
            if (pass.is_none()) continue;
            if (pass->supportsRenderScope()) {
                bool can_join = false;
                if (! pending_scope_passes.is_empty()) {
                    auto previous =
                        resolve(pass_records[pending_scope_passes[pending_scope_passes.len() - 1]]);
                    can_join = previous && pass->canJoinRenderScopeAfter(*previous);
                }
                if (can_join) {
                    pending_scope_passes.push(rstd::usize(index));
                } else {
                    flushScopePasses();
                    pending_scope_passes.push(rstd::usize(index));
                }
                continue;
            }

            flushScopePasses();
            scopes.push(RenderPassScope { .single = rstd::Some(index) });
        }

        flushScopePasses();
    }

    template<typename Callback>
    void withRecordContext(rstd::usize index, RenderingResources& rr, Callback&& callback) {
        if (index >= pass_records.len()) return;
        auto pass = resolve(pass_records[index]);
        if (pass.is_none()) return;

        PreparedPassResources resources(rr.resources.Prepared(), pass_records[index].resources);
        PassRecordContext     context {
            .command =
                rstd::mut_ref<vvk::CommandBuffer>::from_raw_parts(rstd::addressof(rr.command)),
            .resources =
                rstd::ref<PreparedPassResources>::from_raw_parts(rstd::addressof(resources)),
        };
        callback(*pass, context);
    }

    void execute(RenderingResources& rr) {
        auto buffer_writer = rstd::dyn<resource::BufferContentWriter>::from_ref(rr.resources);
        PassUpdateContext update_context {
            .buffers = buffer_writer.as_mut_ref(),
        };
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass && pass->prepared() && ! pass->update(update_context)) {
                rstd_error("update pass resources failed: {}", record.pass_name);
                return;
            }
        }
        if (! rr.resources.RecordPendingUploads(rr.command)) {
            rstd_error("record dynamic buffer uploads failed");
            return;
        }

        for (auto& scope : scopes) {
            if (scope.single) {
                auto index = *scope.single;
                auto pass  = resolve(pass_records[index]);
                if (pass && pass->prepared()) {
                    withRecordContext(
                        index, rr, [](VulkanPass& target, PassRecordContext& context) {
                            target.record(context);
                        });
                }
                continue;
            }

            auto& scoped_passes = scope.scoped_passes;
            if (scoped_passes.is_empty()) continue;
            if (scoped_passes.len() == 1) {
                auto pass = resolve(pass_records[scoped_passes[0]]);
                if (pass && pass->prepared()) {
                    withRecordContext(
                        scoped_passes[0], rr, [](VulkanPass& target, PassRecordContext& context) {
                            target.record(context);
                        });
                }
                continue;
            }

            if (! std::all_of(scoped_passes.begin(), scoped_passes.end(), [&](auto index) {
                    auto pass = resolve(pass_records[index]);
                    return pass && pass->prepared();
                })) {
                continue;
            }

            for (auto index : scoped_passes) {
                withRecordContext(index, rr, [](VulkanPass& target, PassRecordContext& context) {
                    target.prepareRenderScopeDraw(context);
                });
            }
            withRecordContext(
                scoped_passes[0], rr, [](VulkanPass& target, PassRecordContext& context) {
                    target.beginRenderScope(context);
                });
            for (auto index : scoped_passes) {
                withRecordContext(index, rr, [](VulkanPass& target, PassRecordContext& context) {
                    target.recordRenderScopeDraw(context);
                });
            }
            withRecordContext(
                scoped_passes[0], rr, [](VulkanPass& target, PassRecordContext& context) {
                    target.endRenderScope(context);
                });
        }
    }
};

} // namespace owe::vulkan
