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

import :wp_image_object;
import :wp_light_object;
import :wp_misc_object;
import :wp_particle_object;
import :wp_scene;
import :wp_sound_object;

export namespace owe
{

using WPObjectVar = std::variant<wpscene::WPImageObject, wpscene::WPParticleObject,
                                 wpscene::WPSoundObject, wpscene::WPLightObject,
                                 wpscene::WPTextObject, wpscene::WPModelObject,
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
};

struct ProcessOpts {
    enum Kind : unsigned {
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
std::vector<WPObjectVar>
ExpandObjects(const nlohmann::json&, fs::VFS&, wpscene::SceneVersion);

// If general.orthogonalprojection.auto_, replaces width/height with the
// largest image object's size.
void AdjustAutoOrthoProjection(wpscene::WPScene&,
                               std::span<const WPObjectVar>);

// Allocates Scene + cameras + base uniforms + the two default render
// targets (SpecTex_Default, WE_MIP_MAPPED_FRAME_BUFFER).
ParseContext BuildContext(fs::VFS&, std::string_view scene_id,
                          wpscene::WPScene&);

// Per-object dispatch. Brackets glslang init/finalize around the visit
// loop. opts.kinds masks which kinds run; default is all-kinds. Sound
// dispatch additionally requires sm non-null.
void ProcessObjects(ParseContext&, std::span<WPObjectVar>,
                    wavsen::audio::SoundManager* sm, ProcessOpts opts = {});

// Installs the lazily-built ScriptScene onto the Scene (if any) and
// returns the now-frozen Scene.
std::shared_ptr<Scene> FinalizeScene(ParseContext&);

} // namespace owe
