export module wescene.vulkan_render:uniform_buffer;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.scene;

using namespace rstd::prelude;

export namespace owe::vulkan
{

struct UniformBufferUpdateError {
    String message;
};

struct UniformSlot {
    String name;
    usize  offset { 0 };
    usize  size { 0 };
    usize  count { 1 };
};

struct UniformBufferLayout {
    usize                       size { 0 };
    rstd::vec::Vec<UniformSlot> slots;
};

auto CompileUniformBufferLayout(const resource::ShaderArtifactUniformBlock&)
    -> Result<UniformBufferLayout, UniformBufferUpdateError>;

struct UniformBufferFrameContext {
    using Trait                  = UniformBufferFrameContext;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformBufferFrameContext;

        auto Frame() const -> ref<SceneFrame> { return rstd::trait_call<0>(this); }
        auto Viewport() const -> rstd::array<f32, 2> { return rstd::trait_call<1>(this); }
        auto TextureFrame(SceneDrawItemId draw, usize texture_index) const
            -> Option<SceneTextureFrameView> {
            return rstd::trait_call<2>(this, draw, texture_index);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Frame, &T::Viewport, &T::TextureFrame>;
};

class ProgramUniformFrameContext {
public:
    ProgramUniformFrameContext(const SceneFrame& frame, rstd::array<f32, 2> viewport,
                               ref<dyn<SceneTextureAnimationView>> textures)
        : m_frame(ref<SceneFrame>::from_raw_parts(rstd::addressof(frame))),
          m_viewport(viewport),
          m_textures(textures) {}

    auto Frame() const -> ref<SceneFrame> { return m_frame; }
    auto Viewport() const -> rstd::array<f32, 2> { return m_viewport; }
    auto TextureFrame(SceneDrawItemId draw, usize texture_index) const
        -> Option<SceneTextureFrameView>;

private:
    ref<SceneFrame>                             m_frame;
    rstd::array<f32, 2>                         m_viewport;
    mutable ref<dyn<SceneTextureAnimationView>> m_textures;
};

struct UniformBufferUpdate {
    using Trait                  = UniformBufferUpdate;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformBufferUpdate;

        auto Update(ref<dyn<UniformBufferFrameContext>>         context,
                    mut_ref<dyn<resource::BufferContentWriter>> buffers) const
            -> Result<empty, UniformBufferUpdateError> {
            return rstd::trait_call<0>(this, context, buffers);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Update>;
};

struct BoundUniformOutput {
    UniformOutputId output;
    usize           slot_index { 0 };
};

struct BoundUniformSource {
    ref<dyn<UniformSource>> source;
    i32                     priority { 0 };
    Vec<BoundUniformOutput> outputs;
    u64                     version { 0 };
    bool                    evaluated { false };
};

struct PreparedUniformTextureMetadata {
    bool                available { false };
    rstd::array<f32, 2> source_extent { 0.0f, 0.0f };
    rstd::array<f32, 2> sample_extent { 0.0f, 0.0f };
    bool                has_mipmap { false };
    f32                 mipmap_level { 0.0f };
    u64                 revision { 1 };
};

struct UniformPrepareDraw {
    SceneDrawItemId    draw_item;
    SceneNodeId        node_id;
    ref<SceneNode>     node;
    ref<SceneMaterial> material;
};

struct UniformBindingPrepareContext {
    using Trait                  = UniformBindingPrepareContext;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformBindingPrepareContext;

        auto ResolveDraw(SceneDrawItemId draw) const -> Option<UniformPrepareDraw> {
            return rstd::trait_call<0>(this, draw);
        }
        auto DrawItemFor(ref<SceneNode> node, u32 submesh_index) const -> Option<SceneDrawItemId> {
            return rstd::trait_call<1>(this, node, submesh_index);
        }
        auto GlobalSources() const -> slice<UniformSourceAttachment> {
            return rstd::trait_call<2>(this);
        }
        auto NodeSources(SceneNodeId node) const -> slice<UniformSourceAttachment> {
            return rstd::trait_call<3>(this, node);
        }
        auto ResolveSource(UniformSourceId source) const -> Option<ref<dyn<UniformSource>>> {
            return rstd::trait_call<4>(this, source);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::ResolveDraw, &T::DrawItemFor, &T::GlobalSources, &T::NodeSources,
                             &T::ResolveSource>;
};

class SceneUniformBindingPrepareContext {
public:
    explicit SceneUniformBindingPrepareContext(Scene& scene)
        : m_scene(ref<Scene>::from_raw_parts(rstd::addressof(scene))) {}

    auto ResolveDraw(SceneDrawItemId) const -> Option<UniformPrepareDraw>;
    auto DrawItemFor(ref<SceneNode>, u32 submesh_index) const -> Option<SceneDrawItemId>;
    auto GlobalSources() const -> slice<UniformSourceAttachment>;
    auto NodeSources(SceneNodeId) const -> slice<UniformSourceAttachment>;
    auto ResolveSource(UniformSourceId) const -> Option<ref<dyn<UniformSource>>>;

private:
    mutable ref<Scene> m_scene;
};

class UniformBufferBinding {
public:
    UniformBufferBinding(SceneDrawItemId, resource::BufferUseHandle, UniformBufferLayout,
                         Vec<BoundUniformSource>, ShaderValues, ref<SceneMaterial>,
                         Vec<PreparedUniformTextureMetadata>);

    auto Update(ref<dyn<UniformBufferFrameContext>>,
                mut_ref<dyn<resource::BufferContentWriter>>) const
        -> Result<empty, UniformBufferUpdateError>;

    bool WriteSlot(usize slot_index, UniformValueView value) const;
    bool WriteName(std::string_view, const UniformValue&) const;

private:
    SceneDrawItemId                     m_draw_item;
    resource::BufferUseHandle           m_buffer;
    UniformBufferLayout                 m_layout;
    mutable Vec<BoundUniformSource>     m_sources;
    mutable rstd::vec::Vec<u8>          m_data;
    mutable rstd::vec::Vec<u8>          m_base_data;
    ShaderValues                        m_defaults;
    ref<SceneMaterial>                  m_material;
    Vec<PreparedUniformTextureMetadata> m_textures;
    mutable u64                         m_content_version { 0 };
    mutable u64                         m_material_version { 0 };
    mutable bool                        m_uploaded { false };
};

auto MakeUniformBufferBinding(ref<dyn<UniformBindingPrepareContext>>, SceneDrawItemId,
                              resource::BufferUseHandle,
                              const resource::ShaderArtifactUniformBlock&,
                              Vec<PreparedUniformTextureMetadata> textures = {})
    -> Result<Box<dyn<UniformBufferUpdate>>, UniformBufferUpdateError>;

} // namespace owe::vulkan
