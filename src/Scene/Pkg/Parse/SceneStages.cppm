module;
#include <rstd/enum.hpp>

export module wescene.pkg.parse:scene_stages;
import eigen;

import rstd;
import rstd.cppstd;
import wavsen.audio;
import wescene.core;
import wescene.json;
import wescene.load_bench;
import wescene.fs;
import wescene.scene;
import wescene.script;
import wescene.text;
import wescene.types;
import wescene.pkg.scene_obj;

import wescene.pkg.puppet;
import :wp_shader_parser;
import :wp_uniform_source;
import :wp_particle_runtime;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::sync::Arc;

export namespace owe
{

class SceneObjectVar {
    RSTD_ENUM(SceneObjectVar, (Image, (wpscene::ImageObject value;)),
              (Shape, (wpscene::ShapeObject value;)), (Particle, (wpscene::ParticleObject value;)),
              (Sound, (wpscene::SoundObject value;)), (Light, (wpscene::LightObject value;)),
              (Text, (wpscene::TextObject value;)), (Model, (wpscene::ModelObject value;)),
              (Camera, (wpscene::CameraObject value;)))
};

struct PuppetLayerRegistry {
    HashMap<SceneNode*, Arc<WPPuppetLayer>> by_node;
    HashMap<SceneNode*, Arc<WPPuppetLayer>> fallback_by_node;
};

struct WPSceneShaderEnvironment {
    bool fog_distance { false };
    bool fog_height { false };
};

class ParseSceneHandle {
public:
    ParseSceneHandle(): m_owner(Some(Box<Scene>::make())), m_scene((*m_owner).get()) {}

    static auto Borrow(Scene& scene) -> ParseSceneHandle { return ParseSceneHandle(&scene); }

    Scene* get() const noexcept { return m_scene; }
    Scene* operator->() const noexcept { return m_scene; }
    Scene& operator*() const noexcept { return *m_scene; }

    auto Take() -> Box<Scene> {
        auto owner = m_owner.take().unwrap_unchecked();
        m_scene    = owner.get();
        return owner;
    }

private:
    explicit ParseSceneHandle(Scene* scene): m_scene(scene) {}

    Option<Box<Scene>> m_owner;
    Scene*             m_scene { nullptr };
};

// Per-Parse state. Built by BuildContext, mutated by ProcessObjects,
// finalized by FinalizeScene. Holding it as a public struct lets the
// CLI test driver run any subset of the pipeline.
struct ParseContext {
    ParseSceneHandle                               scene;
    Option<Arc<WPParticleRuntime>>                 particle_runtime;
    std::int32_t                                   ortho_w { 0 };
    std::int32_t                                   ortho_h { 0 };
    wpscene::SceneVersion                          pkg_version { wpscene::kSceneVersionUnknown };
    fs::VFS*                                       vfs { nullptr };
    Option<ref<rstd::json::Map>>                   user_properties;
    Arc<WPShaderCache>                             shader_cache { Arc<WPShaderCache>::make() };
    HashMap<String, text::FontCache::ResolvedBlob> font_sources;

    ShaderValueMap           global_base_uniforms;
    WPSceneShaderEnvironment shader_environment;
    Option<Arc<SceneNode>>   effect_camera_node;
    Option<Arc<SceneNode>>   global_camera_node;
    Option<Arc<SceneNode>>   global_perspective_camera_node;

    // Lazily allocated by WireFieldScripts as objects with script
    // bindings come in. Installed onto the Scene by FinalizeScene.
    // Stays empty when no object has any script binding.
    Option<Box<owe::script::ScriptScene>> script_scene;
    using ImageAlignmentSetter = Arc<dyn<FnMut<void(SceneNode*, ref<str>)>>>;
    struct ImageAlignmentBinding {
        SceneNode*           node { nullptr };
        String               alignment;
        ImageAlignmentSetter setter;
    };
    Vec<ImageAlignmentBinding> image_alignment_bindings;
    Arc<PuppetLayerRegistry>   puppet_layers { Arc<PuppetLayerRegistry>::make() };
    struct UniformConfigDraft {
        Arc<SceneNode>           node;
        WPUniformNodeConfigDraft config;
    };
    Vec<UniformConfigDraft>  uniform_configs;
    Arc<AudioResponseDemand> audio_response_demand { scene->AudioDemandHandle() };
    Arc<WPUniformSceneState> uniform_state { Arc<WPUniformSceneState>::make(
        audio_response_demand.clone()) };
    struct TextUniformConfigDraft {
        Arc<SceneNode>                                   node;
        std::shared_ptr<text::TextEffectProjectionState> effect_projection;
    };
    Vec<TextUniformConfigDraft> text_uniform_configs;
    struct ParticleTrailUniformConfigDraft {
        Arc<SceneNode>                   node;
        Arc<WPParticleTrailUniformState> uniform_state;
    };
    Vec<ParticleTrailUniformConfigDraft> particle_trail_uniform_configs;

    // ID → (parent_id, node) for every parseable object. Filled by each
    // ParseXObj. FinalizeScene re-parents nodes with non-zero parent_id
    // from the scene root onto their actual parent. WE wallpapers use
    // this to position child layers relative to a script-driven parent
    // (e.g. workshop 3327063360's "Audio Bars" hardcoded at (-155, 322)
    // is parent=4995, the "总组件" centre).
    struct NodeRef {
        std::uint32_t          parent_id { 0 };
        Option<Arc<SceneNode>> node;
        // Carried for cross-node wiring at FinalizeScene attach time.
        // `puppet` populated for image objects that own an MDL skeleton,
        // so a child layer with `attachment = "<name>"` can resolve the
        // matching MDAT entry on its parent's puppet (no second lookup
        // pass needed). Both nullable.
        Option<Arc<WPPuppet>>                          puppet;
        String                                         attachment;
        Option<Arc<WPPuppetLayer>>                     puppet_layer;
        Option<Box<dyn<FnMut<void(Eigen::Vector3f)>>>> apply_attachment_offset;
        Vec<Arc<SceneNode>>                            ordered_before_nodes;
    };
    HashMap<std::int32_t, NodeRef>       node_id_map;
    HashMap<std::int32_t, std::uint32_t> object_parent_ids;
    HashSet<std::int32_t>                solid_layer_ids;
    // Scene.json declaration order. Reparenting in this order keeps each
    // container's children in the order they appeared in scene.json (so
    // layer 28 stays the first child of layer 79). Iterating the unordered
    // map directly would scramble z-order and let the background overwrite
    // foreground layers.
    Vec<std::int32_t>                    node_id_order;
    HashMap<std::int32_t, std::uint64_t> script_initialization_orders;
    HashMap<std::int32_t, Json>          initial_layer_configs;

    // Audio-bar fanout clones, keyed by their template layer's id. Held here
    // (not appended to the graph at spawn time) so FinalizeScene can attach
    // them right after the template node — keeping all bars at the template's
    // z-position instead of jumping to the front of the root child list.
    HashMap<std::int32_t, Vec<Arc<SceneNode>>> layer_clones;
    std::int32_t                               next_dynamic_layer_id { -100000 };
    Vec<owe::script::FieldScript*>             registered_asset_scripts;
    HashMap<String, Arc<SceneNode>>            dynamic_model_prototypes;
    struct DynamicImagePrototype {
        Arc<SceneNode>           node;
        WPUniformNodeConfigDraft uniform_config;
    };
    HashMap<String, DynamicImagePrototype>   dynamic_image_prototypes;
    HashMap<String, wpscene::ParticleObject> dynamic_particle_prototypes;
    wavsen::audio::SoundManager*             sound_manager { nullptr };

    HashMap<std::int32_t, String> system_media_image_fallbacks;
    HashSet<std::int32_t>         hidden_link_source_ids;
    bool                          scene_layer_text_writes { false };
};

struct ProcessOpts {
    enum Kind : unsigned
    {
        Image    = 1u << 0,
        Particle = 1u << 1,
        Sound    = 1u << 2,
        Light    = 1u << 3,
        Text     = 1u << 4,
        Model    = 1u << 5,
        All      = 0x3Fu,
    };
    unsigned kinds { All };
};

// Walks json["objects"] and instantiates one SceneObjectVar per recognised
// kind via FromJson. Pure JSON deserialisation plus per-object VFS
// reads (for image/material refs); no Scene / glslang touched.
// `user_props` (nullable) lets `visible:{user:"<key>"}` resolve to the
// host's current bool, so layers toggled off in the UI are pruned at
// parse time.
Vec<SceneObjectVar> ExpandObjects(const Json&, fs::VFS&, wpscene::SceneVersion,
                                  Option<ref<rstd::json::Map>>       user_props        = None(),
                                  Option<ref<HashSet<std::int32_t>>> linked_source_ids = None());

// Resolves the effective width/height without mutating the parsed metadata.
array<std::int32_t, 2> ResolveOrthoProjectionExtent(const wpscene::SceneMetadata&,
                                                    slice<SceneObjectVar>);

// Allocates Scene + cameras + base uniforms + the two default render
// targets (SpecTex_Default, WE_MIP_MAPPED_FRAME_BUFFER).
ParseContext BuildContext(fs::VFS&, ref<str> scene_id, const wpscene::SceneMetadata&,
                          array<std::int32_t, 2>       ortho_extent,
                          Option<ref<rstd::json::Map>> user_properties  = None(),
                          Option<rstd::path::PathBuf>  shader_cache_dir = None());

// Per-object dispatch. Brackets glslang init/finalize around the visit
// loop. opts.kinds masks which kinds run; default is all-kinds. Sound
// dispatch additionally requires sm non-null.
void ProcessObjects(ParseContext&, mut_ref<SceneObjectVar[]>, wavsen::audio::SoundManager* sm,
                    ProcessOpts opts = {}, SceneLoadBenchRecorderView load_bench = {});

// Installs the lazily-built ScriptScene onto the Scene (if any) and
// returns the now-frozen Scene.
Box<Scene> FinalizeScene(ParseContext&);

} // namespace owe
