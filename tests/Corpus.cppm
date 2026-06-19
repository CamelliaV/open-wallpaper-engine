// Test corpus index.
//
// Walks workshop/* once, dumps every entry that has a scene.pkg via
// DumpWorkshop, and exposes lookup-by-version slices that the gtest
// fixtures parameterise on. The corpus is built lazily on first access
// (Meyer's singleton) so it's safe to call from INSTANTIATE_TEST_SUITE_P
// at static-init time.
//
// Skipped workshops (e.g. ones that hang WPMdlParser::Parse) are listed
// in kSkipIds and never parsed.

module;

#include <cstdio>

#include <nlohmann/json.hpp>

export module wescene.testing.corpus;

import rstd.cppstd;
import wescene.pkg.parse;
import wescene.pkg_fs;
import wescene.fs;
import wescene.types;
import wescene.testing.pkg_header;

export namespace owe::testing
{

struct WorkshopEntry {
    std::string    id;
    std::string    dir;
    nlohmann::json snapshot;
};

class Corpus {
public:
    // Returns the singleton, building it on first access.
    static const Corpus& instance();

    // All entries successfully dumped.
    const std::vector<WorkshopEntry>& entries() const { return entries_; }

    // Sorted unique sets of every version stamp observed across the corpus.
    // These drive INSTANTIATE_TEST_SUITE_P value lists.
    const std::set<std::string>& pkg_versions() const { return pkg_versions_; }
    const std::set<int>&         texv_versions() const { return texv_versions_; }
    const std::set<int>&         texi_versions() const { return texi_versions_; }
    const std::set<int>&         texb_versions() const { return texb_versions_; }
    const std::set<int>&         texs_versions() const { return texs_versions_; }
    const std::set<int>&         tex_formats() const { return tex_formats_; }
    const std::set<int>&         mdlv_versions() const { return mdlv_versions_; }
    const std::set<int>&         mdls_versions() const { return mdls_versions_; }
    const std::set<int>&         mdla_versions() const { return mdla_versions_; }

    // Slice accessors.
    struct PkgRef {
        const WorkshopEntry* workshop;
    };
    struct TexRef {
        const WorkshopEntry*  workshop;
        const nlohmann::json* tex;
    };
    struct MdlRef {
        const WorkshopEntry*  workshop;
        const nlohmann::json* mdl;
    };

    std::vector<PkgRef> workshops_with_pkg(const std::string& pkgv) const;
    std::vector<TexRef> textures_with_texv(int v) const;
    std::vector<TexRef> textures_with_texi(int v) const;
    std::vector<TexRef> textures_with_texb(int v) const;
    std::vector<TexRef> textures_with_texs(int v) const;
    std::vector<TexRef> textures_with_format(int v) const;
    std::vector<MdlRef> mdls_with_mdlv(int v) const;
    std::vector<MdlRef> mdls_with_mdls(int v) const;
    std::vector<MdlRef> mdls_with_mdla(int v) const;

private:
    Corpus();
    void build();

    std::vector<WorkshopEntry> entries_;
    std::set<std::string>      pkg_versions_;
    std::set<int>              texv_versions_;
    std::set<int>              texi_versions_;
    std::set<int>              texb_versions_;
    std::set<int>              texs_versions_;
    std::set<int>              tex_formats_;
    std::set<int>              mdlv_versions_;
    std::set<int>              mdls_versions_;
    std::set<int>              mdla_versions_;
};

// Gates which sections DumpWorkshop emits. Defaults preserve the historic
// "do everything except shader compile" behavior so Corpus/version_tests
// fixtures don't shift.
struct DumpFlags {
    bool tex { true };      // emit "textures" array (ReadTexMeta)
    bool shader { false };  // emit "shaders" array (CompileMaterialShader)
    bool mdl { true };      // emit "puppets" array
    bool mdl_full { true }; // puppets entries via full WPMdlParser::Parse;
                            // false ⇒ just the WPMdlHeader fields
};

// Per-workshop JSON snapshot used by `wescene-test valid`, by
// `wescene-test scan --json-dir`, and by Corpus to index versions.
// On failure returns a json object with `{"error": "..."}` and `err`
// is set to the same message.
nlohmann::json DumpWorkshop(const std::string& workshop_dir, std::string& err,
                            DumpFlags flags = {});

} // namespace owe::testing

namespace owe::testing
{

namespace
{

namespace fs = std::filesystem;
using json   = nlohmann::json;

// Workshops that hang or crash the dumper.
const std::set<std::string> kSkipIds {
    "2435537849",
    "3346715292",
};

// Workshop root: resolved at build time via the WAYWALLEN_WORKSHOP_DIR
// macro defined in tests/CMakeLists.txt. Falls back to "workshop" so the
// binary stays runnable from the source root for ad-hoc dev loops.
constexpr const char* kWorkshopDirMacro =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

constexpr const char* kAssetsDirMacro =
#ifdef WAYWALLEN_ASSETS_DIR
    WAYWALLEN_ASSETS_DIR
#else
    ""
#endif
    ;

struct TexMeta {
    std::string path;
    int32_t     texv { 0 };
    int32_t     texi { 0 };
    int32_t     texb { 0 };
    int32_t     texs { 0 };
    int32_t     compo1 { 0 };
    int32_t     compo2 { 0 };
    int32_t     compo3 { 0 };
    int32_t     format { 0 };
    int32_t     image_type { 0 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    int32_t     map_width { 0 };
    int32_t     map_height { 0 };
    int32_t     count { 0 };
    bool        is_sprite { false };
    int64_t     sprite_frames { 0 };
    bool        mipmap_pow2 { false };
    bool        mipmap_larger { false };
    int         wrap_s { 0 };
    int         wrap_t { 0 };
    int         min_filter { 0 };
    int         mag_filter { 0 };
    bool        ok { false };
};

TexMeta ReadTexMeta(owe::fs::VFS& vfs, const std::string& pkg_path) {
    TexMeta meta;
    meta.path = pkg_path;

    constexpr std::string_view prefix = "/materials/";
    constexpr std::string_view suffix = ".tex";
    if (pkg_path.compare(0, prefix.size(), prefix) != 0) return meta;
    if (pkg_path.size() < prefix.size() + suffix.size()) return meta;
    if (pkg_path.compare(pkg_path.size() - suffix.size(), suffix.size(), suffix) != 0) return meta;
    std::string name =
        pkg_path.substr(prefix.size(), pkg_path.size() - prefix.size() - suffix.size());

    owe::WPTexImageParser parser(&vfs);
    owe::ImageHeader      h;
    try {
        h = parser.ParseHeader(name);
    } catch (const std::exception&) {
        return meta;
    }

    auto extra_val = [&](const std::string& k) -> int32_t {
        auto it = h.extraHeader.find(k);
        return it == h.extraHeader.end() ? 0 : it->second.val;
    };
    meta.texv          = extra_val("texv");
    meta.texi          = extra_val("texi");
    meta.texb          = extra_val("texb");
    meta.texs          = extra_val("texs");
    meta.compo1        = extra_val("compo1");
    meta.compo2        = extra_val("compo2");
    meta.compo3        = extra_val("compo3");
    meta.format        = static_cast<int32_t>(h.format);
    meta.image_type    = static_cast<int32_t>(h.type);
    meta.width         = h.width;
    meta.height        = h.height;
    meta.map_width     = h.mapWidth;
    meta.map_height    = h.mapHeight;
    meta.count         = h.count;
    meta.is_sprite     = h.isSprite;
    meta.sprite_frames = static_cast<int64_t>(h.spriteAnim.numFrames());
    meta.mipmap_pow2   = h.mipmap_pow2;
    meta.mipmap_larger = h.mipmap_larger;
    meta.wrap_s        = static_cast<int>(h.sample.wrapS);
    meta.wrap_t        = static_cast<int>(h.sample.wrapT);
    meta.min_filter    = static_cast<int>(h.sample.minFilter);
    meta.mag_filter    = static_cast<int>(h.sample.magFilter);
    meta.ok            = (meta.texv > 0 && meta.width > 0 && meta.height > 0);
    return meta;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void sort_by_path(json& arr) {
    std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
        return a.value("path", std::string {}) < b.value("path", std::string {});
    });
}

template<typename Map>
json map_to_json(const Map& m) {
    json o = json::object();
    for (const auto& [k, v] : m) o[k] = v;
    return o;
}

json dump_material(const owe::wpscene::Material& m) {
    return {
        { "shader", m.shader },
        { "blending", m.blending },
        { "cullmode", m.cullmode },
        { "depthtest", m.depthtest },
        { "depthwrite", m.depthwrite },
        { "use_puppet", m.use_puppet },
        { "textures", m.textures },
        { "combos", map_to_json(m.combos) },
        { "constantshadervalues", map_to_json(m.constantshadervalues) },
    };
}

json dump_material_pass(const owe::wpscene::MaterialPass& p) {
    json bind = json::array();
    for (const auto& b : p.bind) {
        bind.push_back({ { "name", b.name }, { "index", b.index } });
    }
    return {
        { "target", p.target },
        { "textures", p.textures },
        { "combos", map_to_json(p.combos) },
        { "constantshadervalues", map_to_json(p.constantshadervalues) },
        { "bind", bind },
    };
}

json dump_effect_fbo(const owe::wpscene::EffectFbo& f) {
    return {
        { "name", f.name },
        { "format", f.format },
        { "scale", f.scale },
    };
}

// Field types in scene.json are inconsistent (origin can be either an
// array of floats or a "x y z" string), so we copy the raw value through
// instead of forcing a particular C++ type.
json dump_object_common(const json& obj) {
    json o;
    o["id"]   = obj.value("id", -1);
    o["name"] = obj.value("name", std::string {});
    // `visible` is sometimes a {script, value} object (scripted property);
    // json::value<bool> would throw type_error on that shape and tear down
    // the entire scene dump. Unwrap when present, default to true.
    if (obj.contains("visible")) {
        const auto& v = obj.at("visible");
        if (v.is_boolean())
            o["visible"] = v.get<bool>();
        else if (v.is_object() && v.contains("value") && v.at("value").is_boolean())
            o["visible"] = v.at("value").get<bool>();
        else
            o["visible"] = true;
    } else {
        o["visible"] = true;
    }
    for (const char* key : { "origin", "scale", "angles", "size", "parallaxDepth", "alignment" }) {
        if (obj.contains(key)) o[key] = obj[key];
    }
    return o;
}

json dump_light_object(const json& obj, owe::fs::VFS& vfs) {
    json out    = dump_object_common(obj);
    out["kind"] = "light";
    owe::wpscene::LightObject lo;
    bool                      ok = false;
    try {
        ok = lo.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    out["parsed"] = ok;
    if (! ok) return out;
    out["light"]          = lo.light;
    out["color"]          = lo.color;
    out["intensity"]      = lo.intensity;
    out["radius"]         = lo.radius;
    out["origin_parsed"]  = lo.origin;
    out["scale_parsed"]   = lo.scale;
    out["angles_parsed"]  = lo.angles;
    out["visible_parsed"] = lo.visible;
    return out;
}

json dump_particle_object(const json& obj, owe::fs::VFS& vfs) {
    json out    = dump_object_common(obj);
    out["kind"] = "particle";
    owe::wpscene::ParticleObject po;
    bool                         ok = false;
    try {
        ok = po.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    out["parsed"] = ok;
    if (! ok) return out;
    out["particle"]           = po.particle;
    out["origin_parsed"]      = po.origin;
    out["scale_parsed"]       = po.scale;
    out["angles_parsed"]      = po.angles;
    out["visible_parsed"]     = po.visible;
    out["emitter_count"]      = static_cast<int>(po.particleObj.emitters.size());
    out["initializer_count"]  = static_cast<int>(po.particleObj.initializers.size());
    out["operator_count"]     = static_cast<int>(po.particleObj.operators.size());
    out["renderer_count"]     = static_cast<int>(po.particleObj.renderers.size());
    out["controlpoint_count"] = static_cast<int>(po.particleObj.controlpoints.size());
    out["child_count"]        = static_cast<int>(po.particleObj.children.size());
    out["maxcount"]           = static_cast<int>(po.particleObj.maxcount);
    out["starttime"]          = static_cast<int>(po.particleObj.starttime);
    out["animationmode"]      = po.particleObj.animationmode;
    return out;
}

json dump_sound_object(const json& obj, owe::fs::VFS& vfs) {
    json out    = dump_object_common(obj);
    out["kind"] = "sound";
    owe::wpscene::SoundObject so;
    bool                      ok = false;
    try {
        ok = so.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    out["parsed"] = ok;
    if (! ok) return out;
    out["playbackmode"]   = so.playbackmode;
    out["volume"]         = so.volume;
    out["mintime"]        = so.mintime;
    out["maxtime"]        = so.maxtime;
    out["visible_parsed"] = so.visible;
    out["sound_paths"]    = so.sound;
    return out;
}

json dump_image_object(const json& obj, owe::fs::VFS& vfs) {
    json out    = dump_object_common(obj);
    out["kind"] = "image";
    owe::wpscene::ImageObject img;
    bool                      ok = false;
    try {
        ok = img.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    out["parsed"] = ok;
    if (! ok) return out;
    out["image"]            = img.image;
    out["color"]            = img.color;
    out["colorBlendMode"]   = img.colorBlendMode;
    out["alpha"]            = img.alpha;
    out["brightness"]       = img.brightness;
    out["fullscreen"]       = img.fullscreen;
    out["nopadding"]        = img.nopadding;
    out["origin_parsed"]    = img.origin;
    out["scale_parsed"]     = img.scale;
    out["angles_parsed"]    = img.angles;
    out["size_parsed"]      = img.size;
    out["visible_parsed"]   = img.visible;
    out["alignment_parsed"] = img.alignment;
    out["puppet"]           = img.puppet;
    out["material"]         = dump_material(img.material);
    out["effect_count"]     = static_cast<int>(img.effects.size());
    // ImageEffect::id and ::version are left uninitialised by the
    // parser when the source json omits them, so dumping their raw value
    // produces stack garbage. Skip them.
    json effs = json::array();
    for (const auto& e : img.effects) {
        json je;
        je["name"]    = e.name;
        je["visible"] = e.visible;
        json mats     = json::array();
        for (const auto& mm : e.materials) mats.push_back(dump_material(mm));
        je["materials"] = std::move(mats);
        json passes     = json::array();
        for (const auto& p : e.passes) passes.push_back(dump_material_pass(p));
        je["passes"] = std::move(passes);
        json fbos    = json::array();
        for (const auto& f : e.fbos) fbos.push_back(dump_effect_fbo(f));
        je["fbos"]           = std::move(fbos);
        je["material_count"] = static_cast<int>(e.materials.size());
        je["pass_count"]     = static_cast<int>(e.passes.size());
        je["fbo_count"]      = static_cast<int>(e.fbos.size());
        effs.push_back(std::move(je));
    }
    out["effects"] = std::move(effs);
    return out;
}

template<typename Predicate>
std::vector<Corpus::TexRef> tex_filter(const std::vector<WorkshopEntry>& es, Predicate pred) {
    std::vector<Corpus::TexRef> out;
    for (const auto& e : es) {
        if (! e.snapshot.contains("textures")) continue;
        for (const auto& t : e.snapshot["textures"]) {
            if (! t.value("ok", false)) continue;
            if (pred(t)) out.push_back({ &e, &t });
        }
    }
    return out;
}

template<typename Predicate>
std::vector<Corpus::MdlRef> mdl_filter(const std::vector<WorkshopEntry>& es, Predicate pred) {
    std::vector<Corpus::MdlRef> out;
    for (const auto& e : es) {
        if (! e.snapshot.contains("puppets")) continue;
        for (const auto& m : e.snapshot["puppets"]) {
            if (pred(m)) out.push_back({ &e, &m });
        }
    }
    return out;
}

} // namespace

json DumpWorkshop(const std::string& workshop_dir, std::string& err, DumpFlags flags) {
    err.clear();
    json out;
    out["workshop_dir"]      = fs::path(workshop_dir).filename().string();
    const std::string pkg_id = fs::path(workshop_dir).filename().string();

    const std::string pkg_path = workshop_dir + "/scene.pkg";
    if (! fs::exists(pkg_path)) {
        err          = "scene.pkg not found at " + pkg_path;
        out["error"] = err;
        return out;
    }

    std::string           pkg_version;
    std::vector<PkgEntry> pkg_entries;
    if (! ReadPkgHeader(pkg_path, pkg_version, pkg_entries)) {
        err          = "failed to read pkg header";
        out["error"] = err;
        return out;
    }

    bool has_scene_json = false;
    for (const auto& e : pkg_entries)
        if (e.path == "/scene.json") {
            has_scene_json = true;
            break;
        }

    json& jpkg             = out["pkg"];
    jpkg["version"]        = pkg_version;
    jpkg["file_count"]     = static_cast<int>(pkg_entries.size());
    jpkg["has_scene_json"] = has_scene_json;

    owe::fs::VFS vfs;
    if (auto afs = owe::fs::CreatePhysicalFs(kAssetsDirMacro)) vfs.Mount("/assets", std::move(afs));
    auto pfs = owe::fs::CreatePhysicalFs(workshop_dir);
    auto wfs = owe::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) {
        err          = "WPPkgFs::CreatePkgFs failed";
        out["error"] = err;
        return out;
    }
    vfs.Mount("/assets", std::move(wfs));
    if (pfs) vfs.Mount("/assets", std::move(pfs));

    if (has_scene_json) {
        auto stream = vfs.Open("/assets/scene.json");
        if (stream) {
            std::string text = stream->ReadAllStr();
            try {
                auto                        j = json::parse(text);
                owe::wpscene::SceneMetadata scene;
                bool                        parsed = scene.FromJson(j);
                json&                       jscene = out["scene"];
                jscene["parsed"]                   = parsed;
                jscene["is_ortho"]                 = scene.general.isOrtho;
                jscene["ortho"]                    = {
                    { "width", scene.general.orthogonalprojection.width },
                    { "height", scene.general.orthogonalprojection.height },
                };
                jscene["camera"] = {
                    { "center", scene.camera.center },
                    { "eye", scene.camera.eye },
                    { "up", scene.camera.up },
                };
                // cameraparallaxamount/delay/mouseinfluence are undefaulted
                // floats in SceneGeneral, so when the source scene.json
                // omits them the parser leaves stack garbage. Only emit them
                // when cameraparallax is enabled.
                json jgen = {
                    { "clearcolor", scene.general.clearcolor },
                    { "ambientcolor", scene.general.ambientcolor },
                    { "skylightcolor", scene.general.skylightcolor },
                    { "cameraparallax", scene.general.cameraparallax },
                    { "zoom", scene.general.zoom },
                    { "fov", scene.general.fov },
                    { "nearz", scene.general.nearz },
                    { "farz", scene.general.farz },
                };
                if (scene.general.cameraparallax) {
                    jgen["cameraparallaxamount"] = scene.general.cameraparallaxamount;
                    jgen["cameraparallaxdelay"]  = scene.general.cameraparallaxdelay;
                    jgen["cameraparallaxmouseinfluence"] =
                        scene.general.cameraparallaxmouseinfluence;
                }
                jscene["general"] = std::move(jgen);
                json jobjects     = json::array();
                if (j.contains("objects") && j["objects"].is_array()) {
                    for (const auto& obj : j["objects"]) {
                        if (obj.contains("image"))
                            jobjects.push_back(dump_image_object(obj, vfs));
                        else if (obj.contains("light"))
                            jobjects.push_back(dump_light_object(obj, vfs));
                        else if (obj.contains("particle"))
                            jobjects.push_back(dump_particle_object(obj, vfs));
                        else if (obj.contains("sound"))
                            jobjects.push_back(dump_sound_object(obj, vfs));
                        else {
                            json o    = dump_object_common(obj);
                            o["kind"] = "unknown";
                            jobjects.push_back(std::move(o));
                        }
                    }
                }
                std::sort(jobjects.begin(), jobjects.end(), [](const json& a, const json& b) {
                    return a.value("id", -1) < b.value("id", -1);
                });
                jscene["object_count"] = static_cast<int>(jobjects.size());
                jscene["objects"]      = std::move(jobjects);
            } catch (const std::exception& e) {
                out["scene"] = { { "parsed", false }, { "error", e.what() } };
            }
        }
    }

    if (flags.tex) {
        json jtex = json::array();
        for (const auto& e : pkg_entries) {
            if (! ends_with(e.path, ".tex")) continue;
            if (e.path.rfind("/materials/", 0) != 0) continue;
            std::string vfs_path = "/assets" + e.path;
            TexMeta     m        = ReadTexMeta(vfs, e.path);
            json        jm;
            jm["path"]          = e.path;
            jm["ok"]            = m.ok;
            jm["texv"]          = m.texv;
            jm["texi"]          = m.texi;
            jm["texb"]          = m.texb;
            jm["texs"]          = m.texs;
            jm["compo1"]        = m.compo1;
            jm["compo2"]        = m.compo2;
            jm["compo3"]        = m.compo3;
            jm["format"]        = m.format;
            jm["image_type"]    = m.image_type;
            jm["width"]         = m.width;
            jm["height"]        = m.height;
            jm["map_width"]     = m.map_width;
            jm["map_height"]    = m.map_height;
            jm["count"]         = m.count;
            jm["is_sprite"]     = m.is_sprite;
            jm["sprite_frames"] = m.sprite_frames;
            jm["mipmap_pow2"]   = m.mipmap_pow2;
            jm["mipmap_larger"] = m.mipmap_larger;
            jm["wrap_s"]        = m.wrap_s;
            jm["wrap_t"]        = m.wrap_t;
            jm["min_filter"]    = m.min_filter;
            jm["mag_filter"]    = m.mag_filter;
            jtex.push_back(std::move(jm));
        }
        sort_by_path(jtex);
        out["textures"] = std::move(jtex);
    }

    if (flags.shader) {
        json jsh = json::array();
        for (const auto& e : pkg_entries) {
            if (e.path.rfind("/materials/", 0) != 0) continue;
            if (! ends_with(e.path, ".json")) continue;
            json jm { { "path", e.path } };
            auto stream = vfs.Open("/assets" + e.path);
            if (! stream) {
                jm["ok"]    = false;
                jm["error"] = "cannot open";
                jsh.push_back(std::move(jm));
                continue;
            }
            const std::string text = stream->ReadAllStr();
            json              jmat;
            try {
                jmat = json::parse(text);
            } catch (const std::exception& ex) {
                jm["ok"]    = false;
                jm["error"] = std::string("json: ") + ex.what();
                jsh.push_back(std::move(jm));
                continue;
            }
            owe::CompileMaterialShaderResult r;
            try {
                r = owe::WPShaderParser::CompileMaterialShader(jmat, vfs, pkg_id);
            } catch (const std::exception& ex) {
                r.ok    = false;
                r.error = ex.what();
            } catch (...) {
                r.ok    = false;
                r.error = "unknown exception";
            }
            jm["ok"]          = r.ok;
            jm["shader_name"] = r.shader_name;
            if (! r.ok) jm["error"] = r.error;
            jsh.push_back(std::move(jm));
        }
        sort_by_path(jsh);
        out["shaders"] = std::move(jsh);
    }

    if (! flags.mdl) return out;

    auto emit_flag = [](uint32_t flag) {
        json flag_arr = json::array();
        for (int byte_idx = 0; byte_idx < 4; ++byte_idx) {
            uint8_t     b = static_cast<uint8_t>((flag >> (byte_idx * 8)) & 0xFFu);
            std::string bits(8, '0');
            for (int i = 0; i < 8; ++i)
                if (b & (1u << (7 - i))) bits[i] = '1';
            flag_arr.push_back(std::move(bits));
        }
        return flag_arr;
    };

    json jmdl = json::array();
    if (! flags.mdl_full) {
        for (const auto& e : pkg_entries) {
            if (! ends_with(e.path, ".mdl")) continue;
            std::string rel = e.path;
            if (! rel.empty() && rel.front() == '/') rel.erase(0, 1);
            WPMdlHeader h;
            bool        ok = false;
            try {
                ok = owe::WPMdlParser::ParseHeader(rel, vfs, h);
            } catch (const std::exception&) {
                ok = false;
            }
            json jm;
            jm["path"]       = e.path;
            jm["ok"]         = ok;
            jm["mdlv"]       = h.mdlv;
            jm["flag"]       = emit_flag(h.mdl_flag);
            jm["unk_a"]      = static_cast<int64_t>(h.unk_a);
            jm["mesh_count"] = static_cast<int64_t>(h.mesh_count);
            jmdl.push_back(std::move(jm));
        }
        sort_by_path(jmdl);
        out["puppets"] = std::move(jmdl);
        return out;
    }

    for (const auto& e : pkg_entries) {
        if (! ends_with(e.path, ".mdl")) continue;
        // WPMdlParser::Parse expects a path relative to /assets without the
        // leading slash.
        std::string rel = e.path;
        if (! rel.empty() && rel.front() == '/') rel.erase(0, 1);
        WPMdl mdl;
        bool  ok = false;
        try {
            ok = owe::WPMdlParser::Parse(rel, vfs, mdl);
        } catch (const std::exception&) {
            ok = false;
        }
        json jm;
        jm["path"]             = e.path;
        jm["ok"]               = ok;
        jm["mdlv"]             = mdl.header.mdlv;
        jm["flag"]             = emit_flag(mdl.header.mdl_flag);
        jm["unk_a"]            = static_cast<int64_t>(mdl.header.unk_a);
        jm["mesh_count"]       = static_cast<int64_t>(mdl.header.mesh_count);
        jm["mdls"]             = mdl.mdls;
        jm["mdla"]             = mdl.mdla;
        const WPMdl::Mesh* m0  = mdl.meshes.empty() ? nullptr : &mdl.meshes.front();
        jm["mat_json_file"]    = m0 ? m0->mat_json_file : std::string();
        jm["vertex_count"]     = m0 ? static_cast<int>(m0->positions.size()) : 0;
        jm["index_count"]      = m0 ? static_cast<int>(m0->indices.size()) : 0;
        jm["vert_extra_count"] = m0 ? static_cast<int>(m0->part_uv2.size()) : 0;
        jm["part_count"]       = m0 ? static_cast<int>(m0->parts.size()) : 0;
        json parts_arr         = json::array();
        if (m0) {
            for (const auto& pt : m0->parts) {
                parts_arr.push_back({
                    { "id", (int64_t)pt.id },
                    { "start", (int64_t)pt.start },
                    { "size", (int64_t)pt.size },
                });
            }
        }
        jm["parts"] = std::move(parts_arr);
        jm["bones"] = ok && mdl.puppet ? static_cast<int>(mdl.puppet->bones.size()) : 0;
        jm["anims"] = ok && mdl.puppet ? static_cast<int>(mdl.puppet->anims.size()) : 0;
        if (ok && mdl.puppet) {
            json bones = json::array();
            for (const auto& b : mdl.puppet->bones) {
                json jb;
                jb["name"]                = b.name;
                jb["bind_parent"]         = static_cast<int64_t>(b.bind_parent);
                jb["anim_parent"]         = static_cast<int64_t>(b.anim_parent);
                jb["has_sim_json"]        = ! b.simulation_json.empty();
                jb["has_file_skin_pivot"] = b.has_file_skin_pivot;
                jb["centroid_offset"]     = {
                    b.vertex_centroid_offset.x(),
                    b.vertex_centroid_offset.y(),
                    b.vertex_centroid_offset.z(),
                };
                std::array<double, 4> col_sums { 0, 0, 0, 0 };
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        col_sums[static_cast<std::size_t>(c)] += b.local_bind.matrix()(r, c);
                jb["transform_col_sums"] = col_sums;
                bones.push_back(std::move(jb));
            }
            jm["bone_tree"]        = std::move(bones);
            jm["attachment_count"] = static_cast<int>(mdl.puppet->attachments.size());
            json atts              = json::array();
            for (const auto& a : mdl.puppet->attachments) {
                atts.push_back({ { "name", a.name } });
            }
            jm["attachments"] = std::move(atts);

            json anims = json::array();
            for (const auto& a : mdl.puppet->anims) {
                json ja;
                ja["id"]                  = a.id;
                ja["fps"]                 = a.fps;
                ja["length"]              = a.length;
                ja["name"]                = a.name;
                ja["mode"]                = static_cast<int>(a.mode);
                ja["bone_track_count"]    = static_cast<int>(a.bone_tracks.size());
                ja["has_trans"]           = a.trans.has_value();
                ja["blend_curves_count"]  = static_cast<int>(a.blend_curves.size());
                ja["v4_events_count"]     = static_cast<int>(a.v4_events.size());
                ja["has_aabb"]            = a.has_aabb;
                ja["scalar_curves_count"] = static_cast<int>(a.scalar_curves.size());
                ja["events_count"]        = static_cast<int>(a.events.size());
                int total_frames          = 0;
                for (const auto& bt : a.bone_tracks)
                    total_frames += static_cast<int>(bt.frames.size());
                ja["total_bone_frames"] = total_frames;
                json moved              = json::array();
                for (size_t ti = 0; ti < a.bone_tracks.size(); ++ti) {
                    const auto& tk = a.bone_tracks[ti];
                    if (tk.frames.empty()) continue;
                    const auto& f0      = tk.frames[0];
                    bool        any_pos = false, any_sc = false, any_an = false;
                    for (const auto& fr : tk.frames) {
                        if ((fr.position - f0.position).norm() > 0.5f) any_pos = true;
                        if ((fr.scale - f0.scale).norm() > 0.01f) any_sc = true;
                        if ((fr.angle - f0.angle).norm() > 0.001f) any_an = true;
                    }
                    if (any_pos || any_sc || any_an) {
                        moved.push_back({ { "i", (int)ti },
                                          { "p", any_pos },
                                          { "s", any_sc },
                                          { "a", any_an } });
                    }
                }
                ja["moved_bones"] = std::move(moved);
                anims.push_back(std::move(ja));
            }
            jm["anim_tracks"] = std::move(anims);
        }
        jmdl.push_back(std::move(jm));
    }
    sort_by_path(jmdl);
    out["puppets"] = std::move(jmdl);

    return out;
}

const Corpus& Corpus::instance() {
    static const Corpus c;
    return c;
}

Corpus::Corpus() { build(); }

void Corpus::build() {
    fs::path root { kWorkshopDirMacro };
    if (! fs::exists(root) || ! fs::is_directory(root)) {
        std::fprintf(stderr, "corpus: workshop dir %s missing\n", root.c_str());
        return;
    }

    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(root)) {
        if (! e.is_directory()) continue;
        if (! fs::exists(e.path() / "scene.pkg")) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());

    entries_.reserve(dirs.size());
    for (const auto& d : dirs) {
        std::string id = d.filename().string();
        if (kSkipIds.contains(id)) continue;

        std::string err;
        auto        snap = DumpWorkshop(d.string(), err);
        if (! err.empty()) {
            std::fprintf(stderr, "corpus: skip %s: %s\n", id.c_str(), err.c_str());
            continue;
        }

        WorkshopEntry e { std::move(id), d.string(), std::move(snap) };

        if (e.snapshot.contains("pkg"))
            pkg_versions_.insert(e.snapshot["pkg"].value("version", std::string {}));
        if (e.snapshot.contains("textures")) {
            for (const auto& t : e.snapshot["textures"]) {
                if (! t.value("ok", false)) continue;
                texv_versions_.insert(t.value("texv", 0));
                texi_versions_.insert(t.value("texi", 0));
                texb_versions_.insert(t.value("texb", 0));
                texs_versions_.insert(t.value("texs", 0));
                tex_formats_.insert(t.value("format", -1));
            }
        }
        if (e.snapshot.contains("puppets")) {
            for (const auto& m : e.snapshot["puppets"]) {
                mdlv_versions_.insert(m.value("mdlv", 0));
                mdls_versions_.insert(m.value("mdls", 0));
                mdla_versions_.insert(m.value("mdla", 0));
            }
        }
        entries_.push_back(std::move(e));
    }

    std::fprintf(stderr,
                 "corpus: indexed %zu workshops; pkgv=%zu texv=%zu texi=%zu texb=%zu "
                 "texs=%zu fmt=%zu mdlv=%zu mdls=%zu mdla=%zu\n",
                 entries_.size(),
                 pkg_versions_.size(),
                 texv_versions_.size(),
                 texi_versions_.size(),
                 texb_versions_.size(),
                 texs_versions_.size(),
                 tex_formats_.size(),
                 mdlv_versions_.size(),
                 mdls_versions_.size(),
                 mdla_versions_.size());
}

std::vector<Corpus::PkgRef> Corpus::workshops_with_pkg(const std::string& v) const {
    std::vector<PkgRef> out;
    for (const auto& e : entries_)
        if (e.snapshot.value("/pkg/version"_json_pointer, std::string {}) == v)
            out.push_back({ &e });
    return out;
}

std::vector<Corpus::TexRef> Corpus::textures_with_texv(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) {
        return t.value("texv", -1) == v;
    });
}
std::vector<Corpus::TexRef> Corpus::textures_with_texi(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) {
        return t.value("texi", -1) == v;
    });
}
std::vector<Corpus::TexRef> Corpus::textures_with_texb(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) {
        return t.value("texb", -1) == v;
    });
}
std::vector<Corpus::TexRef> Corpus::textures_with_texs(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) {
        return t.value("texs", -1) == v;
    });
}
std::vector<Corpus::TexRef> Corpus::textures_with_format(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) {
        return t.value("format", -1) == v;
    });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdlv(int v) const {
    return mdl_filter(entries_, [v](const nlohmann::json& m) {
        return m.value("mdlv", -1) == v;
    });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdls(int v) const {
    return mdl_filter(entries_, [v](const nlohmann::json& m) {
        return m.value("mdls", -1) == v;
    });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdla(int v) const {
    return mdl_filter(entries_, [v](const nlohmann::json& m) {
        return m.value("mdla", -1) == v;
    });
}

} // namespace owe::testing
