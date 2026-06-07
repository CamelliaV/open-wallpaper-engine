module;

export module wescene.parse:scene_stages;
import nlohmann.json;

import rstd.cppstd;
import wavsen.audio;
import wescene.core;
import wescene.fs;
import wescene.scene;
import wescene.script;
import wescene.shader_value_updater;
import wescene.types;

import wescene.puppet;

import :wp_image_object;
import :wp_light_object;
import :wp_misc_object;
import :wp_particle_object;
import :wp_scene;
import :wp_sound_object;

export namespace owe
{

using WPObjectVar =
    std::variant<wpscene::WPImageObject, wpscene::WPParticleObject, wpscene::WPSoundObject,
                 wpscene::WPLightObject, wpscene::WPTextObject, wpscene::WPModelObject,
                 wpscene::WPCameraObject>;

// Per-Parse state. Built by BuildContext, mutated by ProcessObjects,
// finalized by FinalizeScene. Holding it as a public struct lets the
// CLI test driver run any subset of the pipeline.
struct ParseContext {
    std::shared_ptr<Scene> scene;
    WPShaderValueUpdater*  shader_updater { nullptr };
    i32                    ortho_w { 0 };
    i32                    ortho_h { 0 };
    fs::VFS*               vfs { nullptr };

    ShaderValueMap             global_base_uniforms;
    std::shared_ptr<SceneNode> effect_camera_node;
    std::shared_ptr<SceneNode> global_camera_node;
    std::shared_ptr<SceneNode> global_perspective_camera_node;

    // Lazily allocated by WireFieldScripts as objects with script
    // bindings come in. Installed onto the Scene by FinalizeScene.
    // Stays null when no object has any script binding.
    std::unique_ptr<owe::script::ScriptScene> script_scene;

    // ID → (parent_id, node) for every parseable object. Filled by each
    // ParseXObj. FinalizeScene re-parents nodes with non-zero parent_id
    // from the scene root onto their actual parent. WE wallpapers use
    // this to position child layers relative to a script-driven parent
    // (e.g. workshop 3327063360's "Audio Bars" hardcoded at (-155, 322)
    // is parent=4995, the "总组件" centre).
    struct NodeRef {
        std::uint32_t              parent_id { 0 };
        std::shared_ptr<SceneNode> node;
        // Carried for cross-node wiring at FinalizeScene attach time.
        // `puppet` populated for image objects that own an MDL skeleton,
        // so a child layer with `attachment = "<name>"` can resolve the
        // matching MDAT entry on its parent's puppet (no second lookup
        // pass needed). Both nullable.
        std::shared_ptr<WPPuppet> puppet;
        std::string               attachment;
    };
    std::unordered_map<std::int32_t, NodeRef> node_id_map;
    // Scene.json declaration order. Reparenting in this order keeps each
    // container's children in the order they appeared in scene.json (so
    // layer 28 stays the first child of layer 79). Iterating the unordered
    // map directly would scramble z-order and let the background overwrite
    // foreground layers.
    std::vector<std::int32_t> node_id_order;

    // Audio-bar fanout clones, keyed by their template layer's id. Held here
    // (not appended to the graph at spawn time) so FinalizeScene can attach
    // them right after the template node — keeping all bars at the template's
    // z-position instead of jumping to the front of the root child list.
    std::unordered_map<std::int32_t, std::vector<std::shared_ptr<SceneNode>>> layer_clones;
};

struct ProcessOpts {
    enum Kind : unsigned
    {
        Image    = 1u << 0,
        Particle = 1u << 1,
        Sound    = 1u << 2,
        Light    = 1u << 3,
        Text     = 1u << 4,
        All      = 0x1Fu,
    };
    unsigned kinds { All };
};

// Walks json["objects"] and instantiates one WPObjectVar per recognised
// kind via FromJson. Pure JSON deserialisation plus per-object VFS
// reads (for image/material refs); no Scene / glslang touched.
// `user_props` (nullable) lets `visible:{user:"<key>"}` resolve to the
// host's current bool, so layers toggled off in the UI are pruned at
// parse time.
std::vector<WPObjectVar>
ExpandObjects(const nlohmann::json&, fs::VFS&, wpscene::SceneVersion,
              const std::unordered_map<std::string, nlohmann::json>* user_props = nullptr);

// If general.orthogonalprojection.auto_, replaces width/height with the
// largest image object's size.
void AdjustAutoOrthoProjection(wpscene::WPScene&, std::span<const WPObjectVar>);

// Allocates Scene + cameras + base uniforms + the two default render
// targets (SpecTex_Default, WE_MIP_MAPPED_FRAME_BUFFER).
ParseContext BuildContext(fs::VFS&, std::string_view scene_id, wpscene::WPScene&);

// Per-object dispatch. Brackets glslang init/finalize around the visit
// loop. opts.kinds masks which kinds run; default is all-kinds. Sound
// dispatch additionally requires sm non-null.
void ProcessObjects(ParseContext&, std::span<WPObjectVar>, wavsen::audio::SoundManager* sm,
                    ProcessOpts opts = {});

// Installs the lazily-built ScriptScene onto the Scene (if any) and
// returns the now-frozen Scene.
std::shared_ptr<Scene> FinalizeScene(ParseContext&);

} // namespace owe
