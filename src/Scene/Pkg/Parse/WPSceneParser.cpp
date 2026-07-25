module;

#include <algorithm>
#include <rstd/macro.hpp>
#include <string>
#include <vector>

#include "Utils/Sha.hpp"

module wescene.pkg.parse;
import eigen;
import wescene.spec_names;
import wescene.load_bench;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

std::string getAddr(void* p) { return std::to_string(reinterpret_cast<intptr_t>(p)); }

// ParseContext, SceneObjectVar, ProcessOpts and the stage entry points
// (ExpandObjects / AdjustAutoOrthoProjection / BuildContext /
// ProcessObjects / FinalizeScene) are exported from the
// :scene_stages partition; their definitions live near the bottom of
// this file.

namespace
{

auto LoadJsonFile(fs::VFS& vfs, const std::string& path) -> std::optional<Json> {
    auto parsed = owe::ReadJsonFile(vfs, path);
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err_unchecked();
        rstd_error("Can't load json {}: {}", path, error.message.as_str());
        return std::nullopt;
    }
    return rstd::move(parsed).unwrap_unchecked();
}

struct SceneNodeArcHold {
    Arc<SceneNode> node;

    explicit SceneNodeArcHold(Arc<SceneNode> n): node(rstd::move(n)) {}
    SceneNodeArcHold(const SceneNodeArcHold& other): node(other.node.clone()) {}
    SceneNodeArcHold(SceneNodeArcHold&&) noexcept            = default;
    SceneNodeArcHold& operator=(SceneNodeArcHold&&) noexcept = default;
    SceneNodeArcHold& operator=(const SceneNodeArcHold&)     = delete;

    SceneNode* get() const { return node.as_ptr(); }
};

template<typename T>
struct CopyableArcHold {
    Arc<T> value;

    explicit CopyableArcHold(Arc<T> owner): value(rstd::move(owner)) {}
    CopyableArcHold(const CopyableArcHold& other): value(other.value.clone()) {}
    CopyableArcHold(CopyableArcHold&&) noexcept            = default;
    CopyableArcHold& operator=(CopyableArcHold&&) noexcept = default;
    CopyableArcHold& operator=(const CopyableArcHold&)     = delete;
};

// Detect the WE audio-bar fanout pattern: scripts that bind a layer's
// `visible` field, call engine.registerAudioBuffers(N), and create sibling
// layers in init() via thisScene.createLayer(...). owe doesn't have a runtime
// model parser, so we pre-spawn N-1 hidden SceneNode clones as the maximum
// audio-driven capacity and hand them to FieldScript::clone_queue. init()
// activates only the clones it consumes; the rest remain hidden.
//
// Returns N (capacity) when the source matches the pattern, otherwise 0.
unsigned DetectAudioFanoutCapacity(std::string_view src) {
    auto pos = src.find("registerAudioBuffers");
    if (pos == std::string_view::npos) return 0;
    if (src.find("createLayer") == std::string_view::npos) return 0;
    pos += std::string_view("registerAudioBuffers").size();
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t')) ++pos;
    if (pos >= src.size() || src[pos] != '(') return 0;
    ++pos;
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t')) ++pos;

    auto is_digit = [](char c) {
        return c >= '0' && c <= '9';
    };
    auto is_ident = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '$';
    };
    auto read_num = [&](std::size_t p) -> unsigned {
        unsigned n = 0;
        while (p < src.size() && is_digit(src[p])) n = n * 10 + unsigned(src[p++] - '0');
        return n;
    };

    // Numeric literal: registerAudioBuffers(64).
    if (pos < src.size() && is_digit(src[pos])) return read_num(pos);

    // Public WE constants: registerAudioBuffers(engine.AUDIO_RESOLUTION_64).
    constexpr std::string_view resolution_prefix = "engine.AUDIO_RESOLUTION_";
    if (src.substr(pos).starts_with(resolution_prefix)) {
        const std::size_t value_pos = pos + resolution_prefix.size();
        if (value_pos >= src.size() || ! is_digit(src[value_pos])) return 0;
        const unsigned value = read_num(value_pos);
        std::size_t    end   = value_pos;
        while (end < src.size() && is_digit(src[end])) ++end;
        if (end < src.size() && is_ident(src[end])) return 0;
        if (value == 16 || value == 32 || value == 64) return value;
        return 0;
    }

    // Variable: registerAudioBuffers(audioBuffer) with `var audioBuffer = 64`
    // earlier (the common WE audio-bar template). Resolve the first
    // `<ident> = <number>` assignment to that name. We don't run JS, so this
    // only handles a literal-initialized count (always 16/32/64 in practice).
    if (pos >= src.size() || ! is_ident(src[pos]) || is_digit(src[pos])) return 0;
    std::size_t e = pos;
    while (e < src.size() && is_ident(src[e])) ++e;
    std::string_view name = src.substr(pos, e - pos);
    for (std::size_t p = 0; (p = src.find(name, p)) != std::string_view::npos; p += name.size()) {
        const bool        lb = (p == 0) || ! is_ident(src[p - 1]);
        const std::size_t a  = p + name.size();
        const bool        rb = (a >= src.size()) || ! is_ident(src[a]);
        if (! lb || ! rb) continue;
        std::size_t q = a;
        while (q < src.size() && (src[q] == ' ' || src[q] == '\t')) ++q;
        if (q >= src.size() || src[q] != '=') continue;
        ++q;
        while (q < src.size() && (src[q] == ' ' || src[q] == '\t')) ++q;
        if (q < src.size() && is_digit(src[q])) return read_num(q);
    }
    return 0;
}

bool SourceWritesLayerText(std::string_view src) {
    const bool writes_text = src.find(".text") != std::string_view::npos ||
                             src.find("[\"text\"]") != std::string_view::npos ||
                             src.find("['text']") != std::string_view::npos;
    if (! writes_text) return false;
    return src.find("getLayer") != std::string_view::npos;
}

bool FieldBindingsWriteLayerText(const wpscene::FieldBindings& fb) {
    for (const auto& [_, sb] : fb.scripts) {
        if (SourceWritesLayerText(sb.source)) return true;
    }
    return false;
}

const wpscene::FieldBindings& SceneObjectFieldBindings(const SceneObjectVar& object) {
    if (object.is_Image()) return object.as_Image().value.field_bindings;
    if (object.is_Particle()) return object.as_Particle().value.field_bindings;
    if (object.is_Sound()) return object.as_Sound().value.field_bindings;
    if (object.is_Light()) return object.as_Light().value.field_bindings;
    if (object.is_Text()) return object.as_Text().value.field_bindings;
    if (object.is_Model()) return object.as_Model().value.field_bindings;
    return object.as_Camera().value.field_bindings;
}

bool SceneWritesLayerText(slice<SceneObjectVar> scene_objs) {
    for (usize index {}; index < scene_objs.len(); ++index) {
        if (FieldBindingsWriteLayerText(SceneObjectFieldBindings(scene_objs[index]))) return true;
    }
    return false;
}

std::vector<std::string> DetectRegisteredAssets(std::string_view src) {
    std::vector<std::string> out;
    auto                     seen = std::unordered_set<std::string> {};
    for (std::size_t pos = 0; (pos = src.find("registerAsset", pos)) != std::string_view::npos;) {
        pos += std::string_view("registerAsset").size();
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n')) ++pos;
        if (pos >= src.size() || src[pos] != '(') continue;
        ++pos;
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n')) ++pos;
        if (pos >= src.size() || (src[pos] != '\'' && src[pos] != '"')) continue;
        char        quote = src[pos++];
        std::size_t begin = pos;
        while (pos < src.size()) {
            if (src[pos] == '\\' && pos + 1 < src.size()) {
                pos += 2;
                continue;
            }
            if (src[pos] == quote) break;
            ++pos;
        }
        if (pos >= src.size()) break;
        std::string asset { src.substr(begin, pos - begin) };
        if (seen.insert(asset).second) out.push_back(std::move(asset));
    }
    return out;
}

std::optional<std::array<float, 2>> ResolveImageAssetSize(ParseContext&    context,
                                                          std::string_view image_path) {
    auto info = wpscene::LoadImageAssetInfo(*context.vfs, image_path);
    if (! info) return std::nullopt;
    if (info->size) return info->size;
    if (info->first_texture.empty()) return std::nullopt;

    auto parsed_header =
        context.scene->ParseImageHeader(rstd::cppstd::as_str(info->first_texture).unwrap());
    if (parsed_header.is_err()) return std::nullopt;
    auto    header = rstd::move(parsed_header).unwrap_unchecked();
    int32_t w      = 0;
    int32_t h      = 0;
    if (header.isSprite && header.spriteAnim.numFrames() != usize()) {
        const auto& frame = header.spriteAnim.GetCurFrame();
        w                 = static_cast<int32_t>(std::round(frame.width));
        h                 = static_cast<int32_t>(std::round(frame.height));
    } else {
        w = header.width > 0 ? header.width : header.mapWidth;
        h = header.height > 0 ? header.height : header.mapHeight;
    }
    if (w <= 0 || h <= 0) return std::nullopt;
    return std::array { static_cast<float>(w), static_cast<float>(h) };
}

bool AppendLayerCompositePassthroughEffect(fs::VFS& vfs, wpscene::ImageObject& image) {
    wpscene::Material material;
    auto              json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json");
    if (! json || ! material.FromJson(*json)) {
        rstd_error("parse effectpassthrough.json failed for '{}'", image.name);
        return false;
    }

    wpscene::ImageEffect effect;
    effect.name    = "linked layer composite";
    effect.visible = true;
    effect.materials.push_back(std::move(material));
    image.effects.push_back(std::move(effect));
    return true;
}

Arc<WPPuppetLayer> MakePuppetLayer(Arc<WPPuppet>                            puppet,
                                   std::span<WPPuppetLayer::AnimationLayer> layers) {
    auto out = Arc<WPPuppetLayer>::make(rstd::move(puppet));
    out->prepared(
        slice<WPPuppetLayer::AnimationLayer>::from_raw_parts(layers.data(), usize(layers.size())));
    return out;
}

void RegisterPuppetLayer(ParseContext& context, SceneNode* node, Arc<WPPuppetLayer> layer) {
    if (! node) return;
    (void)context.puppet_layers->by_node.insert(node, rstd::move(layer));
}

void SetWPUniformConfig(ParseContext& context, const Arc<SceneNode>& node,
                        WPUniformNodeConfigDraft config) {
    config.configured = true;
    for (auto& entry : context.uniform_configs) {
        if (entry.node.as_ptr() != node.as_ptr()) continue;
        entry.config = rstd::move(config);
        return;
    }
    context.uniform_configs.push(ParseContext::UniformConfigDraft {
        .node   = node.clone(),
        .config = rstd::move(config),
    });
}

const WPUniformNodeConfigDraft* FindWPUniformConfig(const ParseContext& context,
                                                    const SceneNode&    node) {
    for (const auto& entry : context.uniform_configs) {
        if (entry.node.as_ptr() == &node) return &entry.config;
    }
    return nullptr;
}

void RegisterNodeRef(ParseContext& context, std::int32_t id, ParseContext::NodeRef node) {
    (void)context.node_id_map.insert(id, rstd::move(node));
}

void AddLayerClone(ParseContext& context, std::int32_t id, Arc<SceneNode> node) {
    auto clones = context.layer_clones.get_mut(id);
    if (clones.is_none()) {
        (void)context.layer_clones.insert(id, Vec<Arc<SceneNode>> {});
        clones = context.layer_clones.get_mut(id);
    }
    (**clones).push(rstd::move(node));
}

Option<Arc<WPPuppetLayer>> LookupPuppetLayer(const Arc<PuppetLayerRegistry>& layers,
                                             SceneNode*                      node) {
    if (! node) return None();
    if (auto layer = layers->by_node.get(node); layer.is_some()) return Some((**layer).clone());
    if (auto fallback = layers->fallback_by_node.get(node); fallback.is_some()) {
        return Some((**fallback).clone());
    }
    return None();
}

SceneNode* RootOf(SceneNode* node) {
    if (! node) return nullptr;
    while (node->Parent()) node = node->Parent();
    return node;
}

void CollectLinkedSourceIdsFromJsonValue(const Json& value, HashSet<std::int32_t>& out) {
    if (value.is_string()) {
        auto s = *value.as_str();
        if (auto id = ParseImageLayerCompositeId(s)) out.insert(static_cast<std::int32_t>(*id));
        if (IsSpecLinkTex(s)) out.insert(rstd::as_cast<std::int32_t>(ParseLinkTex(s)));
        return;
    }
    if (value.is_array()) {
        const auto values = value.as_array();
        for (const auto& el : **values) CollectLinkedSourceIdsFromJsonValue(el, out);
        return;
    }
    if (! value.is_object()) return;
    auto object = value.as_object();
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  key               = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& child             = *entry_value;
        if (key == "dependencies") {
            if (auto values = child.as_array(); values.is_some()) {
                for (const auto& dep : **values) {
                    auto id = dep.as_i64();
                    if (id.is_some() &&
                        id->to_primitive() >= std::numeric_limits<std::int32_t>::min() &&
                        id->to_primitive() <= std::numeric_limits<std::int32_t>::max()) {
                        out.insert(static_cast<std::int32_t>(id->to_primitive()));
                    }
                }
            }
        }
        CollectLinkedSourceIdsFromJsonValue(child, out);
    });
}

HashSet<std::int32_t> CollectLinkedSourceIdsFromJson(const Json& json) {
    HashSet<std::int32_t> out;
    if (auto objects = json.get("objects"_str); objects.is_some())
        CollectLinkedSourceIdsFromJsonValue(**objects, out);
    return out;
}

void MarkHiddenLinkSource(ParseContext& context, std::int32_t id) {
    if (context.hidden_link_source_ids.contains(id))
        context.scene->MarkLayerVisibilityElidable(WallpaperLayerId { .value = i32(id) });
}

SceneUserVisibilityBinding
ToSceneUserVisibilityBinding(const wpscene::VisibleUserBinding& binding) {
    SceneUserVisibilityBinding out;
    out.key           = String::make(rstd::cppstd::as_str(binding.name).unwrap());
    out.condition     = binding.condition.clone();
    out.has_condition = binding.has_condition;
    return out;
}

std::array<float, 2> Texture0UvScale(const SceneMaterial& material, bool nopadding = false) {
    if (nopadding) return { 1.0f, 1.0f };
    auto it = material.customShader.constValues.find(WE_GLTEX_RESOLUTION_NAMES[0]);
    if (it == material.customShader.constValues.end()) return { 1.0f, 1.0f };
    const auto& r = it->second;
    if (r.size() < usize(4) || r[usize(0)] == 0.0f || r[usize(1)] == 0.0f) {
        return { 1.0f, 1.0f };
    }
    return { r[usize(2)] / r[usize(0)], r[usize(3)] / r[usize(1)] };
}

float ParticleTextureRatio(const SceneMaterial& material) {
    auto it = material.customShader.constValues.find(WE_GLTEX_RESOLUTION_NAMES[0]);
    if (it == material.customShader.constValues.end()) return 1.0f;
    const auto& r = it->second;
    if (r.size() < usize(2) || r[usize(0)] == 0.0f) return 1.0f;
    return r[usize(1)] / r[usize(0)];
}

void InstallImageAlignmentBinding(script::JsRuntime& runtime, SceneNode* node, ref<str> alignment,
                                  const ParseContext::ImageAlignmentSetter& setter) {
    runtime.RegisterImageAlignmentSetter(
        node,
        alignment,
        script::JsRuntime::ImageAlignmentSetter::make(
            [node, setter = setter.clone()](ref<str> value) mutable {
                (*setter)(node, value);
            }));
}

void RegisterImageAlignmentBinding(ParseContext& context, SceneNode* node, ref<str> alignment,
                                   ParseContext::ImageAlignmentSetter setter) {
    if (context.script_scene.is_some()) {
        InstallImageAlignmentBinding((*context.script_scene)->runtime(), node, alignment, setter);
    }
    context.image_alignment_bindings.push(ParseContext::ImageAlignmentBinding {
        .node      = node,
        .alignment = String::make(alignment),
        .setter    = rstd::move(setter),
    });
}

void CloneImageAlignmentBinding(ParseContext& context, SceneNode* source, SceneNode* clone) {
    for (const auto& binding : context.image_alignment_bindings) {
        if (binding.node != source) continue;
        RegisterImageAlignmentBinding(
            context, clone, binding.alignment.as_str(), binding.setter.clone());
        return;
    }
}

Option<Arc<WPPuppetLayer>> FindPuppetLayerWithBone(const Arc<PuppetLayerRegistry>& layers,
                                                   SceneNode* node, std::string_view name,
                                                   uint32_t& index) {
    if (! node) return None();
    if (auto layer = layers->by_node.get(node); layer.is_some()) {
        index = (**layer)->boneIndex(rstd::cppstd::as_str(name).unwrap());
        if (index != 0) return Some((**layer).clone());
    }
    for (auto& child : node->GetChildren()) {
        auto hit = FindPuppetLayerWithBone(layers, child.as_ptr(), name, index);
        if (hit.is_some()) return hit;
    }
    return None();
}

std::vector<owe::SceneNode*> SpawnLayerClones(ParseContext& context, SceneNode* tmpl,
                                              unsigned count) {
    std::vector<owe::SceneNode*> out;
    if (! tmpl || count == 0) return out;
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        auto clone =
            Arc<SceneNode>::make(tmpl->Translate(), tmpl->Scale(), tmpl->Rotation(), tmpl->Name());
        clone->SetLocalFrame(tmpl->LocalFrame());
        clone->SetSize(tmpl->Size());
        clone->SetGeometryTransform(tmpl->GeometryTransform());
        clone->SetPerspective(tmpl->Perspective());
        if (! tmpl->Camera().empty()) clone->SetCamera(tmpl->Camera());
        clone->AddMesh(tmpl->MeshShared());
        clone->SetVisible(false);
        clone->ID() = i32(-static_cast<std::int32_t>(i) - 1); // negative IDs reserved for clones
        if (auto config = FindWPUniformConfig(context, *tmpl); config != nullptr)
            SetWPUniformConfig(context, clone, config->Clone());
        if (auto layer = LookupPuppetLayer(context.puppet_layers, tmpl); layer.is_some()) {
            RegisterPuppetLayer(context, clone.as_ptr(), rstd::move(*layer));
        }
        CloneImageAlignmentBinding(context, tmpl, clone.as_ptr());
        out.push_back(clone.as_ptr());
        // Defer attachment to FinalizeScene so the clones land at the
        // template's z-position (right after it), not at the root front.
        AddLayerClone(context, tmpl->ID().to_primitive(), rstd::move(clone));
    }
    return out;
}

script::ScriptScene& EnsureScriptScene(ParseContext& context) {
    if (context.script_scene.is_none()) {
        context.script_scene =
            Some(Box<script::ScriptScene>::make(Some(context.audio_response_demand.clone())));
        auto layers = CopyableArcHold(context.puppet_layers.clone());
        (*context.script_scene)
            ->runtime()
            .SetBoneResolvers(
                [layers](SceneNode* node, std::string_view name) -> uint32_t {
                    auto     layer = LookupPuppetLayer(layers.value, node);
                    uint32_t index = layer.is_some()
                                         ? (*layer)->boneIndex(rstd::cppstd::as_str(name).unwrap())
                                         : 0;
                    if (index != 0) return index;

                    if (auto fallback =
                            FindPuppetLayerWithBone(layers.value, RootOf(node), name, index);
                        fallback.is_some()) {
                        (void)layers.value->fallback_by_node.insert(node, rstd::move(*fallback));
                        return index;
                    }
                    return 0;
                },
                [layers](SceneNode* node,
                         uint32_t   index,
                         double     time) -> std::optional<script::BoneTranslation> {
                    auto layer = LookupPuppetLayer(layers.value, node);
                    if (layer.is_none()) return std::nullopt;
                    auto bone = (*layer)->boneTransform(index, time);
                    if (bone.is_none()) return std::nullopt;

                    node->UpdateTrans();
                    Eigen::Affine3f world = Eigen::Affine3f::Identity();
                    world.matrix()        = node->ModelTrans().cast<float>();
                    auto t                = (world * *bone).translation();
                    return script::BoneTranslation { t.x(), t.y(), t.z() };
                });
        if (context.user_properties.is_some())
            (*context.user_properties)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                auto key                      = rstd::cppstd::as_string_view(entry_key->as_str());
                (*context.script_scene)->runtime().SetUserProperty(key, *entry_value);
            });
        for (const auto& binding : context.image_alignment_bindings) {
            InstallImageAlignmentBinding((*context.script_scene)->runtime(),
                                         binding.node,
                                         binding.alignment.as_str(),
                                         binding.setter);
        }
    }
    return **context.script_scene;
}

void SetScriptInitializationOrder(ParseContext& context, script::FieldScript& script,
                                  const SceneNode* node) {
    if (node == nullptr) return;
    auto order = context.script_initialization_orders.get(node->ID().to_primitive());
    if (order.is_none()) return;
    EnsureScriptScene(context).runtime().SetInitializationOrder(script, **order);
}

std::optional<float> ScriptValueAsFloat(const script::ScriptValue& value) {
    if (auto* p = std::get_if<script::ScalarValue>(&value)) return static_cast<float>(p->v);
    if (auto* p = std::get_if<script::BoolValue>(&value)) return p->v ? 1.0f : 0.0f;
    if (auto* p = std::get_if<script::Vec2Value>(&value)) return static_cast<float>(p->x);
    if (auto* p = std::get_if<script::Vec3Value>(&value)) return static_cast<float>(p->x);
    return std::nullopt;
}

std::optional<Vector3f> ScriptValueAsVec3(const script::ScriptValue& value,
                                          const Vector3f&            current) {
    Vector3f next = current;
    if (auto* p = std::get_if<script::Vec3Value>(&value)) {
        next = Vector3f { static_cast<float>(p->x),
                          static_cast<float>(p->y),
                          static_cast<float>(p->z) };
    } else if (auto* p = std::get_if<script::Vec2Value>(&value)) {
        next = Vector3f { static_cast<float>(p->x), static_cast<float>(p->y), current.z() };
    } else if (auto* p = std::get_if<script::ScalarValue>(&value)) {
        next.x() = static_cast<float>(p->v);
    } else
        return std::nullopt;
    return next;
}

bool IsFractionSliderProperty(const ParseContext& context, const Json& binding) {
    if (context.user_properties.is_none() || ! binding.is_object()) return false;
    auto user = binding.get("user"_str);
    if (user.is_none()) return false;
    auto key = (*user)->as_str();
    if (key.is_none()) return false;
    auto prop = (*context.user_properties)->get(*key);
    if (prop.is_none() || ! (*prop)->is_object()) return false;
    auto type = (*prop)->get("type"_str);
    if (type.is_none()) return false;
    auto type_string = (*type)->as_str();
    if (type_string.is_none() || rstd::cppstd::as_string_view(*type_string) != "slider")
        return false;
    auto fraction = (*prop)->get("fraction"_str);
    return fraction.is_some() && (*fraction)->as_bool().unwrap_or(false);
}

Json ScriptPropertiesForField(const ParseContext& context, std::string_view field,
                              const wpscene::ScriptBinding& binding) {
    Json props = binding.properties.clone();
    if (field != "scale" || binding.source.find("/10000") == std::string::npos ||
        ! props.is_object())
        return props;

    auto object = props.as_object_mut();
    (*object)->iter_mut().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto& item                    = *entry_value;
        if (IsFractionSliderProperty(context, item)) {
            auto item_object = item.as_object_mut();
            (*item_object)
                ->insert(::alloc::string::String::make("__scriptValueScale"_str),
                         rstd::into<Json>(f64(50.0)));
        }
    });
    return props;
}

Json ScriptInitialValueForField(std::string_view field, const Json& value) {
    if (field != "angles") return value.clone();

    constexpr float kRadToDeg = 180.0f / rstd::f32::consts::PI.to_primitive();
    if (value.is_null()) return Json::Null();
    if (value.is_number()) {
        auto number = value.as_f64();
        return number.is_some() && number->to_primitive() >= std::numeric_limits<float>::lowest() &&
                       number->to_primitive() <= std::numeric_limits<float>::max()
                   ? rstd::into<Json>(f32(static_cast<float>(number->to_primitive()) * kRadToDeg))
                   : Json::Null();
    }

    if (value.is_object()) {
        auto out = value.clone();
        for (auto axis : rstd::array<ref<str>, 3> { "x"_str, "y"_str, "z"_str }) {
            auto member = out.get_mut(axis);
            if (member.is_none()) continue;
            auto number = (*member)->as_f64();
            if (number.is_some() &&
                number->to_primitive() >= std::numeric_limits<float>::lowest() &&
                number->to_primitive() <= std::numeric_limits<float>::max()) {
                **member =
                    rstd::into<Json>(f32(static_cast<float>(number->to_primitive()) * kRadToDeg));
            }
        }
        return out;
    }

    std::vector<float> values;
    if (owe::GetJsonValue(value, values) && ! values.empty()) {
        for (auto& axis : values) axis *= kRadToDeg;
        auto out = rstd::json::Array::make();
        for (float axis : values) out.push(rstd::into<Json>(f32(axis)));
        return Json::Array(rstd::move(out));
    }

    return value.clone();
}

std::array<std::int32_t, 2> TextLayerExtent(const text::TextGeometry& geometry) {
    return {
        std::max<std::int32_t>(1, static_cast<std::int32_t>(std::ceil(geometry.rt_width))),
        std::max<std::int32_t>(1, static_cast<std::int32_t>(std::ceil(geometry.rt_height))),
    };
}

std::uint32_t TextPointSizeToPx(float point_size) {
    constexpr float kPointsizeToPx = 4.0f;
    if (! std::isfinite(point_size) || point_size <= 0.0f) return 1;
    auto px = static_cast<std::uint32_t>(std::round(point_size * kPointsizeToPx));
    return std::clamp<std::uint32_t>(px, 1, 1024);
}

std::array<std::int32_t, 2> TextEffectFboExtent(const text::TextGeometry& geometry,
                                                std::uint32_t scale, std::uint32_t fit) {
    if (fit > 0) {
        const float max_size = std::max(geometry.effect_frame_width, geometry.effect_frame_height);
        if (max_size > 0.0f) {
            const float fit_scale = static_cast<float>(fit) / max_size;
            return {
                std::max<std::int32_t>(
                    1,
                    static_cast<std::int32_t>(std::round(geometry.effect_frame_width * fit_scale))),
                std::max<std::int32_t>(1,
                                       static_cast<std::int32_t>(
                                           std::round(geometry.effect_frame_height * fit_scale))),
            };
        }
    }
    const float fbo_scale = std::max(1.0f, static_cast<float>(scale));
    return {
        std::max<std::int32_t>(
            1, static_cast<std::int32_t>(std::round(geometry.effect_frame_width / fbo_scale))),
        std::max<std::int32_t>(
            1, static_cast<std::int32_t>(std::round(geometry.effect_frame_height / fbo_scale))),
    };
}

struct TextRuntimeFbo {
    std::string   name;
    std::uint32_t scale { 1 };
    std::uint32_t fit { 0 };
};

struct TextRuntimeEffectNode {
    SceneNode*                                       node { nullptr };
    std::shared_ptr<text::TextEffectProjectionState> text_projection;
};

struct TextRuntimeTargets {
    TextRuntimeTargets(Scene& scene_owner, mut_ref<WPUniformSceneState> state)
        : scene(&scene_owner), uniform_state(state) {}

    Scene*                             scene;
    mut_ref<WPUniformSceneState>       uniform_state;
    std::string                        camera_key;
    std::string                        ppong_a;
    std::string                        ppong_b;
    std::string                        effect_final;
    bool                               has_effect { false };
    std::int32_t                       layer_w { 1 };
    std::int32_t                       layer_h { 1 };
    std::vector<TextRuntimeFbo>        fbos;
    std::vector<TextRuntimeEffectNode> effect_nodes;

    bool Apply(const text::TextGeometry& geometry) {
        if (scene == nullptr) return false;

        bool changed          = false;
        auto [next_w, next_h] = TextLayerExtent(geometry);
        changed |= scene->ResizeRenderTarget(
            rstd::cppstd::as_str(ppong_a).unwrap(), i32(next_w), i32(next_h));
        if (has_effect) {
            changed |= scene->ResizeRenderTarget(
                rstd::cppstd::as_str(ppong_b).unwrap(), i32(next_w), i32(next_h));
            changed |= scene->ResizeRenderTarget(
                rstd::cppstd::as_str(effect_final).unwrap(), i32(next_w), i32(next_h));
        }

        auto camera = scene->CameraMut(rstd::cppstd::as_str(camera_key).unwrap());
        if (camera.is_some()) {
            auto& value = **camera;
            if (value.Width() != static_cast<double>(next_w) ||
                value.Height() != static_cast<double>(next_h)) {
                value.SetWidth(next_w);
                value.SetHeight(next_h);
                value.Update();
                changed = true;
            }
        }

        for (const auto& fbo : fbos) {
            auto [w, h] = TextEffectFboExtent(geometry, fbo.scale, fbo.fit);
            changed |=
                scene->ResizeRenderTarget(rstd::cppstd::as_str(fbo.name).unwrap(), i32(w), i32(h));
        }

        const array<float, 2> effect_size {
            geometry.effect_frame_width,
            geometry.effect_frame_height,
        };
        for (auto& item : effect_nodes) {
            if (item.text_projection) {
                item.text_projection->size = effect_size;
                continue;
            }
            if (item.node == nullptr) continue;
            auto node = scene->ResourceIndex().nodeId(*item.node);
            if (node.is_some()) {
                (void)uniform_state->SetEffectProjectionSize(*node, effect_size);
            }
        }

        layer_w = next_w;
        layer_h = next_h;
        return changed;
    }
};

SceneAnimationKey ToSceneAnimationKey(const wpscene::AnimKeyframe& key) {
    return {
        .frame         = key.frame,
        .value         = key.value,
        .front_enabled = key.front.enabled,
        .front_x       = key.front.x,
        .front_y       = key.front.y,
        .back_enabled  = key.back.enabled,
        .back_x        = key.back.x,
        .back_y        = key.back.y,
    };
}

Vec<SceneAnimationKey> ToSceneAnimationAxis(const std::vector<wpscene::AnimKeyframe>& keys) {
    Vec<SceneAnimationKey> out;
    out.reserve(usize(keys.size()));
    for (const auto& key : keys) out.push(ToSceneAnimationKey(key));
    sort_unstable_by(out.as_mut_slice().as_mut_ref(),
                     [](const SceneAnimationKey& left, const SceneAnimationKey& right) {
                         return left.frame < right.frame;
                     });
    return out;
}

SceneAnimationCurve ToSceneAnimationCurve(const wpscene::AnimCurve& curve) {
    SceneAnimationCurve out;
    out.c0       = ToSceneAnimationAxis(curve.c0);
    out.c1       = ToSceneAnimationAxis(curve.c1);
    out.c2       = ToSceneAnimationAxis(curve.c2);
    out.fps      = curve.options.fps;
    out.length   = curve.options.length;
    out.mode     = String::make(rstd::cppstd::as_str(curve.options.mode).unwrap());
    out.wraploop = curve.options.wraploop;
    out.relative = curve.relative;
    return out;
}

void AssignCurve(SceneAnimationCurve& dst, const wpscene::FieldBindings& bindings,
                 std::string_view field) {
    auto it = bindings.animations.find(std::string(field));
    if (it != bindings.animations.end()) dst = ToSceneAnimationCurve(it->second);
}

void AssignNodeFieldAnimations(SceneNode& node, const wpscene::FieldBindings& bindings) {
    auto origin_it = bindings.animations.find("origin");
    if (origin_it != bindings.animations.end())
        node.SetOriginAnimation(ToSceneAnimationCurve(origin_it->second));
    auto scale_it = bindings.animations.find("scale");
    if (scale_it != bindings.animations.end())
        node.SetScaleAnimation(ToSceneAnimationCurve(scale_it->second));
    auto angles_it = bindings.animations.find("angles");
    if (angles_it != bindings.animations.end())
        node.SetRotationAnimation(ToSceneAnimationCurve(angles_it->second));
    auto it = bindings.animations.find("alpha");
    if (it != bindings.animations.end()) node.SetAlphaAnimation(ToSceneAnimationCurve(it->second));
}

Option<SceneCameraLookAtKey> ParseLookAtKey(const Json& json) {
    if (! json.is_object()) return None();
    SceneCameraLookAtKey key;
    std::array<float, 3> eye {};
    std::array<float, 3> center {};
    std::array<float, 3> up {};
    if (! owe::GetJsonValue(json, "eye", eye, false)) return None();
    if (! owe::GetJsonValue(json, "center", center, false)) return None();
    if (! owe::GetJsonValue(json, "up", up, false)) return None();
    owe::GetJsonValue(json, "timestamp", key.frame, false);
    key.eye    = Vector3f(eye.data());
    key.center = Vector3f(center.data());
    key.up     = Vector3f(up.data());
    return Some(rstd::move(key));
}

Option<SceneCameraLookAtTrack> ParseLookAtTrack(const Json& json) {
    auto transforms = json.get("transforms"_str);
    if (transforms.is_none()) return None();
    auto transform_array = (*transforms)->as_array();
    if (transform_array.is_none()) return None();

    SceneCameraLookAtTrack track;
    owe::GetJsonValue(json, "duration", track.duration, false);
    for (const auto& raw_key : **transform_array) {
        auto key = ParseLookAtKey(raw_key);
        if (key.is_some()) track.keys.push(rstd::move(*key));
    }
    if (track.keys.is_empty()) return None();

    sort_unstable_by(track.keys.as_mut_slice().as_mut_ref(),
                     [](const SceneCameraLookAtKey& left, const SceneCameraLookAtKey& right) {
                         return left.frame < right.frame;
                     });
    if (track.duration <= 0.0f) track.duration = track.keys[track.keys.len() - usize(1)].frame;
    if (track.duration <= 0.0f) track.duration = 1.0f;
    return Some(rstd::move(track));
}

void LoadRootCameraPaths(ParseContext& context, const wpscene::SceneMetadata& sc) {
    if (sc.general.isOrtho || sc.camera.paths.empty() || context.vfs == nullptr) return;

    auto camera = context.scene->CameraHandle("global_perspective"_str);
    if (camera.is_none()) return;

    auto path               = Arc<SceneCameraPath>::make();
    path->camera_name       = String::make("global_perspective"_str);
    path->camera            = Some(rstd::move(*camera));
    path->node              = context.global_perspective_camera_node.is_some()
                                  ? (*context.global_perspective_camera_node).as_ptr()
                                  : nullptr;
    path->default_translate = path->node ? path->node->Translate() : Vector3f::Zero();
    path->default_rotation  = path->node ? path->node->Rotation() : Vector3f::Zero();
    path->default_width     = (**path->camera).Width();
    path->default_height    = (**path->camera).Height();
    path->default_fov       = (**path->camera).Fov();
    path->fov_base          = static_cast<float>((**path->camera).Fov());
    path->perspective       = true;
    path->enabled           = true;
    path->default_lookat    = true;
    path->default_eye       = Vector3f(sc.camera.eye.data());
    path->default_center    = Vector3f(sc.camera.center.data());
    path->default_up        = Vector3f(sc.camera.up.data());

    for (const auto& rel : sc.camera.paths) {
        auto file = fs::OpenBinary(*context.vfs, "/assets/" + rel);
        if (file.is_err()) continue;
        auto parsed = ParseJson(file->ReadAllStr());
        if (parsed.is_err()) {
            rstd_warn("Can't parse camera path json {}: {}", rel, parsed.unwrap_err());
            continue;
        }
        auto json   = parsed.unwrap();
        auto tracks = json.get("paths"_str);
        if (tracks.is_none()) continue;
        auto track_array = (*tracks)->as_array();
        if (track_array.is_none()) continue;
        for (const auto& raw_track : **track_array) {
            auto track = ParseLookAtTrack(raw_track);
            if (track.is_some()) path->lookat_tracks.push(rstd::move(*track));
        }
    }

    if (! path->lookat_tracks.is_empty()) context.scene->RegisterCameraPath(rstd::move(path));
}
} // namespace

// Walks `fb.scripts` for one parsed object's field bindings and, for the
// supported fields, creates a FieldScript + closure-based Actuator. Text
// bindings are wired by ParseTextObj's own call site (with the layouter
// closure); side-effect-only bindings (`visible`) get the script without an
// actuator so update() still drives scene mutations.
void WireFieldScripts(ParseContext& context, const Arc<SceneNode>& node_sp,
                      const wpscene::FieldBindings&                   fb,
                      std::function<void(const script::ScriptValue&)> origin_apply = {},
                      std::function<void(const script::ScriptValue&)> scale_apply  = {}) {
    SceneNode* node = node_sp.as_ptr();
    if (fb.scripts.empty()) return;
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::NodeTransformTarget tgt = script::NodeTransformTarget::Translate;
        script::FieldKind           kind;
        bool                        has_actuator = true;
        bool                        is_alpha     = false;
        bool                        is_color     = false;
        if (field == "origin") {
            tgt  = script::NodeTransformTarget::Translate;
            kind = script::FieldKind::Vec3;
        } else if (field == "scale") {
            tgt  = script::NodeTransformTarget::Scale;
            kind = script::FieldKind::Vec3;
        } else if (field == "angles") {
            tgt  = script::NodeTransformTarget::Rotation;
            kind = script::FieldKind::Vec3;
        } else if (field == "visible") {
            // Side-effect-only script bound to visibility. update() may
            // drive other layers via createLayer + property writes; we
            // don't write a return value back to the node.
            kind         = script::FieldKind::Bool;
            has_actuator = false;
        } else if (field == "alpha") {
            kind     = script::FieldKind::Scalar;
            is_alpha = true;
        } else if (field == "color") {
            kind     = script::FieldKind::Vec3;
            is_color = true;
        } else {
            // text/rate/intensity/... are wired elsewhere or not yet supported.
            continue;
        }
        std::string                  sha = utils::genSha1(std::span<const char>(sb.source));
        std::vector<owe::SceneNode*> clones;
        if (unsigned n = DetectAudioFanoutCapacity(sb.source); n > 1) {
            clones = SpawnLayerClones(context, node, n - 1);
        }
        auto  props         = ScriptPropertiesForField(context, field, sb);
        auto  initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto* fs =
            rt.MakeFieldScript(sb.source, sha, kind, props, initial_value, node, std::move(clones));
        if (! fs) continue;
        SetScriptInitializationOrder(context, *fs, node);
        if (sb.source.find("createLayer") != std::string_view::npos &&
            sb.source.find("registerAsset") != std::string_view::npos) {
            context.create_layer_asset_requests.push(
                { fs,
                  node->ID().to_primitive(),
                  String::make(rstd::cppstd::as_str(sb.source).unwrap()) });
        }
        if (! has_actuator) continue;
        if (is_alpha)
            ss.AddActuator({ fs, script::MakeNodeAlphaApply(node_sp.clone()) });
        else if (is_color)
            ss.AddActuator({ fs, script::MakeNodeColorApply(node_sp.clone()) });
        else if (field == "origin" && origin_apply)
            ss.AddActuator({ fs, origin_apply });
        else if (field == "scale" && scale_apply)
            ss.AddActuator({ fs, scale_apply });
        else
            ss.AddActuator({ fs, script::MakeNodeTransformApply(node_sp.clone(), tgt) });
    }
}

void WireCameraShakeScripts(ParseContext& context, const wpscene::FieldBindings& fb) {
    if (fb.scripts.empty()) return;

    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::FieldKind kind = script::FieldKind::Scalar;
        if (field == "camerashake") {
            kind = script::FieldKind::Bool;
        } else if (field != "camerashakeamplitude" && field != "camerashakespeed" &&
                   field != "camerashakeroughness") {
            continue;
        }

        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = rt.MakeFieldScript(sb.source, sha, kind, sb.properties, sb.initial_value);
        if (! fs) continue;

        auto state = mut_ref<WPUniformSceneState>::from_raw_parts(context.uniform_state.as_ptr());
        ss.AddActuator({ fs, [state, field](const script::ScriptValue& value) mutable {
                            auto scalar = ScriptValueAsFloat(value);
                            if (! scalar) return;
                            auto& shake = state->CameraShake();
                            if (field == "camerashake")
                                shake.enable = *scalar >= 0.5f;
                            else if (field == "camerashakeamplitude")
                                shake.amplitude = *scalar;
                            else if (field == "camerashakespeed")
                                shake.speed = *scalar;
                            else if (field == "camerashakeroughness")
                                shake.roughness = *scalar;
                        } });
    }
}

void WireCameraFieldScripts(ParseContext& context, const Arc<SceneNode>& node_sp,
                            const Arc<SceneCamera>& camera, const Arc<SceneCameraPath>& camera_path,
                            const wpscene::FieldBindings& fb, const Vector3f& translate_bias,
                            const Vector3f& rotation_bias) {
    SceneNode* node = node_sp.as_ptr();
    if (fb.scripts.empty()) return;
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::FieldKind kind = script::FieldKind::Vec3;
        if (field == "visible") {
            kind = script::FieldKind::Bool;
        } else if (field != "origin" && field != "angles") {
            continue;
        }

        std::string sha           = utils::genSha1(std::span<const char>(sb.source));
        auto        initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto* fs = rt.MakeFieldScript(sb.source, sha, kind, sb.properties, initial_value, node);
        if (! fs) continue;
        SetScriptInitializationOrder(context, *fs, node);

        if (field == "origin") {
            auto path         = CopyableArcHold(camera_path.clone());
            auto camera_owner = CopyableArcHold(camera.clone());
            ss.AddActuator(
                { fs, [node, camera_owner, path, translate_bias](const script::ScriptValue& value) {
                     Vector3f current = path.value->origin_base;
                     auto     next    = ScriptValueAsVec3(value, current);
                     if (next) {
                         path.value->origin_base = *next;
                         node->SetTranslate(translate_bias + *next);
                         camera_owner.value->Update();
                     }
                 } });
        } else if (field == "angles") {
            auto path         = CopyableArcHold(camera_path.clone());
            auto camera_owner = CopyableArcHold(camera.clone());
            ss.AddActuator(
                { fs, [node, camera_owner, path, rotation_bias](const script::ScriptValue& value) {
                     constexpr float kRadToDeg = 180.0f / rstd::f32::consts::PI.to_primitive();
                     constexpr float kDegToRad = rstd::f32::consts::PI.to_primitive() / 180.0f;
                     Vector3f        current   = path.value->rotation_base;
                     current *= kRadToDeg;
                     auto next = ScriptValueAsVec3(value, current);
                     if (next) {
                         path.value->rotation_base = *next * kDegToRad;
                         node->SetRotation(rotation_bias + *next * kDegToRad);
                         camera_owner.value->Update();
                     }
                 } });
        }
    }
}

// SceneObjectVar is exported from :scene_stages.

namespace
{
// mapRate < 1.0
void GenCardMesh(SceneMesh& mesh, const std::array<float, 2> size,
                 const std::array<float, 2> mapRate         = { 1.0f, 1.0f },
                 const Vector3f&            position_offset = Vector3f::Zero()) {
    float left   = -(size[0] / 2.0f) + position_offset.x();
    float right  = size[0] / 2.0f + position_offset.x();
    float bottom = -(size[1] / 2.0f) + position_offset.y();
    float top    = size[1] / 2.0f + position_offset.y();
    float z      = 0.0f;

    float tw = mapRate[0], th = mapRate[1];

    // clang-format off
	const rstd::array<float, 12> pos = {
		left,  top, z,
		left, bottom, z,
		right,  top, z,
		right, bottom, z,
	};
	const rstd::array<float, 8> texCoord = {
		0.0f, 0.0f,
		0.0f, th,
		tw, 0.0f,
		tw, th,
	};
    // clang-format on

    SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord }), usize(4));
    vertex.SetVertex(as_string_view(WE_IN_POSITION), pos.as_slice());
    vertex.SetVertex(as_string_view(WE_IN_TEXCOORD), texCoord.as_slice());
    mesh.AddVertexArray(std::move(vertex));
}

void SetParticleMesh(SceneMesh& mesh, uint32_t count, bool thick_format) {
    std::vector<VertexAttrSpec> specs {
        VAttr::Position,
        VAttr::TexCoordVec4,
        VAttr::Color,
    };
    if (thick_format) specs.push_back(VAttr::TexCoordVec4C1);
    mesh.SetPrimitive(MeshPrimitive::POINT);
    mesh.AddVertexArray(SceneVertexArray(MakeAttrSet(specs), usize(count)));
    mesh.GetVertexArray(usize(0)).SetOption(as_string_view(WE_CB_THICK_FORMAT), thick_format);
}

bool IsLayerCompositeShader(std::string_view shader) {
    return shader == "genericimage" || shader == "genericimage2" || shader == "genericimage3" ||
           shader == "genericimage4" || shader == "passthrough";
}

// TODO: Confirm WE's exact semantics for zero-height audio-buffer layers.
std::int32_t NonZeroRenderTargetDimension(float value) {
    if (! std::isfinite(value) || value < 1.0f) return 1;
    return static_cast<std::int32_t>(value);
}

std::array<std::int32_t, 2> NonZeroRenderTargetExtent(float width, float height) {
    return { NonZeroRenderTargetDimension(width), NonZeroRenderTargetDimension(height) };
}

std::array<float, 2> ImageEffectTargetSize(const ParseContext&         context,
                                           const wpscene::ImageObject& obj) {
    auto camera = context.scene->ActiveCamera();
    if (obj.fullscreen && camera.is_some()) {
        return { static_cast<float>((**camera).Width()), static_cast<float>((**camera).Height()) };
    }
    return { obj.size[0], obj.size[1] };
}

void SetRopeParticleMesh(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                         bool thick_format) {
    (void)particle;
    std::vector<VertexAttrSpec> specs {
        VAttr::PositionVec4,
        VAttr::TexCoordVec4,
        VAttr::TexCoordVec4C1,
    };
    if (thick_format) {
        specs.push_back(VAttr::TexCoordVec4C2);
        specs.push_back(VAttr::TexCoordVec4C3);
    } else {
        specs.push_back(VAttr::TexCoordVec3C2);
    }
    specs.push_back(VAttr::Color);
    mesh.SetPrimitive(MeshPrimitive::POINT);
    mesh.AddVertexArray(SceneVertexArray(MakeAttrSet(specs), usize(count)));
    mesh.GetVertexArray(usize(0)).SetOption(as_string_view(WE_PRENDER_ROPE), true);
    mesh.GetVertexArray(usize(0)).SetOption(as_string_view(WE_CB_THICK_FORMAT), thick_format);
}

struct ParticleRenderDesc {
    bool rope { false };
    bool trail { false };
};

ParticleRenderDesc DescribeParticleRender(const wpscene::ParticleRender& render) {
    ParticleRenderDesc desc;
    desc.rope  = render.name == "rope";
    desc.trail = send_with(render.name, "trail");
    return desc;
}

WPParticleAnimationMode ToAnimMode(const std::string& str) {
    if (str == "randomframe")
        return WPParticleAnimationMode::RANDOMONE;
    else if (str == "sequence")
        return WPParticleAnimationMode::SEQUENCE;
    else {
        return WPParticleAnimationMode::SEQUENCE;
    }
}

struct WPParticleOverrideControl {
    Arc<wpscene::ParticleInstanceoverride> state;
    String                                 field;

    void Apply(slice<float> values) {
        auto write_scalar = [&](float& destination) {
            if (values.len() >= usize(1)) destination = values[usize()];
        };
        auto write_vec3 = [&](std::array<float, 3>& destination, float scale) -> bool {
            if (values.len() < usize(3)) return false;
            destination = { values[usize()] * scale,
                            values[usize(1)] * scale,
                            values[usize(2)] * scale };
            return true;
        };

        auto field_view = as_string_view(field.as_str());
        if (field_view == "alpha")
            write_scalar(state->alpha);
        else if (field_view == "size")
            write_scalar(state->size);
        else if (field_view == "lifetime")
            write_scalar(state->lifetime);
        else if (field_view == "rate")
            write_scalar(state->rate);
        else if (field_view == "speed")
            write_scalar(state->speed);
        else if (field_view == "count")
            write_scalar(state->count);
        else if (field_view == "brightness")
            write_scalar(state->brightness);
        else if (field_view == "color") {
            write_vec3(state->color, 255.0f);
            state->overColor = true;
        } else if (field_view == "colorn") {
            write_vec3(state->colorn, 1.0f);
            state->overColorn = true;
        } else if (field_view.starts_with("controlpoint") &&
                   ! field_view.starts_with("controlpointangle")) {
            try {
                int index = std::stoi(
                    std::string(field_view.substr(std::string_view("controlpoint").size())));
                if (index >= 0 && index < 8) {
                    std::array<float, 3> point {};
                    if (write_vec3(point, 1.0f)) state->controlpoint[index] = point;
                }
            } catch (...) {
            }
        } else if (field_view.starts_with("controlpointangle")) {
            try {
                int index = std::stoi(
                    std::string(field_view.substr(std::string_view("controlpointangle").size())));
                if (index >= 0 && index < 8) write_vec3(state->controlpointangle[index], 1.0f);
            } catch (...) {
            }
        }
    }
};

void LoadControlPoint(WPParticleSubSystem& system, const wpscene::Particle& particle,
                      Arc<wpscene::ParticleInstanceoverride> instance_override) {
    auto points = system.ControlpointsMut();
    auto count  = rstd::cmp::min(points.len(), usize(particle.controlpoints.size()));
    for (usize index {}; index < count; ++index) {
        auto source_index         = index.to_primitive();
        points[index].base_offset = Eigen::Vector3d {
            array_cast<double>(particle.controlpoints[source_index].offset).data()
        };
        points[index].offset     = points[index].base_offset;
        points[index].link_mouse = particle.controlpoints[source_index]
                                       .flags[wpscene::ParticleControlpoint::FlagEnum::link_mouse];
        points[index].worldspace = particle.controlpoints[source_index]
                                       .flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
    }
    system.SetInstanceOverride(instance_override.clone());
    if (! instance_override->field_bindings) return;
    for (usize index {}; index < points.len(); ++index) {
        auto field = std::string("controlpointangle") + std::to_string(index.to_primitive());
        auto curve = instance_override->field_bindings->animations.find(field);
        if (curve != instance_override->field_bindings->animations.end())
            system.SetControlpointAngleCurve(index, ToSceneAnimationCurve(curve->second));
    }
}
void LoadInitializer(WPParticleSubSystem& system, const wpscene::Particle& particle,
                     Arc<wpscene::ParticleInstanceoverride> over_state) {
    u32 implicit_sequence_count { 2 };
    for (const auto& emitter : particle.emitters) {
        if (emitter.max_emit_per_period > u32()) {
            implicit_sequence_count = emitter.max_emit_per_period;
            break;
        }
    }
    for (const auto& initializer : particle.initializers) {
        auto instruction = WPParticleParser::GenInitializer(initializer, implicit_sequence_count);
        auto count       = instruction.SequenceCount();
        if (count.is_some()) system.SetRopeSequenceCount(*count);
        system.AddInitializer(rstd::move(instruction));
    }
    if (over_state->enabled) {
        system.AddInitializer(WPParticleParser::GenOverride(rstd::move(over_state)));
    }
}
void LoadOperator(WPParticleSubSystem& system, const wpscene::Particle& particle,
                  Arc<wpscene::ParticleInstanceoverride> over_state) {
    usize index {};
    for (const auto& operation : particle.operators) {
        system.AddOperator(
            WPParticleParser::GenOperator(operation, over_state.clone(), system, index++));
    }
}
void LoadEmitter(WPParticleSubSystem& system, const wpscene::Particle& particle, float count) {
    usize emitter_index {};
    for (const auto& em : particle.emitters) {
        auto newEm = em;
        newEm.rate *= count;
        system.AddEmitter(WPParticleParser::GenEmitter(newEm, system, emitter_index++));
    }
}

WPParticleSubSystem::SpawnType ParseSpawnType(std::string_view str) {
    using ST = WPParticleSubSystem::SpawnType;
    ST type { ST::STATIC };
    if (str == "eventfollow") {
        type = ST::EVENT_FOLLOW;
    } else if (str == "eventspawn") {
        type = ST::EVENT_SPAWN;
    } else if (str == "eventdeath") {
        type = ST::EVENT_DEATH;
    }
    return type;
};

BlendMode ParseBlendMode(std::string_view str) {
    BlendMode bm;
    if (str == "translucent") {
        bm = BlendMode::Translucent;
    } else if (str == "additive") {
        bm = BlendMode::Additive;
    } else if (str == "alphatocoverage") {
        bm = BlendMode::AlphaToCoverage;
    } else if (str == "normal") {
        bm = BlendMode::Normal;
    } else if (str == "disabled") {
        bm = BlendMode::Disable;
    } else {
        bm = BlendMode::Normal;
        rstd_error("unknown blending: {}", str);
    }
    return bm;
}

void ApplyImageColorBlend(wpscene::Material& material, const wpscene::ImageObject& image) {
    if (image.colorBlendMode == 0) return;
    material.combos[rstd::cppstd::to_string(WE_CB_BLENDMODE)] = image.colorBlendMode;
}

ShaderValueMap NeutralColorUniforms(ShaderValueMap values) {
    values[rstd::cppstd::to_string(G_COLOR4)]     = std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f };
    values[rstd::cppstd::to_string(G_COLOR)]      = std::array<float, 3> { 1.0f, 1.0f, 1.0f };
    values[rstd::cppstd::to_string(G_ALPHA)]      = 1.0f;
    values[rstd::cppstd::to_string(G_USERALPHA)]  = 1.0f;
    values[rstd::cppstd::to_string(G_BRIGHTNESS)] = 1.0f;
    return values;
}

std::int32_t CountVisibleImageEffects(std::span<const wpscene::ImageEffect> effects) {
    std::int32_t count = 0;
    for (const auto& effect : effects) {
        if (effect.visible || ! effect.visible_user.empty()) ++count;
    }
    return count;
}

bool ParseEnabled(std::string_view str) { return str == "enabled"; }

CullMode ParseCullMode(std::string_view str) {
    if (str == "back" || str == "normal") return CullMode::Back;
    if (str == "front") return CullMode::Front;
    if (str == "nocull" || str == "none" || str.empty()) return CullMode::None;
    rstd_error("unknown cullmode: {}", str);
    return CullMode::None;
}

void ParseSpecTexName(std::string& name, const wpscene::Material& wpmat, const WPShaderInfo& sinfo,
                      Scene& scene) {
    auto text = as_str(name).unwrap();
    if (IsSpecTex(text)) {
        if (text == WE_FULL_FRAME_BUFFER) {
            name = rstd::cppstd::to_string(SpecTex_Default);
            if (wpmat.shader == "genericimage2" &&
                ! exists(sinfo.combos, as_string_view(WE_CB_BLENDMODE)))
                name = "";
            /*
            if(wpmat.shader == "genericparticle") {
                name = "_rt_ParticleRefract";
            }
            */
        } else if (auto wpid = ParseImageLayerCompositeId(text)) {
            rstd_info("link tex \"{}\"", name);
            name = GenLinkTex(*wpid);
        } else if (rstd::str_::starts_with(text, WE_MIP_MAPPED_FRAME_BUFFER)) {
        } else if (rstd::str_::starts_with(text, WE_SHADOW_ATLAS_PREFIX)) {
            name.clear();
        } else if (rstd::str_::starts_with(text, OWE_BLOOM_MIP_PREFIX)) {
        } else if (rstd::str_::starts_with(text, WE_REFLECTION_PREFIX)) {
            name = rstd::cppstd::to_string(WE_REFLECTION_PREFIX);
            scene.EnablePlanarReflection();
        } else if (rstd::str_::starts_with(text, OWE_EFFECT_PPONG_PREFIX)) {
        } else if (rstd::str_::starts_with(text, WE_HALF_COMPO_BUFFER_PREFIX)) {
        } else if (rstd::str_::starts_with(text, WE_QUARTER_COMPO_BUFFER_PREFIX)) {
        } else if (rstd::str_::starts_with(text, WE_FULL_COMPO_BUFFER_PREFIX)) {
        } else if (rstd::str_::starts_with(text, WE_EIGHT_COMPO_BUFFER_PREFIX)) {
        } else if (rstd::str_::starts_with(text, WE_VOLUMETRICS_PREFIX) ||
                   rstd::str_::starts_with(text, WE_QUARTER_FORCE_RG_PREFIX) ||
                   rstd::str_::starts_with(text, WE_BLOOM_PREFIX) ||
                   rstd::str_::starts_with(text, WE_QUARTER_FRAME_BUFFER_PREFIX) ||
                   rstd::str_::starts_with(text, WE_EIGHTH_FRAME_BUFFER_PREFIX)) {
            name.clear();
        } else if (scene.RenderTarget(as_str(name).unwrap()).is_some()) {
            // an effect-local fbo registered with a non-conventional name
            // (e.g. WE DOF's `_rt__coc_<addr>`) — already a valid RT.
        } else {
            rstd_warn("ignoring unsupported special tex \"{}\"", name);
            name.clear();
        }
    }
}

SceneShaderTextureCompileInfo ToSceneShaderTextureCompileInfo(const WPShaderTexInfo& info) {
    return SceneShaderTextureCompileInfo {
        .enabled = info.enabled,
        .components =
            array<bool, 3> { info.composEnabled[0], info.composEnabled[1], info.composEnabled[2] },
    };
}

owe::Map<std::string, std::string> MaterialCombosToShaderCombos(const wpscene::Material& material) {
    owe::Map<std::string, std::string> combos;
    for (const auto& [key, value] : material.combos) combos[key] = std::to_string(value);
    return combos;
}

bool IsLegacyAtmosphereMaterial(const wpscene::Material& material) {
    return material.shader == "workshop/2839476907/effects/atmosphere";
}

void ApplyLegacyAtmosphereLightCombo(const wpscene::Material& material, WPShaderInfo& info) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    if (! info.combos.contains("LIGHT_INDEX") || material.combos.contains("LIGHT_INDEX")) return;
    if (! material.combos.contains("LIGHT1")) return;

    info.combos["LIGHT_INDEX"] = "4";
}

void ApplySceneFogCombos(const WPSceneShaderEnvironment& environment, WPShaderInfo& info) {
    auto fog = info.combos.find("FOG");
    if (fog == info.combos.end() || fog->second == "0") return;

    if (environment.fog_distance) info.combos["FOG_DIST"] = "1";
    if (environment.fog_height) info.combos["FOG_HEIGHT"] = "1";
    if (environment.fog_distance || environment.fog_height) info.combos["FOG_COMPUTED"] = "1";
}

void ApplyLegacyAtmosphereUniformAliases(const wpscene::Material& material, WPShaderInfo& info) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    info.baseConstSvs[rstd::cppstd::to_string(G_VIEWFORWARD)] = std::array { 0.0f, 0.0f, 1.0f };

    auto prefer_legacy = [&](std::string_view legacy, std::string_view current) {
        if (! material.constantshadervalues.contains(std::string(legacy))) return;
        auto current_it = info.alias.find(std::string(current));
        if (current_it == info.alias.end()) return;
        info.alias[std::string(legacy)] = current_it->second;
        info.alias.erase(current_it);
    };

    prefer_legacy("Planet position", "Position");
    prefer_legacy("Planet radius", "Planet size");
    prefer_legacy("Atmosphere radius", "Atmosphere size");
    prefer_legacy("Thickness", "Density falloff");
    prefer_legacy("Color", "Light color");
    prefer_legacy("Intensity", "Brightness");
}

void ReplaceAllInPlace(std::string& body, std::string_view needle, std::string_view repl) {
    for (std::size_t pos = 0; (pos = body.find(needle, pos)) != std::string::npos;
         pos += repl.size()) {
        body.replace(pos, needle.size(), repl);
    }
}

void ApplyLegacyAtmosphereShaderCompat(const wpscene::Material&   material,
                                       std::vector<WPShaderUnit>& units) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    for (auto& unit : units) {
        if (unit.stage != ShaderType::FRAGMENT) continue;
        ReplaceAllInPlace(unit.src,
                          "float pointDensity, opticalDepth;",
                          "float pointDensity = 0.0, opticalDepth = 0.0;");
        ReplaceAllInPlace(unit.src,
                          "float localDensity, cameraOpticalDepth, sunRayLength, "
                          "sunOpticalDepth, lightInstensity = 1.0;",
                          "float localDensity = 0.0, cameraOpticalDepth = 0.0, "
                          "sunRayLength = 0.0, sunOpticalDepth = 0.0, lightInstensity = 1.0;");
    }
}

bool IsLegacyAtmosphereShadowValue(const wpscene::Material& material, std::string_view name) {
    if (! IsLegacyAtmosphereMaterial(material)) return false;

    static constexpr std::string_view shadow_values[] = {
        "Position",    "Planet size", "Atmosphere size", "Density falloff",
        "Light color", "Brightness",  "Radius",
    };

    for (std::string_view shadow_value : shadow_values) {
        if (name == shadow_value) return true;
    }
    return false;
}

std::vector<SceneShaderDefaultTexture> ToSceneShaderDefaultTextures(const WPShaderInfo& info) {
    std::vector<SceneShaderDefaultTexture> out;
    out.reserve(info.defTexs.size());
    for (const auto& [slot, texture] : info.defTexs) {
        out.push_back(SceneShaderDefaultTexture { .slot = slot, .texture = texture });
    }
    return out;
}

SceneShaderVariantDesc MakeSceneShaderVariantDesc(
    std::string_view scene_id, const wpscene::Material& material, const WPShaderInfo& info,
    std::span<const WPShaderUnit> units, std::span<const std::string> source_keys,
    std::span<const std::string> stage_sources, std::span<const WPShaderTexInfo> texinfos,
    bool geometry_shader_enabled) {
    SceneShaderVariantDesc desc;
    desc.scene_id                = std::string(scene_id);
    desc.shader_name             = material.shader;
    desc.input_combos            = MaterialCombosToShaderCombos(material);
    desc.resolved_combos         = info.combos;
    desc.uniform_aliases         = info.alias;
    desc.default_uniforms        = info.svs;
    desc.default_textures        = ToSceneShaderDefaultTextures(info);
    desc.geometry_shader_enabled = geometry_shader_enabled;

    desc.texture_infos.reserve(texinfos.size());
    for (const auto& texinfo : texinfos) {
        desc.texture_infos.push_back(ToSceneShaderTextureCompileInfo(texinfo));
    }

    desc.stages.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        desc.stages.push_back(SceneShaderVariantStage {
            .stage      = units[i].stage,
            .source_key = i < source_keys.size() ? source_keys[i] : std::string {},
            .source     = i < stage_sources.size() ? stage_sources[i] : units[i].src,
        });
    }
    return desc;
}

enum class GeometryStageRequirement
{
    None,
    Required,
};

bool LoadMaterial(fs::VFS& vfs, Option<ref<rstd::path::Path>> shader_cache_dir,
                  const WPSceneShaderEnvironment& environment, const wpscene::Material& wpmat,
                  Scene* pScene, SceneMaterial* pMaterial, WPShaderInfo* pWPShaderInfo = nullptr,
                  GeometryStageRequirement geometry_stage = GeometryStageRequirement::None) {
    auto& material   = *pMaterial;
    auto  blend_mode = ParseBlendMode(wpmat.blending);

    std::unique_ptr<WPShaderInfo> upWPShaderInfo(nullptr);
    if (pWPShaderInfo == nullptr) {
        upWPShaderInfo = std::make_unique<WPShaderInfo>();
        pWPShaderInfo  = upWPShaderInfo.get();
    }

    SceneMaterialCustomShader materialShader;

    auto& shader              = materialShader.shader;
    shader                    = std::make_shared<SceneShader>();
    shader->name              = wpmat.shader;
    shader->matrix_convention = ShaderMatrixConvention::RowVector;
    shader->matrix_abi        = ShaderMatrixAbi::Hlsl;
    std::string shaderPath("/assets/shaders/" + wpmat.shader);

    std::vector<WPShaderUnit> sd_units;
    std::vector<std::string>  sd_source_keys;
    std::vector<std::string>  sd_original_sources;
    auto                      add_shader_unit = [&](ShaderType stage, std::string source_key) {
        auto        loaded = fs::ReadFileContent(vfs, source_key);
        std::string source;
        if (loaded.is_ok()) {
            source = rstd::move(loaded).unwrap_unchecked();
        } else {
            rstd_error("Can't read shader source {}", source_key);
        }
        sd_source_keys.push_back(std::move(source_key));
        sd_original_sources.push_back(source);
        sd_units.push_back({
            .stage           = stage,
            .src             = std::move(source),
            .preprocess_info = {},
        });
    };
    add_shader_unit(ShaderType::VERTEX, shaderPath + ".vert");
    bool has_geometry_stage = geometry_stage == GeometryStageRequirement::Required;
    if (has_geometry_stage) {
        std::string geom_path = shaderPath + ".geom";
        if (vfs.metadata(fs::ToPath(geom_path)).is_err()) {
            rstd_error("required geometry shader source missing: {}", geom_path);
            return false;
        }
        add_shader_unit(ShaderType::GEOMETRY, std::move(geom_path));
        pWPShaderInfo->combos[rstd::cppstd::to_string(WE_CB_GS_ENABLED)] = "1";
    }
    add_shader_unit(ShaderType::FRAGMENT, shaderPath + ".frag");

    std::vector<WPShaderTexInfo>                 texinfos;
    std::unordered_map<std::string, ImageHeader> texHeaders;
    for (const auto& el : wpmat.textures) {
        if (el.empty()) {
            texinfos.push_back({ false });
        } else if (! IsSpecTex(as_str(el).unwrap())) {
            auto parsed_header = pScene->ParseImageHeader(rstd::cppstd::as_str(el).unwrap());
            auto texh      = parsed_header.is_ok() ? rstd::move(parsed_header).unwrap_unchecked()
                                                   : ImageHeader {};
            texHeaders[el] = texh;
            if (texh.extraHeader.count("compo1") == 0) {
                texinfos.push_back({ false });
                continue;
            }
            texinfos.push_back({ true,
                                 {
                                     (bool)texh.extraHeader.at("compo1").val,
                                     (bool)texh.extraHeader.at("compo2").val,
                                     (bool)texh.extraHeader.at("compo3").val,
                                 } });
        } else
            texinfos.push_back({ true });
    }

    for (auto& unit : sd_units) {
        unit.src = WPShaderParser::PreShaderSrc(vfs, unit.src, pWPShaderInfo, texinfos);
    }
    ApplyLegacyAtmosphereUniformAliases(wpmat, *pWPShaderInfo);
    ApplyLegacyAtmosphereShaderCompat(wpmat, sd_units);

    for (const auto& el : wpmat.combos) {
        pWPShaderInfo->combos[el.first] = std::to_string(el.second);
    }
    if (blend_mode == BlendMode::AlphaToCoverage) {
        pWPShaderInfo->combos["ALPHATOCOVERAGE"] = "1";
    }
    ApplySceneFogCombos(environment, *pWPShaderInfo);
    ApplyLegacyAtmosphereLightCombo(wpmat, *pWPShaderInfo);

    auto textures = wpmat.textures;
    if (pWPShaderInfo->defTexs.size() > 0) {
        for (auto& t : pWPShaderInfo->defTexs) {
            const auto index = static_cast<std::size_t>(t.first);
            if (textures.size() > index) {
                if (! textures.at(index).empty()) continue;
            } else {
                textures.resize(index + 1);
            }
            textures[index] = t.second;
        }
    }

    for (std::size_t i = 0; i < textures.size(); i++) {
        std::string name = textures.at(i);
        ParseSpecTexName(name, wpmat, *pWPShaderInfo, *pScene);
        material.textures.push_back(name);
        material.texture_metadata.emplace_back();
        material.defines.push_back("g_Texture" + std::to_string(i));
        if (name.empty()) {
            continue;
        }

        std::array<std::int32_t, 4> resolution {};
        auto                        texture_name = as_str(name).unwrap();
        if (IsSpecTex(texture_name)) {
            auto target = pScene->RenderTarget(as_str(name).unwrap());
            if (! IsSpecLinkTex(texture_name) && target.is_none()) {
                rstd_error("{} not found in render targes", name);
            } else if (target.is_some()) {
                const auto& rt = **target;
                resolution     = { rt.width, rt.height, rt.width, rt.height };
            }
        } else {
            auto texh = [&] {
                if (texHeaders.count(name) != 0) return texHeaders.at(name);
                auto parsed_header = pScene->ParseImageHeader(rstd::cppstd::as_str(name).unwrap());
                return parsed_header.is_ok() ? rstd::move(parsed_header).unwrap_unchecked()
                                             : ImageHeader {};
            }();
            if (i == 0) {
                if (texh.format == TextureFormat::R8)
                    pWPShaderInfo->combos["TEX0FORMAT"] = "FORMAT_R8";
                else if (texh.format == TextureFormat::RG8)
                    pWPShaderInfo->combos["TEX0FORMAT"] = "FORMAT_RG88";
            }
            if (texh.mipmap_larger) {
                resolution = { texh.width, texh.height, texh.mapWidth, texh.mapHeight };
            } else {
                resolution = { texh.mapWidth, texh.mapHeight, texh.mapWidth, texh.mapHeight };
            }
            material.texture_metadata.back() = SceneMaterialTextureMetadata {
                .has_extent    = true,
                .source_extent = { static_cast<float>(resolution[0]),
                                   static_cast<float>(resolution[1]) },
                .sample_extent = { static_cast<float>(resolution[2]),
                                   static_cast<float>(resolution[3]) },
            };

            auto scene_texture = pScene->Texture(rstd::cppstd::as_str(name).unwrap());
            if (scene_texture.is_none()) {
                SceneTexture stex;
                stex.sample = texh.sample;
                stex.url    = name;
                if (texh.isSprite) {
                    stex.isSprite   = texh.isSprite;
                    stex.spriteAnim = texh.spriteAnim;
                }
                pScene->RegisterTexture(String::make(rstd::cppstd::as_str(name).unwrap()),
                                        rstd::move(stex));
                scene_texture = pScene->Texture(rstd::cppstd::as_str(name).unwrap());
            }
            if (scene_texture.is_some() && (**scene_texture).isSprite) {
                material.hasSprite = true;
                const auto& f1     = texh.spriteAnim.GetCurFrame();
                if (wpmat.shader == "genericparticle" || wpmat.shader == "genericropeparticle") {
                    pWPShaderInfo->combos[rstd::cppstd::to_string(WE_CB_SPRITESHEET)]  = "1";
                    pWPShaderInfo->combos[rstd::cppstd::to_string(WE_CB_THICK_FORMAT)] = "1";
                    if (algorism::IsPowOfTwo(u32(static_cast<std::uint32_t>(texh.width))) &&
                        algorism::IsPowOfTwo(u32(static_cast<std::uint32_t>(texh.height)))) {
                        pWPShaderInfo->combos[rstd::cppstd::to_string(WE_CB_SPRITESHEETBLENDNPOT)] =
                            "1";
                        resolution[2] = resolution[0] - resolution[0] % (int)f1.width;
                        resolution[3] = resolution[1] - resolution[1] % (int)f1.height;
                        material.texture_metadata.back().sample_extent = {
                            static_cast<float>(resolution[2]), static_cast<float>(resolution[3])
                        };
                    }
                    materialShader.constValues[rstd::cppstd::to_string(G_RENDERVAR1)] =
                        std::array { f1.xAxis[0],
                                     f1.yAxis[1],
                                     static_cast<float>(texh.spriteAnim.numFrames().to_primitive()),
                                     f1.rate };
                }
            }
        }
        if (! resolution.empty()) {
            const std::string gResolution = WE_GLTEX_RESOLUTION_NAMES[i];

            materialShader.constValues[gResolution] = array_cast<float>(resolution);
        }
    }
    if (exists(pWPShaderInfo->combos, rstd::cppstd::to_string(WE_CB_LIGHTING))) {
        // pWPShaderInfo->combos["PRELIGHTING"] =
        // pWPShaderInfo->combos.at(rstd::cppstd::to_string(WE_CB_LIGHTING));
    }

    auto scene_id              = as_string_view(pScene->SceneId());
    auto variant_desc          = MakeSceneShaderVariantDesc(scene_id,
                                                            wpmat,
                                                            *pWPShaderInfo,
                                                            sd_units,
                                                            sd_source_keys,
                                                            sd_original_sources,
                                                            texinfos,
                                                            has_geometry_stage);
    variant_desc.texture_slots = material.textures;

    if (! WPShaderParser::CompileToSpv(
            scene_id, sd_units, shader->codes, pWPShaderInfo, texinfos, shader_cache_dir)) {
        return false;
    }
    shader->default_uniforms      = pWPShaderInfo->svs;
    variant_desc.default_uniforms = pWPShaderInfo->svs;
    WPShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(
        variant_desc, sd_units, shader->codes);
    shader->sampler_bindings = variant_desc.sampler_bindings;

    material.blenmode    = blend_mode;
    material.depth_test  = ParseEnabled(wpmat.depthtest);
    material.depth_write = ParseEnabled(wpmat.depthwrite);
    material.cull_mode   = ParseCullMode(wpmat.cullmode);

    // FS is always the last unit (VS may be followed by optional GS, then FS).
    const auto& fs_active = sd_units.back().preprocess_info.active_tex_slots;
    for (unsigned i = 0; i < material.textures.size(); i++) {
        if (! exists(fs_active, i)) material.textures[i].clear();
    }

    for (const auto& el : pWPShaderInfo->baseConstSvs) {
        materialShader.constValues[el.first] = el.second;
    }
    // Register bindings only after AddMaterial places the material in its
    // stable mesh-owned allocation. Registering the stack-local pointer here
    // would leave a dangling binding after the move.
    for (const auto& var : pWPShaderInfo->scalar_uniforms) {
        if (! var.is_user || var.material.is_empty()) continue;
        auto uniform_name = rstd::cppstd::to_string(var.name.as_str());
        pWPShaderInfo->user_var_staging.push(UserVarRecord {
            .material      = var.material.clone(),
            .name          = var.name.clone(),
            .default_value = var.default_value.clone(),
        });
        if (auto value = shader->default_uniforms.find(uniform_name);
            value != shader->default_uniforms.end()) {
            materialShader.constValues[uniform_name] = value->second;
        }
    }

    material.customShader         = rstd::move(materialShader);
    material.customShader.variant = std::move(variant_desc);
    material.name                 = wpmat.shader;

    return true;
}

std::string ResolveShaderMaterialKey(const WPShaderInfo& info, const std::string& material_key) {
    if (auto it = info.alias.find(material_key); it != info.alias.end()) return it->second;

    for (const auto& el : info.alias) {
        if (el.second.size() > 2 && el.second.substr(2) == material_key) return el.second;
    }
    return {};
}

bool IsShaderPositionUniform(const WPShaderInfo& info, const std::string& glname) {
    for (const auto& var : info.scalar_uniforms) {
        if (var.name == rstd::cppstd::as_str(glname).unwrap()) return var.position;
    }
    return false;
}

bool UsesEffectPositionSpace(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/spin" && wpmat.shader != "effects/transform") return false;
    auto mode_it = wpmat.combos.find("MODE");
    return mode_it != wpmat.combos.end() && mode_it->second == 1;
}

bool UsesUnitFinalQuad(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/transform") return false;
    auto mode_it = wpmat.combos.find("MODE");
    return mode_it != wpmat.combos.end() && mode_it->second == 1;
}

bool CanCompositeFinalEffectShader(std::string_view shader) {
    return IsLayerCompositeShader(shader) || shader == "effects/transform" ||
           shader == "effects/scroll" || shader == "effects/spin" ||
           shader == "effects/perspective" || shader == "effects/foliagesway" ||
           shader == "effects/blend" || shader == "effects/tint";
}

bool HasShaderCombo(const WPShaderInfo& info, std::string_view combo_name) {
    auto name = rstd::cppstd::as_str(combo_name).unwrap();
    for (const auto& combo : info.combo_defs)
        if (combo.combo == name) return true;
    return false;
}

bool HasShaderTextureMaterial(const WPShaderInfo& info, std::string_view material_key) {
    auto key = rstd::cppstd::as_str(material_key).unwrap();
    for (const auto& texture : info.texture_uniforms)
        if (texture.material == key) return true;
    return false;
}

bool HasSolidCompositeContext(const ParseContext& context, const wpscene::ImageObject& obj) {
    if (obj.solid || context.solid_layer_ids.contains(obj.id)) return true;

    std::unordered_set<std::int32_t> seen;
    std::uint32_t                    parent = obj.parent;
    while (parent != 0 && seen.insert(static_cast<std::int32_t>(parent)).second) {
        const auto parent_id = static_cast<std::int32_t>(parent);
        if (context.solid_layer_ids.contains(parent_id)) return true;

        auto found = context.object_parent_ids.get(parent_id);
        if (found.is_none()) break;
        parent = **found;
    }

    return false;
}

bool CanCompositeFinalEffectMaterial(std::string_view shader, const WPShaderInfo& info,
                                     bool allow_transparent_previous) {
    if (CanCompositeFinalEffectShader(shader)) return true;
    if (! allow_transparent_previous) return false;

    // TODO: WE does not document this as the final-composite rule. This keeps
    // the historical shortcut only for non-solid layer contexts.
    return HasShaderCombo(info, "TRANSPARENCY") && HasShaderTextureMaterial(info, "previous");
}

void NormalizeEffectPositionCurve(SceneAnimationCurve& curve) {
    auto normalize_axis = [&](Vec<SceneAnimationKey>& keys) {
        for (auto& key : keys) {
            key.value = curve.relative ? key.value * 2.0f : key.value * 2.0f - 1.0f;
        }
    };
    normalize_axis(curve.c0);
    normalize_axis(curve.c1);
}

// Register a (material, shader-info, wpmat) triple into the scene-wide user
// variable index. Must be called after SceneMaterial has entered its stable
// mesh-owned allocation. Wires up:
//   (1) Direct-route u_* whose shader annotation's `material` field is the
//       wallpaper-level project.json key (the legacy convention).
//   (2) Instance-bound effect-internal keys from
//       `wpmat.constantshadervalues_user`, mapped through `info.alias` to
//       the GLSL uniform name.
//   (3) Legacy material `usershadervalues` bindings: project key to shader
//       material key.
void RegisterShaderUserVarIndex(Scene* pScene, SceneMaterial* stable_mat,
                                const wpscene::Material& wpmat, const WPShaderInfo& info) {
    if (! pScene || ! stable_mat) return;
    for (const auto& combo : info.combo_defs) {
        if (combo.material.is_empty() || combo.combo.is_empty()) continue;
        Scene::ShaderComboUserBinding binding {
            .material = stable_mat,
            .combo    = combo.combo.clone(),
            .fallback =
                String::make(as_str(std::to_string(combo.default_.to_primitive())).unwrap()),
        };
        combo.options.iter().for_each([&](auto entry) {
            auto [label, value] = entry;
            (void)binding.options.insert(
                label->clone(),
                String::make(as_str(std::to_string(value->to_primitive())).unwrap()));
        });
        pScene->RegisterShaderComboUserBinding(combo.material.clone(), rstd::move(binding));
    }
    for (const auto& rec : info.user_var_staging) {
        pScene->RegisterShaderUserBinding(rec.material.clone(), *stable_mat, rec.name.clone());
    }
    for (const auto& [effect_key, wallpaper_key] : wpmat.constantshadervalues_user) {
        // Resolve effect-internal key → GLSL uniform name via alias.
        // LoadConstvalue's fallback search (alias entry whose value, after
        // dropping the leading "u_", matches the key) is honored here too.
        std::string glname = ResolveShaderMaterialKey(info, effect_key);
        if (glname.empty()) {
            rstd_warn("user binding '{}' → no shader uniform with material='{}'",
                      wallpaper_key,
                      effect_key);
            continue;
        }
        pScene->RegisterShaderUserBinding(String::make(as_str(wallpaper_key).unwrap()),
                                          *stable_mat,
                                          String::make(as_str(glname).unwrap()));
    }
    for (const auto& [wallpaper_key, material_key] : wpmat.user_shader_values) {
        std::string glname = ResolveShaderMaterialKey(info, material_key);
        if (glname.empty()) {
            rstd_warn("user shader value '{}' -> no shader uniform with material='{}'",
                      wallpaper_key,
                      material_key);
            continue;
        }
        pScene->RegisterShaderUserBinding(String::make(as_str(wallpaper_key).unwrap()),
                                          *stable_mat,
                                          String::make(as_str(glname).unwrap()));
    }
}

std::optional<std::string> UserTexturePropertyKey(const Json& binding) {
    if (binding.is_string()) {
        auto key = rstd::cppstd::to_string(*binding.as_str());
        if (key.empty()) return std::nullopt;
        return key;
    }
    if (! binding.is_object()) return std::nullopt;
    auto type  = binding.get("type"_str);
    auto value = binding.get("name"_str);
    if (type.is_none() || value.is_none()) return std::nullopt;
    auto type_string  = (*type)->as_str();
    auto value_string = (*value)->as_str();
    if (type_string.is_none() || value_string.is_none() ||
        rstd::cppstd::as_string_view(*type_string) != "system")
        return std::nullopt;
    auto name = rstd::cppstd::as_string_view(*value_string);
    if (name != "$mediaThumbnail" && name != "$mediaPreviousThumbnail") return std::nullopt;
    return std::string(name);
}

bool IsSystemMediaTextureBinding(const Json& binding) {
    return UserTexturePropertyKey(binding).has_value() && binding.is_object();
}

std::string ResolveMaterialTextureFallback(Scene& scene, const wpscene::Material& fallback_material,
                                           const WPShaderInfo& shader_info, usize slot) {
    std::string fallback;
    if (slot.to_primitive() < fallback_material.textures.size()) {
        fallback = fallback_material.textures[slot.to_primitive()];
    }
    if (fallback.empty()) {
        for (const auto& [index, texture] : shader_info.defTexs) {
            if (usize(static_cast<std::size_t>(index)) == slot) {
                fallback = texture;
                break;
            }
        }
    }
    ParseSpecTexName(fallback, fallback_material, shader_info, scene);
    return fallback;
}

void RegisterMaterialUserTextureIndex(Scene* pScene, SceneMaterial* stable_mat,
                                      const wpscene::Material& fallback_material,
                                      const WPShaderInfo&      shader_info) {
    if (! pScene || ! stable_mat) return;
    for (usize i {}; i < fallback_material.usertextures.len(); ++i) {
        auto key = UserTexturePropertyKey(fallback_material.usertextures[i]);
        if (! key.has_value()) continue;
        std::string fallback =
            ResolveMaterialTextureFallback(*pScene, fallback_material, shader_info, i);
        if (IsSystemMediaTextureBinding(fallback_material.usertextures[i]) &&
            i.to_primitive() < stable_mat->textures.size()) {
            fallback = stable_mat->textures[i.to_primitive()];
        }
        pScene->RegisterMaterialTextureUserBinding(
            String::make(as_str(*key).unwrap()),
            Scene::MaterialTextureUserBinding {
                .material = stable_mat,
                .slot     = u32(static_cast<uint32_t>(i.to_primitive())),
                .fallback = String::make(as_str(fallback).unwrap()),
            });
    }
}

Vector3f AlignmentOffset(ref<str> align, Vector2f size) {
    Vector3f offset = Vector3f::Zero();
    size *= 0.5f;
    size.y() *= 1.0f;

    // topleft top center ...
    if (rstd::str_::contains(align, "top"_str)) offset.y() -= size.y();
    if (rstd::str_::contains(align, "left"_str)) offset.x() += size.x();
    if (rstd::str_::contains(align, "right"_str)) offset.x() -= size.x();
    if (rstd::str_::contains(align, "bottom"_str)) offset.y() += size.y();

    return offset;
}

// Apply effect-pass `bind` overrides onto wpmat.textures by index, using
// fboMap to resolve effect-local FBO names to actual scene RT keys.
void ApplyTextureBinds(wpscene::Material&                                  wpmat,
                       std::span<const wpscene::MaterialPassBindItem>      binds,
                       const std::unordered_map<std::string, std::string>& fboMap) {
    for (const auto& el : binds) {
        if (fboMap.count(el.name) == 0) {
            rstd_error("fbo {} not found", el.name);
            continue;
        }
        const auto index = static_cast<std::size_t>(el.index);
        if (wpmat.textures.size() <= index) wpmat.textures.resize(index + 1);
        wpmat.textures[index] = fboMap.at(el.name);
    }
}

std::string ResolveSceneTextureProperty(const ParseContext& context, std::string_view key) {
    if (context.user_properties.is_none()) return {};
    auto prop = (*context.user_properties)->get(rstd::cppstd::as_str(key).unwrap());
    if (prop.is_none()) return {};
    const auto& payload = **prop;
    if (payload.is_string()) {
        auto text = rstd::cppstd::to_string(*payload.as_str());
        return text.empty() ? std::string {} : text;
    }
    if (! payload.is_object()) return {};

    std::string type;
    if (auto value = payload.get("type"_str); value.is_some()) {
        auto string = (*value)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }
    if (! type.empty() && type != "scenetexture" && type != "texture" && type != "replacetexture")
        return {};
    auto value = payload.get("value"_str);
    if (value.is_none()) return {};
    auto string = (*value)->as_str();
    return string.is_none() ? std::string {} : rstd::cppstd::to_string(*string);
}

std::string ResolveUserTextureProperty(const ParseContext& context, const Json& binding) {
    if (! binding.is_string()) return {};
    auto key = rstd::cppstd::to_string(*binding.as_str());
    return ResolveSceneTextureProperty(context, key);
}

std::string ResolveMaterialTextureSlot(const ParseContext&      context,
                                       const wpscene::Material& material, usize slot) {
    std::string fallback;
    if (slot.to_primitive() < material.textures.size()) {
        fallback = material.textures[slot.to_primitive()];
    }
    if (slot >= material.usertextures.len()) return fallback;

    if (auto prop = ResolveUserTextureProperty(context, material.usertextures[slot]);
        ! prop.empty())
        return prop;
    return fallback;
}

bool CanUseImageAsSystemMediaFallback(const wpscene::ImageObject& image) {
    if (! image.puppet.empty()) return false;
    if (image.fullscreen || image.config.passthrough) return false;
    return CountVisibleImageEffects(image.effects) == 0;
}

std::string ResolveLinkedImageFallback(const ParseContext& context, std::string_view texture) {
    auto                         name      = as_str(texture).unwrap();
    std::optional<std::uint32_t> linked_id = ParseImageLayerCompositeId(name);
    if (! linked_id && IsSpecLinkTex(name)) {
        linked_id = ParseLinkTex(name).to_primitive();
    }
    if (! linked_id) return {};

    auto fallback = context.system_media_image_fallbacks.get(static_cast<std::int32_t>(*linked_id));
    return fallback.is_some() ? rstd::cppstd::to_string((**fallback).as_str()) : std::string {};
}

std::string ResolveSystemMediaFallback(const ParseContext&      context,
                                       const wpscene::Material& material, usize slot) {
    if (slot.to_primitive() >= material.textures.size()) return {};
    return ResolveLinkedImageFallback(context, material.textures[slot.to_primitive()]);
}

void ApplyUserTextureBindings(ParseContext& context, wpscene::Material& material) {
    for (usize i {}; i < material.usertextures.len(); ++i) {
        const auto& binding = material.usertextures[i];
        if (binding.is_null()) continue;

        std::string resolved = ResolveUserTextureProperty(context, binding);
        if (resolved.empty() && IsSystemMediaTextureBinding(binding)) {
            resolved = ResolveSystemMediaFallback(context, material, i);
        }
        if (resolved.empty()) continue;

        if (material.textures.size() <= i.to_primitive()) {
            material.textures.resize(i.to_primitive() + 1);
        }
        material.textures[i.to_primitive()] = std::move(resolved);
    }
}

void IndexSystemMediaImageFallbacks(ParseContext& context, slice<SceneObjectVar> scene_objs) {
    context.system_media_image_fallbacks.clear();
    for (usize index {}; index < scene_objs.len(); ++index) {
        const auto& object = scene_objs[index];
        if (! object.is_Image()) continue;
        const auto& image = object.as_Image().value;
        if (! CanUseImageAsSystemMediaFallback(image)) continue;

        auto texture = ResolveMaterialTextureSlot(context, image.material, usize(0));
        if (texture.empty() || IsSpecTex(as_str(texture).unwrap())) continue;
        (void)context.system_media_image_fallbacks.insert(
            image.id, String::make(rstd::cppstd::as_str(texture).unwrap()));
    }
}

void LoadConstvalue(SceneMaterial& material, const wpscene::Material& wpmat,
                    const WPShaderInfo&           info,
                    SceneShaderValueAnimationMap* final_quad_shader_values = nullptr) {
    // load glname from alias and load to constvalue
    for (const auto& cs : wpmat.constantshadervalues) {
        const auto&               name   = cs.first;
        const std::vector<float>& value  = cs.second;
        std::string               glname = ResolveShaderMaterialKey(info, name);
        if (glname.empty()) {
            if (IsLegacyAtmosphereShadowValue(wpmat, name)) continue;
            rstd_error("ShaderValue: {} not found in glsl", name);
        } else {
            std::vector<float> const_value = value;
            bool               normalize_position =
                UsesEffectPositionSpace(wpmat) && IsShaderPositionUniform(info, glname);
            std::optional<SceneShaderValueAnimation> final_quad_value;
            if (normalize_position && const_value.size() >= 2) {
                final_quad_value.emplace();
                final_quad_value->base = ShaderValue(value);
                const_value[0]         = const_value[0] * 2.0f - 1.0f;
                const_value[1]         = const_value[1] * 2.0f - 1.0f;
            }
            material.SetShaderValue(
                glname,
                ShaderValue(std::span<const float>(const_value.data(), const_value.size())));
            if (auto it = wpmat.constantshadervalues_animations.find(name);
                it != wpmat.constantshadervalues_animations.end()) {
                auto curve = Arc<SceneAnimationCurve>::make(ToSceneAnimationCurve(it->second));
                if (final_quad_value) final_quad_value->curve = Some(curve.clone());
                if (normalize_position) {
                    curve = Arc<SceneAnimationCurve>::make(ToSceneAnimationCurve(it->second));
                    NormalizeEffectPositionCurve(*curve);
                }
                material.SetShaderValueAnimation(
                    String::make(rstd::cppstd::as_str(glname).unwrap()), rstd::move(curve));
            }
            if (final_quad_value && final_quad_shader_values) {
                (void)final_quad_shader_values->insert(
                    String::make(rstd::cppstd::as_str(glname).unwrap()),
                    rstd::move(*final_quad_value));
            }
        }
    }
}

// parse

void ParseCamera(ParseContext& context, const wpscene::SceneMetadata& sc) {
    auto& scene   = *context.scene;
    auto& general = sc.general;
    // effect camera
    auto effect_camera = Arc<SceneCamera>::make(SceneCamera::MakeOrthographic(2, 2, -1.0, 1.0));
    context.effect_camera_node = Some(Arc<SceneNode>::make()); // at 0,0,0
    effect_camera->AttatchNode((*context.effect_camera_node).as_ptr());
    scene.RegisterCamera(String::make("effect"_str), rstd::move(effect_camera));
    scene.RootMut()->AppendChild((*context.effect_camera_node).clone());

    // global camera
    auto     global_camera = Arc<SceneCamera>::make(SceneCamera::MakeOrthographic(
        context.ortho_w / general.zoom, context.ortho_h / general.zoom, -5000.0, 5000.0));
    Vector3f cori { (float)context.ortho_w / 2.0f, (float)context.ortho_h / 2.0f, 0 },
        cscale { 1.0f, 1.0f, 1.0f }, cangle(Vector3f::Zero());

    context.global_camera_node = Some(Arc<SceneNode>::make(cori, cscale, cangle));
    global_camera->AttatchNode((*context.global_camera_node).as_ptr());
    scene.RegisterCamera(String::make("global"_str), rstd::move(global_camera));
    (void)scene.SetActiveCamera("global"_str);
    scene.RootMut()->AppendChild((*context.global_camera_node).clone());

    auto perspective_camera = Arc<SceneCamera>::make(
        SceneCamera::MakePerspective(static_cast<double>(context.ortho_w) / context.ortho_h,
                                     general.nearz,
                                     general.farz,
                                     algorism::CalculatePersperctiveFov(1000.0f, context.ortho_h)));

    Vector3f cperori                       = cori;
    cperori[2]                             = 1000.0f;
    context.global_perspective_camera_node = Some(Arc<SceneNode>::make(cperori, cscale, cangle));
    perspective_camera->AttatchNode((*context.global_perspective_camera_node).as_ptr());
    scene.RegisterCamera(String::make("global_perspective"_str), perspective_camera.clone());
    scene.RootMut()->AppendChild((*context.global_perspective_camera_node).clone());

    // Perspective scene (orthogonalprojection==null). The content is authored
    // in WE world units around the origin and viewed by an explicit eye/center
    // camera, not the 2D pixel-space placement above. Drive global_perspective
    // from scene.camera + general.fov and make it the active camera so every
    // layer (and its composite) renders under the same world-space view.
    if (! general.isOrtho) {
        Vector3d eye { sc.camera.eye[0], sc.camera.eye[1], sc.camera.eye[2] };
        Vector3d center { sc.camera.center[0], sc.camera.center[1], sc.camera.center[2] };
        Vector3d up { sc.camera.up[0], sc.camera.up[1], sc.camera.up[2] };
        perspective_camera->SetLookAt(eye, center, up);
        perspective_camera->SetFov(
            general.perspectiveoverridefov > 0.0f ? general.perspectiveoverridefov : general.fov);
        perspective_camera->SetAspect((double)context.ortho_w / (double)context.ortho_h);
        (void)scene.SetActiveCamera("global_perspective"_str);
        LoadRootCameraPaths(context, sc);
    }
}

void ParseCameraObj(ParseContext& context, wpscene::CameraObject& cam) {
    auto& scene           = *context.scene;
    bool  use_perspective = false;
    auto  perspective     = scene.Camera("global_perspective"_str);
    auto  active          = scene.ActiveCamera();
    if (perspective.is_some() && active.is_some() &&
        (*perspective).as_raw_ptr() == (*active).as_raw_ptr())
        use_perspective = true;

    std::string camera_name = use_perspective ? "global_perspective" : "global";
    auto        camera      = scene.CameraHandle(rstd::cppstd::as_str(camera_name).unwrap());
    if (camera.is_none()) return;

    auto       camera_owner = rstd::move(*camera);
    SceneNode* default_node =
        use_perspective
            ? (context.global_perspective_camera_node.is_some()
                   ? (*context.global_perspective_camera_node).as_ptr()
                   : nullptr)
            : (context.global_camera_node.is_some() ? (*context.global_camera_node).as_ptr()
                                                    : nullptr);
    if (default_node == nullptr) {
        auto attached = camera_owner->GetAttachedNode();
        if (attached.is_some()) default_node = attached.unwrap();
    }
    if (default_node == nullptr) return;

    double   default_width     = camera_owner->Width();
    double   default_height    = camera_owner->Height();
    double   default_fov       = camera_owner->Fov();
    Vector3f default_translate = default_node->Translate();
    Vector3f default_rotation  = default_node->Rotation();
    Vector3f origin { cam.origin[0], cam.origin[1], cam.origin[2] };
    Vector3f angles { cam.angles[0], cam.angles[1], cam.angles[2] };
    Vector3f path_translate_bias = use_perspective ? Vector3f::Zero() : default_translate;
    Vector3f path_rotation_bias  = use_perspective ? Vector3f::Zero() : default_rotation;

    auto node = Arc<SceneNode>::make(
        path_translate_bias + origin, Vector3f::Ones(), path_rotation_bias + angles, cam.name);
    node->ID() = i32(cam.id);
    if (! cam.visible) node->SetVisible(false);
    if (! cam.visible_user.empty())
        node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(cam.visible_user));

    if (cam.visible) camera_owner->AttatchNode(node.as_ptr());
    if (use_perspective) {
        if (cam.fov > 0.0f) camera_owner->SetFov(cam.fov);
        camera_owner->SetAspect((double)context.ortho_w / (double)context.ortho_h);
        (void)scene.SetActiveCamera(rstd::cppstd::as_str(camera_name).unwrap());
    }

    auto path                 = Arc<SceneCameraPath>::make();
    path->camera_name         = String::make(rstd::cppstd::as_str(camera_name).unwrap());
    path->camera              = Some(camera_owner.clone());
    path->node                = node.as_ptr();
    path->default_translate   = default_translate;
    path->default_rotation    = default_rotation;
    path->path_translate_bias = path_translate_bias;
    path->path_rotation_bias  = path_rotation_bias;
    path->default_width       = default_width;
    path->default_height      = default_height;
    path->default_fov         = default_fov;
    path->origin_base         = origin;
    path->rotation_base       = angles;
    path->zoom_base           = cam.zoom;
    path->fov_base            = cam.fov;
    path->perspective         = use_perspective;
    path->enabled             = cam.visible;
    if (! cam.visible_user.empty())
        path->visible_user_binding = ToSceneUserVisibilityBinding(cam.visible_user);
    AssignCurve(path->origin_curve, cam.field_bindings, "origin");
    AssignCurve(path->rotation_curve, cam.field_bindings, "angles");
    AssignCurve(path->zoom_curve, cam.field_bindings, "zoom");
    AssignCurve(path->fov_curve, cam.field_bindings, "fov");
    scene.RegisterCameraPath(path.clone());
    if (! cam.visible_user_key.empty()) {
        scene.RegisterCameraPathUserBinding(
            String::make(rstd::cppstd::as_str(cam.visible_user_key).unwrap()), path.clone());
    }

    WireCameraFieldScripts(context,
                           node,
                           camera_owner,
                           path,
                           cam.field_bindings,
                           path_translate_bias,
                           path_rotation_bias);
    RegisterNodeRef(context, cam.id, ParseContext::NodeRef { cam.parent, Some(node.clone()) });
}

void InitContext(ParseContext& context, fs::VFS& vfs, const wpscene::SceneMetadata& sc,
                 array<std::int32_t, 2> ortho_extent) {
    context.vfs = &vfs;
    auto& scene = *context.scene;
    scene.SetImageParser(Box<dyn<IImageParser>>::make(WPTexImageParser(&vfs)));
    context.particle_runtime = Some(WPParticleRuntime {});
    GenCardMesh(*scene.DefaultEffectMeshMut(), { 2.0f, 2.0f });

    scene.SetClearColor(array_cast<float>(sc.general.clearcolor));
    if (auto it = sc.general.user_bindings.find("clearcolor");
        it != sc.general.user_bindings.end()) {
        scene.SetClearColorUserKey(String::make(as_str(it->second).unwrap()));
    }
    scene.SetOrtho({ i32(ortho_extent[usize()]), i32(ortho_extent[usize(1)]) });
    context.ortho_w = ortho_extent[usize()];
    context.ortho_h = ortho_extent[usize(1)];

    {
        auto& gb                                   = context.global_base_uniforms;
        gb[rstd::cppstd::to_string(G_VIEWUP)]      = std::array { 0.0f, 1.0f, 0.0f };
        gb[rstd::cppstd::to_string(G_VIEWRIGHT)]   = std::array { 1.0f, 0.0f, 0.0f };
        gb[rstd::cppstd::to_string(G_VIEWFORWARD)] = std::array { 0.0f, 0.0f, -1.0f };
        gb[rstd::cppstd::to_string(G_EYEPOSITION)] = std::array { 0.0f, 0.0f, 0.0f };
        gb[rstd::cppstd::to_string(G_TEXELSIZE)]   = std::array { 1.0f / 1920.0f, 1.0f / 1080.0f };
        gb[rstd::cppstd::to_string(G_TEXELSIZEHALF)] =
            std::array { 1.0f / 1920.0f / 2.0f, 1.0f / 1080.0f / 2.0f };
        gb[rstd::cppstd::to_string(G_LIGHTAMBIENTCOLOR)]  = sc.general.ambientcolor;
        gb[rstd::cppstd::to_string(G_LIGHTSKYLIGHTCOLOR)] = sc.general.skylightcolor;

        if (sc.general.fogdistance) {
            context.shader_environment.fog_distance          = true;
            gb[rstd::cppstd::to_string(G_FOGDISTANCECOLOR)]  = sc.general.fogdistancecolor;
            gb[rstd::cppstd::to_string(G_FOGDISTANCEPARAMS)] = std::array {
                sc.general.fogdistancestart,
                sc.general.fogdistanceend - sc.general.fogdistancestart,
                sc.general.fogdistancestartdensity,
                sc.general.fogdistanceenddensity - sc.general.fogdistancestartdensity,
            };
        }
        if (sc.general.fogheight) {
            context.shader_environment.fog_height          = true;
            gb[rstd::cppstd::to_string(G_FOGHEIGHTCOLOR)]  = sc.general.fogheightcolor;
            gb[rstd::cppstd::to_string(G_FOGHEIGHTPARAMS)] = std::array {
                sc.general.fogheightstart,
                sc.general.fogheightend - sc.general.fogheightstart,
                sc.general.fogheightstartdensity,
                sc.general.fogheightenddensity - sc.general.fogheightstartdensity,
            };
        }
    }

    {
        WPUniformCameraParallax cam_para;
        cam_para.enable                         = sc.general.cameraparallax;
        cam_para.amount                         = sc.general.cameraparallaxamount;
        cam_para.delay                          = sc.general.cameraparallaxdelay;
        cam_para.mouse_influence                = sc.general.cameraparallaxmouseinfluence;
        context.uniform_state->CameraParallax() = cam_para;
        context.uniform_state->SetPointerDelay(cam_para.delay);
        if (auto it = sc.general.user_bindings.find("cameraparallaxmouseinfluence");
            it != sc.general.user_bindings.end()) {
            auto state =
                mut_ref<WPUniformSceneState>::from_raw_parts(context.uniform_state.as_ptr());
            auto field = it->first;
            scene.RegisterUserPropertyBinding(String::make(as_str(it->second).unwrap()),
                                              Box<dyn<FnMut<void(const Json&)>>>::make(
                                                  [state, field](const Json& property) mutable {
                                                      state->ApplyUserProperty(field, property);
                                                  }));
        }
    }
    {
        WPUniformCameraShake cam_shake;
        cam_shake.enable                     = sc.general.camerashake;
        cam_shake.amplitude                  = sc.general.camerashakeamplitude;
        cam_shake.speed                      = sc.general.camerashakespeed;
        cam_shake.roughness                  = sc.general.camerashakeroughness;
        context.uniform_state->CameraShake() = cam_shake;
        for (const auto& [field, key] : sc.general.user_bindings) {
            if (field == "camerashake" || field == "camerashakeamplitude" ||
                field == "camerashakespeed" || field == "camerashakeroughness") {
                auto state =
                    mut_ref<WPUniformSceneState>::from_raw_parts(context.uniform_state.as_ptr());
                scene.RegisterUserPropertyBinding(String::make(as_str(key).unwrap()),
                                                  Box<dyn<FnMut<void(const Json&)>>>::make(
                                                      [state, field](const Json& property) mutable {
                                                          state->ApplyUserProperty(field, property);
                                                      }));
            }
        }
        WireCameraShakeScripts(context, sc.general.field_bindings);
    }
}

void ParseImageObj(ParseContext& context, wpscene::ImageObject& img_obj) {
    auto& wpimgobj = img_obj;
    // Invisible image layers are kept in the scene tree because their composite
    // may be sampled by other layers via `_rt_imageLayerComposite_<id>`. The
    // render-graph builder decides whether to actually emit passes for them.
    if (! wpimgobj.visible) {
        context.scene->MarkLayerVisibilityElidable(
            WallpaperLayerId { .value = static_cast<i32>(wpimgobj.id) });
    }

    auto& vfs = *context.vfs;

    bool       isPassthrough      = wpimgobj.config.passthrough;
    const bool alpha_can_change   = ! wpimgobj.alpha_user_key.empty() ||
                                    wpimgobj.field_bindings.animations.count("alpha") != 0 ||
                                    wpimgobj.field_bindings.scripts.count("alpha") != 0;
    const auto geometry_size      = wpimgobj.size;
    const auto effect_target_size = ImageEffectTargetSize(context, wpimgobj);

    bool hasPuppet = ! wpimgobj.puppet.empty();
    (void)hasPuppet;

    std::unique_ptr<WPMdl> puppet;
    bool                   has_bones = false;
    bool                   has_mesh  = false;
    if (! wpimgobj.puppet.empty()) {
        puppet = std::make_unique<WPMdl>();
        if (! WPMdlParser::Parse(rstd::cppstd::as_str(wpimgobj.puppet).unwrap(), vfs, *puppet)) {
            rstd_error("parse puppet failed: {}", wpimgobj.puppet);
            puppet = nullptr;
        } else {
            has_bones = puppet->puppet.is_some() && ! (*puppet->puppet)->bones.is_empty();
            has_mesh  = false;
            for (const auto& m : puppet->meshes) {
                if (! m.positions.is_empty()) {
                    has_mesh = true;
                    break;
                }
            }
            if (! has_bones && ! has_mesh) {
                rstd_error("puppet has no mesh data: {}", wpimgobj.puppet);
                puppet = nullptr;
            }
        }
    }

    const bool has_author_effect = CountVisibleImageEffects(wpimgobj.effects) > 0;
    // A solid layer's flat material only produces its source color; a final compositor owns
    // BLENDMODE and the previous-framebuffer input.
    const bool layer_material_is_final =
        (! has_author_effect || has_bones) && ! wpimgobj.solid_layer;
    const bool color_blend_uses_layer_material =
        wpimgobj.colorBlendMode != 0 && layer_material_is_final;
    const bool append_color_blend_final_effect =
        wpimgobj.colorBlendMode != 0 && ! color_blend_uses_layer_material;
    if (append_color_blend_final_effect) {
        wpscene::ImageEffect colorEffect;
        wpscene::Material    colorMat;
        auto json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json");
        if (! json) {
            return;
        }
        colorMat.FromJson(*json);
        colorMat.combos[rstd::cppstd::to_string(WE_CB_BONECOUNT)] = 1;
        ApplyImageColorBlend(colorMat, wpimgobj);
        colorEffect.materials.push_back(std::move(colorMat));
        wpimgobj.effects.push_back(std::move(colorEffect));
    }
    const bool is_hidden_link_source =
        context.hidden_link_source_ids.contains(static_cast<std::int32_t>(wpimgobj.id));
    if (! has_author_effect && (is_hidden_link_source || wpimgobj.composite_layer)) {
        AppendLayerCompositePassthroughEffect(vfs, wpimgobj);
    }

    bool hasEffect = CountVisibleImageEffects(wpimgobj.effects) > 0;

    // No-effect fullscreen / compose layers contribute nothing on their own
    // (they just sample `_rt_default` and write it back). Mark as elidable
    // so the render-graph builder drops them when unreferenced, or routes
    // them to `_rt_link_<id>` when another layer reads their composite.
    if (! hasEffect && wpimgobj.visible && (wpimgobj.fullscreen || isPassthrough)) {
        context.scene->MarkLayerStaticElidable(
            WallpaperLayerId { .value = static_cast<i32>(wpimgobj.id) });
    }
    if (! hasEffect && wpimgobj.visible && wpimgobj.alpha <= 0.0f && ! alpha_can_change) {
        context.scene->MarkLayerStaticElidable(
            WallpaperLayerId { .value = static_cast<i32>(wpimgobj.id) });
    }

    // wpimgobj.origin[1] = context.ortho_h - wpimgobj.origin[1];
    auto           spImgNode = Arc<SceneNode>::make(Vector3f(wpimgobj.origin.data()),
                                                    Vector3f(wpimgobj.scale.data()),
                                                    Vector3f(wpimgobj.angles.data()),
                                                    wpimgobj.name);
    const Vector3f alignment_offset =
        wpimgobj.fullscreen ? Vector3f::Zero()
                            : AlignmentOffset(rstd::cppstd::as_str(wpimgobj.alignment).unwrap(),
                                              { geometry_size[0], geometry_size[1] });
    const bool solid_composite_context = HasSolidCompositeContext(context, wpimgobj);
    spImgNode->SetSize({ geometry_size[0], geometry_size[1] });
    spImgNode->SetPerspective(wpimgobj.perspective);
    spImgNode->SetBaseColor(Vector3f(wpimgobj.color.data()), wpimgobj.alpha);
    spImgNode->ID() = i32(wpimgobj.id);
    if (! wpimgobj.visible_user.empty())
        spImgNode->SetVisibleUserBinding(ToSceneUserVisibilityBinding(wpimgobj.visible_user));
    Vec<SceneMaterial*> image_property_materials;
    auto                track_image_property_material = [&](SceneMaterial* mat) {
        if ((wpimgobj.color_user_key.empty() && wpimgobj.alpha_user_key.empty()) || mat == nullptr)
            return;
        image_property_materials.emplace_back(mat);
    };
    Option<Arc<WPPuppetLayer>> image_puppet_layer;
    if (puppet && has_bones) {
        image_puppet_layer =
            Some(MakePuppetLayer((*puppet->puppet).clone(), wpimgobj.puppet_layers));
        RegisterPuppetLayer(context, spImgNode.as_ptr(), (*image_puppet_layer).clone());
    }

    // Puppet clipping masks: register the half-res shared RT here; per-mask
    // submeshes (pre-pass + clipped main) are emitted below after the base
    // material/mesh are built. Main material stays unmodified — only the
    // clipped-main submesh gets a CLIPPINGTARGET combo + g_Texture8 binding.
    constexpr std::string_view PUPPET_MASK_RT   = "_rt_puppet_mask";
    bool                       puppet_has_masks = false;
    if (puppet) {
        for (const auto& pmesh : puppet->meshes) {
            if (! pmesh.masks.is_empty()) {
                puppet_has_masks = true;
                break;
            }
        }
    }
    if (puppet_has_masks && has_bones &&
        context.scene->RenderTarget(as_str(PUPPET_MASK_RT).unwrap()).is_none()) {
        SceneRenderTarget rt {};
        rt.width       = 2;
        rt.height      = 2;
        rt.allowReuse  = true;
        rt.force_clear = true;
        rt.bind.enable = true;
        rt.bind.screen = true;
        rt.bind.scale  = 0.5f;
        context.scene->RegisterRenderTarget(String::make(as_str(PUPPET_MASK_RT).unwrap()),
                                            rstd::move(rt));
    }

    SceneMaterial            material;
    WPUniformNodeConfigDraft svData;

    ShaderValueMap    baseConstSvs = context.global_base_uniforms;
    WPShaderInfo      shaderInfo;
    wpscene::Material image_wpmat                 = wpimgobj.material.clone();
    wpscene::Material image_user_texture_fallback = image_wpmat.clone();
    if (color_blend_uses_layer_material && ! hasEffect) ApplyImageColorBlend(image_wpmat, wpimgobj);
    ApplyUserTextureBindings(context, image_wpmat);
    {
        svData.propagate_parallax_to_children = ! wpimgobj.disablepropagation;
        svData.propagated_parallax_depth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
        if (! hasEffect) {
            svData.parallax_depth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
            if (puppet && has_bones) {
                WPMdlParser::AddPuppetShaderInfo(shaderInfo, *puppet);
            }
        }

        baseConstSvs[rstd::cppstd::to_string(G_COLOR4)] = std::array<float, 4> {
            wpimgobj.color[0], wpimgobj.color[1], wpimgobj.color[2], wpimgobj.alpha
        };
        baseConstSvs[rstd::cppstd::to_string(G_COLOR)] =
            std::array<float, 3> { wpimgobj.color[0], wpimgobj.color[1], wpimgobj.color[2] };
        baseConstSvs[rstd::cppstd::to_string(G_ALPHA)]      = wpimgobj.alpha;
        baseConstSvs[rstd::cppstd::to_string(G_USERALPHA)]  = wpimgobj.alpha;
        baseConstSvs[rstd::cppstd::to_string(G_BRIGHTNESS)] = wpimgobj.brightness;

        shaderInfo.baseConstSvs = baseConstSvs;

        if (! LoadMaterial(vfs,
                           context.ShaderCachePath(),
                           context.shader_environment,
                           image_wpmat,
                           context.scene.get(),
                           &material,
                           &shaderInfo)) {
            rstd_error("load imageobj '{}' material faild", wpimgobj.name);
            return;
        };
        LoadConstvalue(material, image_wpmat, shaderInfo);
    }

    // Whether the layer's base texture is point-sampled (noInterpolation).
    // Captured here because `material` is moved into the mesh below, well
    // before the effect ping-pong RTs are created.
    bool point_source = false;
    if (! material.textures.empty()) {
        auto texture =
            context.scene->Texture(rstd::cppstd::as_str(material.textures.front()).unwrap());
        point_source = texture.is_some() && (**texture).sample.magFilter == TextureFilter::NEAREST;
    }

    for (const auto& cs : image_wpmat.constantshadervalues) {
        const auto&               name  = cs.first;
        const std::vector<float>& value = cs.second;
        std::string               glname;
        if (shaderInfo.alias.count(name) != 0) {
            glname = shaderInfo.alias.at(name);
        } else {
            for (const auto& el : shaderInfo.alias) {
                if (el.second.substr(2) == name) {
                    glname = el.second;
                    break;
                }
            }
        }
        if (glname.empty()) {
            rstd_error("ShaderValue: {} not found in glsl", name);
        } else {
            material.customShader.constValues[glname] = value;
        }
    }

    // mesh
    SceneMesh                  effct_final_mesh {};
    auto                       spMesh        = std::make_shared<SceneMesh>();
    auto&                      mesh          = *spMesh;
    const std::array<float, 2> mapRate       = Texture0UvScale(material, wpimgobj.nopadding);
    const Vector3f source_alignment_offset   = hasEffect ? Vector3f::Zero() : alignment_offset;
    auto           add_puppet_mask_submeshes = [&](SceneMesh& target, uint32_t first_mask_slot) {
        if (! puppet_has_masks) return;
        std::set<uint32_t> clipped_indices;
        for (const auto& pmesh : puppet->meshes) {
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(idx);
            }
        }
        if (! clipped_indices.empty()) {
            size_t smi = 0;
            for (const auto& pmesh : puppet->meshes) {
                if (pmesh.positions.is_empty()) continue;
                if (smi >= target.Submeshes().size()) break;
                std::vector<SceneMesh::DrawRange> kept;
                kept.reserve(pmesh.parts.len().to_primitive());
                for (usize i {}; i < pmesh.parts.len(); ++i) {
                    const auto& p = pmesh.parts[i];
                    if (p.size == 0) continue;
                    if (clipped_indices.count(static_cast<uint32_t>(i.to_primitive())) != 0)
                        continue;
                    kept.push_back({ p.start, p.size });
                }
                target.Submeshes()[smi].draw_ranges = std::move(kept);
                ++smi;
            }
        }

        uint32_t slot = first_mask_slot;
        for (const auto& pmesh : puppet->meshes) {
            for (const auto& mb : pmesh.masks) {
                target.Submeshes().emplace_back();
                auto& pre_sm = target.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    pre_sm, pmesh, mb.part_ids_b.as_slice(), { mapRate[0], mapRate[1] });
                pre_sm.material_slot   = slot++;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                target.Submeshes().emplace_back();
                auto& clip_sm = target.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    clip_sm, pmesh, mb.part_ids_a.as_slice(), { mapRate[0], mapRate[1] });
                clip_sm.material_slot = slot++;
            }
        }
    };

    if (puppet) {
        if (hasEffect) {
            effct_final_mesh.SetGeometryTransform(
                Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
            GenCardMesh(
                mesh, { geometry_size[0], geometry_size[1] }, mapRate, source_alignment_offset);
            for (const auto& m : puppet->meshes) {
                if (m.positions.is_empty()) continue;
                effct_final_mesh.Submeshes().emplace_back();
                WPMdlParser::GenMeshFromMdl(
                    effct_final_mesh.Submeshes().back(), m, { mapRate[0], mapRate[1] });
            }
            if (has_bones) add_puppet_mask_submeshes(effct_final_mesh, 1);

            if (has_bones) {
                wpscene::ImageEffect puppet_effect;
                wpscene::Material    puppet_mat = image_wpmat.clone();
                puppet_mat.textures[0]          = "";
                WPMdlParser::AddPuppetMatInfo(puppet_mat, *puppet);
                if (color_blend_uses_layer_material) ApplyImageColorBlend(puppet_mat, wpimgobj);
                puppet_effect.materials.push_back(std::move(puppet_mat));
                wpimgobj.effects.push_back(std::move(puppet_effect));
            }
        } else {
            mesh.SetGeometryTransform(
                Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
            for (const auto& m : puppet->meshes) {
                if (m.positions.is_empty()) continue;
                mesh.Submeshes().emplace_back();
                WPMdlParser::GenMeshFromMdl(mesh.Submeshes().back(), m, { mapRate[0], mapRate[1] });
            }
        }
    }
    if (! puppet) {
        GenCardMesh(mesh, { geometry_size[0], geometry_size[1] }, mapRate, source_alignment_offset);
        GenCardMesh(effct_final_mesh,
                    { geometry_size[0], geometry_size[1] },
                    { 1.0f, 1.0f },
                    alignment_offset);
    }
    // material blendmode for last step to use
    auto finalMaterialState = material;
    // disable img material blend, as it's the first effect node now
    SceneImageEffectLayer* image_effect_layer { nullptr };
    if (hasEffect) {
        material.blenmode = BlendMode::Normal;
    }
    mesh.AddMaterial(std::move(material));
    track_image_property_material(mesh.MaterialSlots().back().get());
    RegisterShaderUserVarIndex(context.scene.get(), mesh.Material(), image_wpmat, shaderInfo);
    RegisterMaterialUserTextureIndex(
        context.scene.get(), mesh.Material(), image_user_texture_fallback, shaderInfo);

    // Puppet clipping masks: each MaskBlock becomes a pair of submeshes.
    // 1) Pre-pass: clippingmaskimage4 over `part_ids_b` (mask shape mesh)
    //    writes the mask RT.
    // 2) Clipped main: a clone of the main material with CLIPPINGTARGET combo
    //    + g_Texture8 = mask RT, draw range = `part_ids_a` (the clipped parts).
    // The original main submesh has all `part_ids_a` parts removed so the
    // clipped region is only drawn through the masked variant.
    if (puppet && ! hasEffect && has_bones && puppet_has_masks) {
        // `part_ids_a` indexes into pmesh.parts[] (position), not part.id.
        std::set<uint32_t> clipped_indices;
        for (const auto& pmesh : puppet->meshes) {
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(idx);
            }
        }
        // Rebuild main submeshes' draw_ranges: drop any part whose position
        // index is in `part_ids_a` of any mask block.
        if (! clipped_indices.empty()) {
            size_t smi = 0;
            for (const auto& pmesh : puppet->meshes) {
                if (pmesh.positions.is_empty()) continue;
                if (smi >= mesh.Submeshes().size()) break;
                std::vector<SceneMesh::DrawRange> kept;
                kept.reserve(pmesh.parts.len().to_primitive());
                for (usize i {}; i < pmesh.parts.len(); ++i) {
                    const auto& p = pmesh.parts[i];
                    if (p.size == 0) continue;
                    if (clipped_indices.count(static_cast<uint32_t>(i.to_primitive())) != 0)
                        continue;
                    kept.push_back({ p.start, p.size });
                }
                mesh.Submeshes()[smi].draw_ranges = std::move(kept);
                ++smi;
            }
        }

        const std::string albedo_tex =
            image_wpmat.textures.empty() ? std::string {} : image_wpmat.textures[0];
        for (const auto& pmesh : puppet->meshes) {
            for (const auto& mb : pmesh.masks) {
                // (1) mask pre-pass submesh
                wpscene::Material mask_wpmat;
                mask_wpmat.shader     = "clippingmaskimage4";
                mask_wpmat.blending   = "translucent";
                mask_wpmat.depthtest  = "disabled";
                mask_wpmat.depthwrite = "disabled";
                mask_wpmat.cullmode   = "nocull";
                mask_wpmat.textures.resize(2);
                mask_wpmat.textures[0] = albedo_tex;
                mask_wpmat.textures[1] = rstd::cppstd::to_string(mb.mat_json.as_str());
                WPMdlParser::AddPuppetMatInfo(mask_wpmat, *puppet);

                SceneMaterial mask_scene_mat;
                WPShaderInfo  mask_shaderInfo;
                mask_shaderInfo.baseConstSvs = baseConstSvs;
                if (! LoadMaterial(vfs,
                                   context.ShaderCachePath(),
                                   context.shader_environment,
                                   mask_wpmat,
                                   context.scene.get(),
                                   &mask_scene_mat,
                                   &mask_shaderInfo)) {
                    rstd_warn("load mask pre-pass material failed for '{}'", wpimgobj.name);
                    continue;
                }
                uint32_t pre_slot = (uint32_t)mesh.MaterialSlots().size();
                mesh.AddMaterial(std::move(mask_scene_mat));
                track_image_property_material(mesh.MaterialSlots().back().get());
                mesh.Submeshes().emplace_back();
                auto& pre_sm = mesh.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    pre_sm, pmesh, mb.part_ids_b.as_slice(), { mapRate[0], mapRate[1] });
                pre_sm.material_slot   = pre_slot;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                // (2) clipped-main submesh: main material + CLIPPINGTARGET
                wpscene::Material clip_wpmat        = image_wpmat.clone();
                clip_wpmat.combos["CLIPPINGTARGET"] = 1;
                clip_wpmat.combos["CLIPPINGUVS"]    = 1;
                if (clip_wpmat.textures.size() < 9) clip_wpmat.textures.resize(9);
                clip_wpmat.textures[8] = std::string(PUPPET_MASK_RT);
                WPMdlParser::AddPuppetMatInfo(clip_wpmat, *puppet);

                SceneMaterial clip_scene_mat;
                WPShaderInfo  clip_shaderInfo;
                clip_shaderInfo.baseConstSvs = baseConstSvs;
                if (! LoadMaterial(vfs,
                                   context.ShaderCachePath(),
                                   context.shader_environment,
                                   clip_wpmat,
                                   context.scene.get(),
                                   &clip_scene_mat,
                                   &clip_shaderInfo)) {
                    rstd_warn("load clipped main material failed for '{}'", wpimgobj.name);
                    continue;
                }
                LoadConstvalue(clip_scene_mat, clip_wpmat, clip_shaderInfo);
                uint32_t clip_slot = (uint32_t)mesh.MaterialSlots().size();
                mesh.AddMaterial(std::move(clip_scene_mat));
                track_image_property_material(mesh.MaterialSlots().back().get());
                mesh.Submeshes().emplace_back();
                auto& clip_sm = mesh.Submeshes().back();
                WPMdlParser::GenMaskSubmeshFromMdl(
                    clip_sm, pmesh, mb.part_ids_a.as_slice(), { mapRate[0], mapRate[1] });
                clip_sm.material_slot = clip_slot;
            }
        }
    }

    spImgNode->AddMesh(spMesh);

    SetWPUniformConfig(context, spImgNode, rstd::move(svData));
    if (hasEffect) {
        auto& scene = *context.scene;
        // currently use addr for unique
        std::string nodeAddr = getAddr(spImgNode.as_ptr());
        const auto  effect_extent =
            NonZeroRenderTargetExtent(effect_target_size[0], effect_target_size[1]);
        auto active = scene.ActiveCamera();
        if (active.is_none()) return;
        // set camera to attatch effect
        Arc<SceneCamera> layer_camera =
            isPassthrough ? Arc<SceneCamera>::make(SceneCamera::MakeOrthographic(
                                (**active).Width(), (**active).Height(), -1.0, 1.0))
                          : Arc<SceneCamera>::make(SceneCamera::MakeOrthographic(
                                effect_extent[0], effect_extent[1], -1.0, 1.0));
        if (isPassthrough) {
            auto attached = (**active).GetAttachedNode();
            if (attached.is_some()) layer_camera->AttatchNode(attached.unwrap());
            scene.RegisterLinkedCamera(String::make("global"_str),
                                       String::make(rstd::cppstd::as_str(nodeAddr).unwrap()));
        } else {
            // Attach the per-layer effect camera to spImgNode itself so the
            // camera follows the layer through any parent-container world
            // translation. Otherwise the layer's quad ends up off-center in
            // the ping-pong RT whenever the layer is nested under a non-zero
            // container.
            layer_camera->AttatchNode(spImgNode.as_ptr());
        }
        scene.RegisterCamera(String::make(rstd::cppstd::as_str(nodeAddr).unwrap()),
                             layer_camera.clone());
        if (wpimgobj.composite_layer) {
            const std::string group_camera       = nodeAddr + "_group";
            auto              group_camera_owner = Arc<SceneCamera>::make(
                SceneCamera::MakeOrthographic(effect_extent[0], effect_extent[1], -1.0, 1.0));
            group_camera_owner->AttatchNode(spImgNode.as_ptr());
            scene.RegisterCamera(String::make(rstd::cppstd::as_str(group_camera).unwrap()),
                                 rstd::move(group_camera_owner));
            scene.RegisterRenderGroup(WallpaperLayerId { .value = static_cast<i32>(wpimgobj.id) },
                                      String::make(rstd::cppstd::as_str(group_camera).unwrap()));
        }
        spImgNode->SetCamera(nodeAddr);
        std::string effect_ppong_a, effect_ppong_b;
        effect_ppong_a = rstd::cppstd::to_string(OWE_EFFECT_PPONG_PREFIX_A) + nodeAddr;
        effect_ppong_b = rstd::cppstd::to_string(OWE_EFFECT_PPONG_PREFIX_B) + nodeAddr;
        // set image effect
        auto imgEffectLayer =
            std::make_shared<SceneImageEffectLayer>(spImgNode.as_ptr(),
                                                    static_cast<float>(effect_extent[0]),
                                                    static_cast<float>(effect_extent[1]),
                                                    effect_ppong_a,
                                                    effect_ppong_b);
        image_effect_layer = imgEffectLayer.get();
        {
            imgEffectLayer->SetFullscreen(wpimgobj.fullscreen);
            imgEffectLayer->SetFinalMaterialState(finalMaterialState);
            imgEffectLayer->SetSkipWhenNoRuntimeEffect(wpimgobj.fullscreen || isPassthrough);
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effct_final_mesh);
            layer_camera->AttatchImgEffect(imgEffectLayer);
        }
        // set renderTarget for ping-pong operate
        {
            SceneRenderTarget target {
                .width                = effect_extent[0],
                .height               = effect_extent[1],
                .allowReuse           = true,
                .force_clear          = ! wpimgobj.fullscreen && ! wpimgobj.composite_layer,
                .clear_on_first_write = true,
                .preserve_on_write    = wpimgobj.composite_layer,
            };
            if (wpimgobj.fullscreen) {
                target.bind = { .enable = true, .screen = true };
            }
            // Point-art images (noInterpolation) must stay point-sampled through
            // the whole effect chain.
            if (point_source) {
                auto& s     = target.sample;
                s.magFilter = s.minFilter = TextureFilter::NEAREST;
            }
            scene.RegisterRenderTarget(String::make(as_str(effect_ppong_a).unwrap()), target);
            scene.RegisterRenderTarget(String::make(as_str(effect_ppong_b).unwrap()),
                                       rstd::move(target));
        }

        int32_t    i_eff = -1;
        bool       last_effect_can_composite_final { false };
        const bool allow_transparent_previous_final = ! solid_composite_context;
        const bool passthrough_can_composite_final  = isPassthrough;
        for (const auto& wpeffobj : wpimgobj.effects) {
            i_eff++;
            if (! wpeffobj.visible && wpeffobj.visible_user.empty()) {
                i_eff--;
                continue;
            }
            std::shared_ptr<SceneImageEffect> imgEffect = std::make_shared<SceneImageEffect>();
            imgEffect->name                             = wpeffobj.name;
            imgEffect->runtime_visible                  = wpeffobj.visible;
            if (! wpeffobj.visible_user.empty()) {
                imgEffect->visible_user_binding =
                    ToSceneUserVisibilityBinding(wpeffobj.visible_user);
            }

            // this will be replace when resolve, use here to get rt info
            const std::string inRT { effect_ppong_a };

            // fbo name map and effect command
            std::string effaddr = getAddr(imgEffectLayer.get());

            std::unordered_map<std::string, std::string> fboMap;
            {
                fboMap["previous"] = inRT;
                for (std::size_t i = 0; i < wpeffobj.fbos.size(); i++) {
                    const auto& wpfbo = wpeffobj.fbos.at(i);
                    // Some effects (e.g. WE DOF) use fbo names without the
                    // `_rt_` prefix (`_coc`, `_downscaled1`, ...). Force the
                    // prefix so IsSpecTex / render-target lookups treat them
                    // as render targets instead of disk textures.
                    std::string rtname =
                        rstd::str_::starts_with(as_str(wpfbo.name).unwrap(), WE_SPEC_PREFIX)
                            ? wpfbo.name + "_" + effaddr
                            : rstd::cppstd::to_string(WE_SPEC_PREFIX) + wpfbo.name + "_" + effaddr;
                    if (wpimgobj.fullscreen) {
                        SceneRenderTarget target {
                            .width      = 2,
                            .height     = 2,
                            .allowReuse = ! wpfbo.unique,
                        };
                        target.bind = {
                            .enable = true,
                            .screen = true,
                            .scale  = 1.0 / wpfbo.scale,
                        };
                        scene.RegisterRenderTarget(String::make(as_str(rtname).unwrap()),
                                                   rstd::move(target));
                    } else {
                        auto fbo_size = [&]() -> std::array<uint16_t, 2> {
                            if (wpfbo.fit > 0) {
                                const float max_size =
                                    std::max(effect_target_size[0], effect_target_size[1]);
                                if (max_size > 0.0f) {
                                    const float fit_scale =
                                        static_cast<float>(wpfbo.fit) / max_size;
                                    const auto fit_extent = NonZeroRenderTargetExtent(
                                        std::round(effect_target_size[0] * fit_scale),
                                        std::round(effect_target_size[1] * fit_scale));
                                    return { static_cast<uint16_t>(fit_extent[0]),
                                             static_cast<uint16_t>(fit_extent[1]) };
                                }
                            }
                            const auto scaled_extent = NonZeroRenderTargetExtent(
                                effect_target_size[0] / static_cast<float>(wpfbo.scale),
                                effect_target_size[1] / static_cast<float>(wpfbo.scale));
                            return { static_cast<uint16_t>(scaled_extent[0]),
                                     static_cast<uint16_t>(scaled_extent[1]) };
                        }();
                        scene.RegisterRenderTarget(
                            String::make(as_str(rtname).unwrap()),
                            SceneRenderTarget { .width      = fbo_size[0],
                                                .height     = fbo_size[1],
                                                .allowReuse = ! wpfbo.unique });
                    }
                    fboMap[wpfbo.name] = rtname;
                }
            }
            // load! effect commands
            {
                for (const auto& el : wpeffobj.commands) {
                    if (el.command != "copy") {
                        rstd_error("Unknown effect command: {}", el.command);
                        continue;
                    }
                    if (fboMap.count(el.target) + fboMap.count(el.source) < 2) {
                        rstd_error(
                            "Unknown effect command dst or src: {} {}", el.target, el.source);
                        continue;
                    }
                    imgEffect->commands.push_back({ .cmd      = SceneImageEffect::CmdType::Copy,
                                                    .dst      = fboMap[el.target],
                                                    .src      = fboMap[el.source],
                                                    .afterpos = el.afterpos });
                }
            }

            bool eff_mat_ok { true };

            for (std::size_t i_mat = 0; i_mat < wpeffobj.materials.size(); i_mat++) {
                wpscene::Material wpmat = wpeffobj.materials.at(i_mat).clone();
                std::string       matOutRT { rstd::cppstd::to_string(OWE_EFFECT_PPONG_PREFIX_B) };
                std::optional<wpscene::Material> user_texture_fallback;
                if (wpeffobj.passes.size() > i_mat) {
                    const auto& wppass = wpeffobj.passes.at(i_mat);
                    wpmat.MergePass(wppass);
                    ApplyTextureBinds(wpmat, std::span(wppass.bind), fboMap);
                    user_texture_fallback = wpmat.clone();
                    ApplyUserTextureBindings(context, wpmat);
                    if (! wppass.target.empty()) {
                        if (fboMap.count(wppass.target) == 0) {
                            rstd_error("fbo {} not found", wppass.target);
                        } else {
                            matOutRT = fboMap.at(wppass.target);
                        }
                    }
                }
                // A layer's own effect referencing its composite
                // (`_rt_imageLayerComposite_<self>[_a|_b]`) wants this layer's
                // running chain result.
                for (auto& t : wpmat.textures) {
                    if (ParseImageLayerCompositeId(as_str(t).unwrap()) ==
                        static_cast<std::uint32_t>(wpimgobj.id))
                        t = effect_ppong_a;
                }
                if (wpmat.textures.size() == 0) wpmat.textures.resize(1);
                if (wpmat.textures.at(0).empty()) {
                    wpmat.textures[0] = inRT;
                }
                auto         spEffNode  = Arc<SceneNode>::make();
                std::string  effmataddr = getAddr(spEffNode.as_ptr());
                WPShaderInfo wpEffShaderInfo;
                wpEffShaderInfo.baseConstSvs = baseConstSvs;
                wpEffShaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ETVP)] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                wpEffShaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ETVPI)] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                SceneMaterial            material;
                WPUniformNodeConfigDraft svData;
                svData.propagate_parallax_to_children = ! wpimgobj.disablepropagation;
                SceneShaderValueAnimationMap final_quad_shader_values;
                if (! LoadMaterial(vfs,
                                   context.ShaderCachePath(),
                                   context.shader_environment,
                                   wpmat,
                                   context.scene.get(),
                                   &material,
                                   &wpEffShaderInfo)) {
                    eff_mat_ok = false;
                    break;
                }

                // load glname from alias and load to constvalue
                LoadConstvalue(material, wpmat, wpEffShaderInfo, &final_quad_shader_values);
                auto spMesh = std::make_shared<SceneMesh>();
                {
                    svData.propagated_parallax_depth = { wpimgobj.parallaxDepth[0],
                                                         wpimgobj.parallaxDepth[1] };
                    svData.parallax_depth            = { wpimgobj.parallaxDepth[0],
                                                         wpimgobj.parallaxDepth[1] };
                    svData.effect_projection_node    = Some(spImgNode.clone());
                    svData.effect_projection_size    = { static_cast<float>(effect_extent[0]),
                                                         static_cast<float>(effect_extent[1]) };
                    if (puppet && wpmat.use_puppet) {
                        auto effect_puppet_layer =
                            MakePuppetLayer((*puppet->puppet).clone(), wpimgobj.puppet_layers);
                        RegisterPuppetLayer(
                            context, spEffNode.as_ptr(), rstd::move(effect_puppet_layer));
                    }
                }
                spMesh->AddMaterial(std::move(material));
                track_image_property_material(spMesh->MaterialSlots().back().get());
                RegisterShaderUserVarIndex(
                    context.scene.get(), spMesh->Material(), wpmat, wpEffShaderInfo);
                if (user_texture_fallback.has_value()) {
                    RegisterMaterialUserTextureIndex(context.scene.get(),
                                                     spMesh->Material(),
                                                     *user_texture_fallback,
                                                     wpEffShaderInfo);
                }
                auto add_puppet_mask_materials = [&]() -> bool {
                    if (! (puppet && wpmat.use_puppet && puppet_has_masks)) return true;
                    const std::string source_tex =
                        wpmat.textures.empty() ? std::string {} : wpmat.textures[0];
                    for (const auto& pmesh : puppet->meshes) {
                        for (const auto& mb : pmesh.masks) {
                            wpscene::Material mask_wpmat;
                            mask_wpmat.shader     = "clippingmaskimage4";
                            mask_wpmat.blending   = "translucent";
                            mask_wpmat.depthtest  = "disabled";
                            mask_wpmat.depthwrite = "disabled";
                            mask_wpmat.cullmode   = "nocull";
                            mask_wpmat.textures.resize(2);
                            mask_wpmat.textures[0] = source_tex;
                            mask_wpmat.textures[1] = rstd::cppstd::to_string(mb.mat_json.as_str());
                            WPMdlParser::AddPuppetMatInfo(mask_wpmat, *puppet);

                            SceneMaterial mask_material;
                            WPShaderInfo  mask_shaderInfo;
                            mask_shaderInfo.baseConstSvs = wpEffShaderInfo.baseConstSvs;
                            if (! LoadMaterial(vfs,
                                               context.ShaderCachePath(),
                                               context.shader_environment,
                                               mask_wpmat,
                                               context.scene.get(),
                                               &mask_material,
                                               &mask_shaderInfo)) {
                                return false;
                            }
                            LoadConstvalue(mask_material, mask_wpmat, mask_shaderInfo);
                            spMesh->AddMaterial(std::move(mask_material));
                            track_image_property_material(spMesh->MaterialSlots().back().get());

                            wpscene::Material clip_wpmat        = wpmat.clone();
                            clip_wpmat.combos["CLIPPINGTARGET"] = 1;
                            clip_wpmat.combos["CLIPPINGUVS"]    = 1;
                            if (clip_wpmat.textures.size() < 9) clip_wpmat.textures.resize(9);
                            clip_wpmat.textures[8] = std::string(PUPPET_MASK_RT);
                            WPMdlParser::AddPuppetMatInfo(clip_wpmat, *puppet);

                            SceneMaterial clip_material;
                            WPShaderInfo  clip_shaderInfo;
                            clip_shaderInfo.baseConstSvs = wpEffShaderInfo.baseConstSvs;
                            if (! LoadMaterial(vfs,
                                               context.ShaderCachePath(),
                                               context.shader_environment,
                                               clip_wpmat,
                                               context.scene.get(),
                                               &clip_material,
                                               &clip_shaderInfo)) {
                                return false;
                            }
                            LoadConstvalue(clip_material, clip_wpmat, clip_shaderInfo);
                            spMesh->AddMaterial(std::move(clip_material));
                            track_image_property_material(spMesh->MaterialSlots().back().get());
                        }
                    }
                    return true;
                };
                if (! add_puppet_mask_materials()) {
                    eff_mat_ok = false;
                    break;
                }
                if (auto* mat = spMesh->Material(); mat != nullptr) {
                    last_effect_can_composite_final = CanCompositeFinalEffectMaterial(
                        mat->name, wpEffShaderInfo, allow_transparent_previous_final);
                }
                spEffNode->AddMesh(spMesh);

                SetWPUniformConfig(context, spEffNode, rstd::move(svData));
                imgEffect->nodes.push_back(SceneImageEffectNode {
                    .output                   = matOutRT,
                    .sceneNode                = spEffNode.clone(),
                    .uses_unit_final_quad     = UsesUnitFinalQuad(wpmat),
                    .final_quad_shader_values = std::move(final_quad_shader_values),
                });
            }

            if (eff_mat_ok)
                imgEffectLayer->AddEffect(imgEffect);
            else {
                rstd_error("effect \'{}\' failed to load", wpeffobj.name);
            }
        }

        if (! wpimgobj.fullscreen && ! wpimgobj.copybackground &&
            ! passthrough_can_composite_final && ! last_effect_can_composite_final) {
            wpscene::Material passthrough_mat;
            auto json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json");
            if (! json) {
                rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
            } else {
                if (! passthrough_mat.FromJson(*json)) {
                    rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
                } else {
                    if (passthrough_mat.textures.empty())
                        passthrough_mat.textures.push_back(effect_ppong_a);
                    else
                        passthrough_mat.textures[0] = effect_ppong_a;

                    auto finalEffect = std::make_shared<SceneImageEffect>();
                    auto spFinalNode = Arc<SceneNode>::make();

                    WPShaderInfo wpFinalShaderInfo;
                    wpFinalShaderInfo.baseConstSvs = NeutralColorUniforms(baseConstSvs);
                    SceneMaterial            finalMaterial;
                    WPUniformNodeConfigDraft finalSvData;
                    finalSvData.propagate_parallax_to_children = ! wpimgobj.disablepropagation;
                    finalSvData.propagated_parallax_depth      = { wpimgobj.parallaxDepth[0],
                                                                   wpimgobj.parallaxDepth[1] };
                    finalSvData.parallax_depth                 = { wpimgobj.parallaxDepth[0],
                                                                   wpimgobj.parallaxDepth[1] };
                    if (LoadMaterial(vfs,
                                     context.ShaderCachePath(),
                                     context.shader_environment,
                                     passthrough_mat,
                                     context.scene.get(),
                                     &finalMaterial,
                                     &wpFinalShaderInfo)) {
                        LoadConstvalue(finalMaterial, passthrough_mat, wpFinalShaderInfo);
                        auto spFinalMesh = std::make_shared<SceneMesh>();
                        spFinalMesh->AddMaterial(std::move(finalMaterial));
                        RegisterShaderUserVarIndex(context.scene.get(),
                                                   spFinalMesh->Material(),
                                                   passthrough_mat,
                                                   wpFinalShaderInfo);
                        spFinalNode->AddMesh(spFinalMesh);
                        SetWPUniformConfig(context, spFinalNode, rstd::move(finalSvData));
                        finalEffect->nodes.push_back(
                            SceneImageEffectNode { effect_ppong_b, spFinalNode.clone() });
                        imgEffectLayer->AddEffect(finalEffect);
                    } else {
                        rstd_error("effect passthrough failed to load for '{}'", wpimgobj.name);
                    }
                }
            }
        }
    }
    const Matrix4d alignment_base_transform =
        image_effect_layer ? image_effect_layer->FinalMesh().GeometryTransform()
                           : spImgNode->GeometryTransform();
    RegisterImageAlignmentBinding(
        context,
        spImgNode.as_ptr(),
        rstd::cppstd::as_str(wpimgobj.alignment).unwrap(),
        ParseContext::ImageAlignmentSetter::make(
            [image_effect_layer, alignment_base_transform, alignment_offset, geometry_size](
                SceneNode* node, ref<str> alignment) {
                const Vector3f delta =
                    AlignmentOffset(alignment, { geometry_size[0], geometry_size[1] }) -
                    alignment_offset;
                auto transform = alignment_base_transform *
                                 Affine3d(Translation3d(delta.cast<double>())).matrix();
                if (image_effect_layer)
                    image_effect_layer->FinalMesh().SetGeometryTransform(rstd::move(transform));
                else if (node)
                    node->SetGeometryTransform(rstd::move(transform));
            }));

    AssignNodeFieldAnimations(*spImgNode.as_ptr(), wpimgobj.field_bindings);
    WireFieldScripts(context, spImgNode, wpimgobj.field_bindings);
    if (! wpimgobj.color_user_key.empty()) {
        context.scene->RegisterImageColorUserBinding(
            String::make(as_str(wpimgobj.color_user_key).unwrap()),
            *spImgNode,
            image_property_materials.as_slice());
    }
    if (! wpimgobj.alpha_user_key.empty()) {
        context.scene->RegisterImageAlphaUserBinding(
            String::make(as_str(wpimgobj.alpha_user_key).unwrap()),
            *spImgNode,
            image_property_materials.as_slice());
    }
    RegisterNodeRef(
        context,
        wpimgobj.id,
        ParseContext::NodeRef {
            wpimgobj.parent,
            Some(spImgNode.clone()),
            (puppet && puppet->puppet.is_some()) ? Some((*puppet->puppet).clone()) : None(),
            String::make(rstd::cppstd::as_str(wpimgobj.attachment).unwrap()),
            image_puppet_layer.is_some() ? Some((*image_puppet_layer).clone()) : None(),
        });
}

struct ParticleChildPtr {
    wpscene::ParticleChild* child { nullptr };
    SceneNode*              node_parent { nullptr };
    WPParticleSubSystem*    particle_parent { nullptr };
    bool                    inherit_instance_override { false };

    // Effective world scale at node_parent. Particle child origins are
    // pre-divided by this so the shader's MVP scale recovers the authored
    // parent-relative world-pixel offset.
    Eigen::Vector3f world_scale { 1.f, 1.f, 1.f };
};

wpscene::ParticleInstanceoverride ParticleOverrideForNode(const wpscene::ParticleObject& obj,
                                                          bool                           is_child,
                                                          bool inherit_instance_override) {
    if (! is_child) return obj.instanceoverride;

    wpscene::ParticleInstanceoverride out;
    if (! inherit_instance_override) return out;
    const auto& parent = obj.instanceoverride;
    out.enabled        = parent.enabled;
    out.alpha          = parent.alpha;
    out.overColor      = parent.overColor;
    out.overColorn     = parent.overColorn;
    out.color          = parent.color;
    out.colorn         = parent.colorn;
    for (std::string_view field : { "alpha", "color", "colorn" }) {
        if (auto it = parent.bindings.find(std::string(field)); it != parent.bindings.end()) {
            out.bindings.emplace(it->first, it->second);
        }
    }
    return out;
}

void ParseParticleObj(ParseContext& context, wpscene::ParticleObject& wppartobj,
                      ParticleChildPtr child_ptr = {}) {
    struct ChildData {
        ChildData() = default;
        ChildData(const wpscene::ParticleChild& o)
            : type(o.type),
              maxcount(o.maxcount),
              controlpointstartindex(o.controlpointstartindex),
              probability(o.probability) {}
        std::string type { "static" };
        i32         maxcount { 20 };
        Option<i32> controlpointstartindex;
        float       probability { 1.0f };
    };

    wpscene::Particle*     p_particle_obj { nullptr };
    Option<Arc<SceneNode>> spNodeOpt;
    ChildData              child_data;

    bool is_child = child_ptr.child != nullptr;
    if (is_child) {
        p_particle_obj = &(child_ptr.child->obj);
        // ParticleChild::origin is a WE world-pixel offset from the parent
        // particle. SceneNode hierarchy composes T(local) * S(parent) so
        // the local translation gets multiplied by parent scale at render
        // time; pre-divide so the world translation matches the JSON.
        Vector3f corigin(child_ptr.child->origin.data());
        for (int i = 0; i < 3; ++i) {
            float s = child_ptr.world_scale[i];
            if (std::abs(s) > 1e-6f) corigin[i] /= s;
        }
        spNodeOpt  = Some(Arc<SceneNode>::make(corigin,
                                               Vector3f(child_ptr.child->scale.data()),
                                               Vector3f(child_ptr.child->angles.data()),
                                               child_ptr.child->name));
        child_data = ChildData(*child_ptr.child);

    } else {
        p_particle_obj = &wppartobj.particleObj;
        spNodeOpt      = Some(Arc<SceneNode>::make(Vector3f(wppartobj.origin.data()),
                                                   Vector3f(wppartobj.scale.data()),
                                                   Vector3f(wppartobj.angles.data()),
                                                   wppartobj.name));
        auto& spNode   = *spNodeOpt;
        spNode->ID()   = i32(wppartobj.id);
        if (! wppartobj.visible) {
            spNode->SetVisible(false);
            context.scene->MarkLayerVisibilityElidable(
                WallpaperLayerId { .value = static_cast<i32>(wppartobj.id) });
        }
        if (! wppartobj.visible_user.empty())
            spNode->SetVisibleUserBinding(ToSceneUserVisibilityBinding(wppartobj.visible_user));
    }
    auto& spNode = *spNodeOpt;

    // Effective world scale at this SceneNode: parent's world scale times
    // this node's local scale. Propagated to child particle nodes.
    Eigen::Vector3f node_world_scale = child_ptr.world_scale.cwiseProduct(spNode->Scale());

    // The placed object's opacity/tint enters its direct child presets only. A preset's own
    // children keep their authored values instead of inheriting the scene override transitively.
    auto override_state = Arc<wpscene::ParticleInstanceoverride>::make(
        ParticleOverrideForNode(wppartobj, is_child, child_ptr.inherit_instance_override));
    auto& override = *override_state;

    auto& particle_obj = *p_particle_obj;
    auto& vfs          = *context.vfs;

    auto wppartRenderer = particle_obj.renderers.at(0);
    auto render_desc    = DescribeParticleRender(wppartRenderer);
    bool render_rope    = render_desc.rope;
    bool hastrail       = render_desc.trail;

    if (render_rope) particle_obj.material.shader = "genericropeparticle";

    // wppartobj.origin[1] = context.ortho_h - wppartobj.origin[1];

    if (particle_obj.flags[wpscene::Particle::FlagEnum::perspective]) {
        spNode->SetCamera("global_perspective");
    }

    SceneMaterial            material;
    WPUniformNodeConfigDraft svData;

    if (! is_child) {
        svData.parallax_depth = { wppartobj.parallaxDepth[0], wppartobj.parallaxDepth[1] };
        svData.propagated_parallax_depth = { wppartobj.parallaxDepth[0],
                                             wppartobj.parallaxDepth[1] };
    }
    svData.use_camera_eye_position = particle_obj.flags[wpscene::Particle::FlagEnum::perspective];

    WPShaderInfo shaderInfo;
    shaderInfo.baseConstSvs = context.global_base_uniforms;
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ORIENTATIONUP)] =
        std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ORIENTATIONRIGHT)] =
        std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ORIENTATIONFORWARD)] =
        std::array { 0.0f, 0.0f, 1.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_VIEWUP)]    = std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_VIEWRIGHT)] = std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_EYEPOSITION)] = std::array {
        static_cast<float>(context.ortho_w) / 2.0f,
        static_cast<float>(context.ortho_h) / 2.0f,
        1000.0f,
    };

    std::uint32_t maxcount = particle_obj.maxcount;
    maxcount               = std::min(maxcount, 20000u);

    if (hastrail) {
        double          in_SegmentUVTimeOffset = 0.0;
        double          in_SegmentMaxCount     = maxcount - 1.0;
        array<float, 4> render_var {
            (float)wppartRenderer.length,
            (float)wppartRenderer.maxlength,
            (float)in_SegmentUVTimeOffset,
            (float)in_SegmentMaxCount,
        };
        shaderInfo.baseConstSvs[rstd::cppstd::to_string(G_RENDERVAR0)]  = render_var;
        shaderInfo.combos[rstd::cppstd::to_string(WE_CB_TRAILRENDERER)] = "1";
        if (! render_rope) shaderInfo.combos[rstd::cppstd::to_string(WE_CB_THICK_FORMAT)] = "1";
    }
    if (render_rope) {
        std::int32_t subdiv = static_cast<std::int32_t>(std::round(wppartRenderer.subdivision));
        if (subdiv < 0) subdiv = 0;
        shaderInfo.combos["TRAILSUBDIVISION"] = std::to_string(subdiv);
    }

    auto animationmode = ToAnimMode(particle_obj.animationmode);
    if (animationmode == WPParticleAnimationMode::SEQUENCE &&
        ! particle_obj.flags[wpscene::Particle::FlagEnum::spritenoframeblending]) {
        shaderInfo.combos["SPRITESHEETBLEND"] = "1";
    }

    bool mat_ok = false;
    try {
        mat_ok = LoadMaterial(vfs,
                              context.ShaderCachePath(),
                              context.shader_environment,
                              particle_obj.material,
                              context.scene.get(),
                              &material,
                              &shaderInfo,
                              GeometryStageRequirement::Required);
    } catch (const std::exception& e) {
        rstd_error("load particleobj '{}' material exception: {}", wppartobj.name, e.what());
    }
    if (! mat_ok) {
        rstd_error("load particleobj '{}' material faild", wppartobj.name);
        return;
    }
    LoadConstvalue(material, particle_obj.material, shaderInfo);
    auto  spMesh             = std::make_shared<SceneMesh>(true);
    auto& mesh               = *spMesh;
    auto  sequencemultiplier = particle_obj.sequencemultiplier;
    bool  hasSprite          = material.hasSprite;
    (void)hasSprite;

    bool                   thick_format = material.hasSprite || (hastrail && ! render_rope);
    WPParticleFollowAnchor follow_anchor;
    if (hastrail && ! render_rope) {
        follow_anchor.trail_renderer = true;
        follow_anchor.length         = wppartRenderer.length;
        follow_anchor.max_length     = wppartRenderer.maxlength;
        follow_anchor.texture_ratio  = ParticleTextureRatio(material);
    }

    auto spawn_type = ParseSpawnType(child_data.type);
    if (is_child && spawn_type == WPParticleSubSystem::SpawnType::STATIC &&
        child_data.controlpointstartindex.is_some()) {
        spawn_type = WPParticleSubSystem::SpawnType::STATIC_CONTROLPOINT;
    }
    auto max_instance_count = u32(
        static_cast<std::uint32_t>(std::max(child_data.maxcount.to_primitive(), std::int32_t(0))));
    auto particleSub =
        Box<WPParticleSubSystem>::make(*context.scene,
                                       spMesh,
                                       u32(maxcount),
                                       f64(override.rate),
                                       max_instance_count,
                                       f64(child_data.probability),
                                       spawn_type,
                                       WPParticleAnimationSpec {
                                           .mode                = animationmode,
                                           .sequence_multiplier = sequencemultiplier,
                                       },
                                       follow_anchor,
                                       u32(),
                                       f64(),
                                       f64(static_cast<double>(particle_obj.starttime)),
                                       particle_obj.flags[wpscene::Particle::FlagEnum::wordspace]);

    {
        auto mesh_capacity = particleSub->MaxParticleCapacity();
        if (mesh_capacity.is_none()) {
            rstd_error("particle mesh capacity overflow for '{}'", spNode->Name());
            return;
        }
        auto mesh_maxcount = mesh_capacity->to_primitive();
        if (render_rope) {
            SetRopeParticleMesh(mesh, particle_obj, mesh_maxcount, thick_format);
        } else {
            SetParticleMesh(mesh, mesh_maxcount, thick_format);
        }
    }

    particleSub->SetOwnerNode(spNode.as_ptr());
    if (child_data.controlpointstartindex.is_some())
        particleSub->SetParentControlpointStartIndex(*child_data.controlpointstartindex);
    LoadEmitter(*particleSub, particle_obj, override.count);
    LoadInitializer(*particleSub, particle_obj, override_state.clone());
    LoadOperator(*particleSub, particle_obj, override_state.clone());
    LoadControlPoint(*particleSub, particle_obj, override_state.clone());
    particleSub->Finalize();

    // Register every {user:"<key>", value:...} binding on instanceoverride
    // so RenderSetUserProperty can mutate the shared state at runtime.
    for (const auto& [field, key] : override.bindings) {
        context.scene->RegisterParticleOverrideBinding(
            String::make(as_str(key).unwrap()),
            Arc<dyn<SceneParticleOverrideControl>>::make(WPParticleOverrideControl {
                .state = override_state.clone(),
                .field = String::make(as_str(field).unwrap()),
            }));
    }

    mesh.AddMaterial(std::move(material));
    RegisterShaderUserVarIndex(
        context.scene.get(), mesh.Material(), particle_obj.material, shaderInfo);
    spNode->AddMesh(spMesh);
    SetWPUniformConfig(context, spNode, rstd::move(svData));

    for (auto& child : particle_obj.children) {
        ParseParticleObj(context,
                         wppartobj,
                         {
                             .child                     = &child,
                             .node_parent               = spNode.as_ptr(),
                             .particle_parent           = particleSub.get(),
                             .inherit_instance_override = ! is_child,
                             .world_scale               = node_world_scale,
                         });
    }

    if (is_child)
        child_ptr.particle_parent->AddChild(std::move(particleSub));
    else
        context.particle_runtime->Add(rstd::move(particleSub));

    if (! is_child) AssignNodeFieldAnimations(*spNode.as_ptr(), wppartobj.field_bindings);
    WireFieldScripts(context, spNode, wppartobj.field_bindings);
    if (is_child)
        child_ptr.node_parent->AppendChild(spNode.clone());
    else {
        RegisterNodeRef(context,
                        wppartobj.id,
                        ParseContext::NodeRef {
                            wppartobj.parent,
                            Some(spNode.clone()),
                            None(),
                            String::make(rstd::cppstd::as_str(wppartobj.attachment).unwrap()),
                        });
    }
}

void ParseSoundObj(ParseContext& context, wpscene::SoundObject& obj,
                   wavsen::audio::SoundManager& sm) {
    auto node  = Arc<SceneNode>::make(Vector3f(obj.origin.data()),
                                      Vector3f(obj.scale.data()),
                                      Vector3f(obj.angles.data()),
                                      obj.name);
    node->ID() = i32(obj.id);
    if (! obj.visible) node->SetVisible(false);
    if (! obj.visible_user.empty())
        node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(obj.visible_user));

    auto control = WPSoundParser::Parse(obj, *context.vfs, sm, context.scene.get());
    if (! obj.volume_user_key.empty()) {
        context.scene->RegisterSoundVolumeBinding(
            rstd::cppstd::as_str(obj.volume_user_key).unwrap(), control.clone());
    }
    node->SetSoundControl(rstd::move(control));

    AssignNodeFieldAnimations(*node.as_ptr(), obj.field_bindings);
    WireFieldScripts(context, node, obj.field_bindings);
    RegisterNodeRef(context, obj.id, ParseContext::NodeRef { obj.parent, Some(node.clone()) });
}

void ParseLightObj(ParseContext& context, wpscene::LightObject& light_obj) {
    auto node = Arc<SceneNode>::make(Vector3f(light_obj.origin.data()),
                                     Vector3f(light_obj.scale.data()),
                                     Vector3f(light_obj.angles.data()),
                                     light_obj.name);

    SceneLight::Desc desc;
    if (light_obj.light == "spot") {
        desc.type = SceneLightType::Spot;
    } else if (light_obj.light == "directional") {
        desc.type = SceneLightType::Directional;
    } else {
        desc.type = SceneLightType::Point; // default + "point"
    }
    desc.color       = Vector3f(light_obj.color.data());
    desc.radius      = light_obj.radius;
    desc.intensity   = light_obj.intensity;
    desc.exponent    = light_obj.exponent;
    desc.attenuation = light_obj.attenuation;
    desc.mindistance = light_obj.mindistance;
    // WE cone fields are full angles in degrees; convert to cos(half-angle).
    const float kDegToRad     = rstd::f32::consts::PI.to_primitive() / 180.0f;
    desc.inner_cone_cos       = std::cos(light_obj.innercone * 0.5f * kDegToRad);
    desc.outer_cone_cos       = std::cos(light_obj.outercone * 0.5f * kDegToRad);
    desc.light_source_size    = light_obj.lightsourcesize;
    desc.cascade_distances[0] = light_obj.cascadedistance0;
    desc.cascade_distances[1] = light_obj.cascadedistance1;
    desc.cascade_distances[2] = light_obj.cascadedistance2;
    desc.cast_shadow          = light_obj.castshadow;
    desc.cast_volumetrics     = light_obj.castvolumetrics;

    auto light = context.scene->RegisterLight(Box<SceneLight>::make(desc));
    light->setNode(node.as_ptr());
    light->setRuntimeVisible(light_obj.visible);
    if (! light_obj.visible_user.empty()) {
        light->setVisibleUserBinding(ToSceneUserVisibilityBinding(light_obj.visible_user));
    }

    AssignNodeFieldAnimations(*node.as_ptr(), light_obj.field_bindings);
    WireFieldScripts(context, node, light_obj.field_bindings);
    RegisterNodeRef(
        context, light_obj.id, ParseContext::NodeRef { light_obj.parent, Some(node.clone()) });
}

void ParseModelObj(ParseContext& context, wpscene::ModelObject& model_obj) {
    auto& vfs = *context.vfs;

    WPMdl mdl;
    if (! WPMdlParser::Parse(rstd::cppstd::as_str(model_obj.model).unwrap(), vfs, mdl)) {
        rstd_error("parse model failed: {}", model_obj.model);
        return;
    }

    auto node  = Arc<SceneNode>::make(Vector3f(model_obj.origin.data()),
                                      Vector3f(model_obj.scale.data()),
                                      Vector3f(model_obj.angles.data()),
                                      model_obj.name);
    node->ID() = i32(model_obj.id);
    node->SetReflected(model_obj.reflected);
    if (! model_obj.visible) {
        node->SetVisible(false);
        context.scene->MarkLayerVisibilityElidable(
            WallpaperLayerId { .value = static_cast<i32>(model_obj.id) });
    }
    MarkHiddenLinkSource(context, model_obj.id);
    if (! model_obj.visible_user.empty())
        node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(model_obj.visible_user));

    auto mesh = std::make_shared<SceneMesh>();

    WPUniformNodeConfigDraft svData;
    svData.parallax_depth            = { model_obj.parallaxDepth[0], model_obj.parallaxDepth[1] };
    svData.propagated_parallax_depth = { model_obj.parallaxDepth[0], model_obj.parallaxDepth[1] };
    svData.use_camera_eye_position   = true;
    Option<Arc<WPPuppetLayer>> model_puppet_layer;
    if (mdl.puppet.is_some() && ! (*mdl.puppet)->bones.is_empty()) {
        model_puppet_layer = Some(
            MakePuppetLayer((*mdl.puppet).clone(),
                            std::span<WPPuppetLayer::AnimationLayer>(model_obj.puppet_layers)));
        RegisterPuppetLayer(context, node.as_ptr(), (*model_puppet_layer).clone());
    }

    for (const auto& mdl_mesh : mdl.meshes) {
        if (mdl_mesh.positions.is_empty()) continue;

        auto wpmat = WPMdlParser::ParseMaterial(mdl_mesh.mat_json_file, vfs);
        if (! wpmat) continue;
        if (mdl.puppet.is_some() && ! (*mdl.puppet)->bones.is_empty()) {
            WPMdlParser::AddPuppetMatInfo(*wpmat, mdl);
        }

        SceneMaterial scene_mat;
        WPShaderInfo  shader_info;
        shader_info.baseConstSvs = context.global_base_uniforms;
        if (mdl.puppet.is_some() && ! (*mdl.puppet)->bones.is_empty()) {
            WPMdlParser::AddPuppetShaderInfo(shader_info, mdl);
        }

        if (! LoadMaterial(vfs,
                           context.ShaderCachePath(),
                           context.shader_environment,
                           *wpmat,
                           context.scene.get(),
                           &scene_mat,
                           &shader_info)) {
            rstd_error(
                "load model material '{}' failed for '{}'", mdl_mesh.mat_json_file, model_obj.name);
            continue;
        }
        LoadConstvalue(scene_mat, *wpmat, shader_info);

        const uint32_t material_slot  = static_cast<uint32_t>(mesh->MaterialSlots().size());
        const auto     texcoord_scale = Texture0UvScale(scene_mat);
        mesh->AddMaterial(std::move(scene_mat));
        RegisterShaderUserVarIndex(
            context.scene.get(), mesh->MaterialSlots().back().get(), *wpmat, shader_info);

        mesh->Submeshes().emplace_back();
        auto& submesh = mesh->Submeshes().back();
        WPMdlParser::GenMeshFromMdl(submesh, mdl_mesh, { texcoord_scale[0], texcoord_scale[1] });
        submesh.material_slot = material_slot;
    }

    if (mesh->Submeshes().empty()) {
        rstd_error("model '{}' has no renderable mesh", model_obj.model);
        return;
    }

    node->AddMesh(mesh);
    SetWPUniformConfig(context, node, rstd::move(svData));
    AssignNodeFieldAnimations(*node.as_ptr(), model_obj.field_bindings);
    WireFieldScripts(context, node, model_obj.field_bindings);
    RegisterNodeRef(
        context,
        model_obj.id,
        ParseContext::NodeRef { model_obj.parent,
                                Some(node.clone()),
                                mdl.puppet.is_some() ? Some((*mdl.puppet).clone()) : None(),
                                String::make(rstd::cppstd::as_str(model_obj.attachment).unwrap()),
                                model_puppet_layer.is_some() ? Some((*model_puppet_layer).clone())
                                                             : None() });
}

bool EnsureTextAtlas(Scene& scene, text::FontFace& face) {
    const std::string& atlas_url = face.AtlasUrl();
    if (scene.Texture(rstd::cppstd::as_str(atlas_url).unwrap()).is_some()) return true;
    auto atlas_image = text::BuildAtlasImage(face, rstd::cppstd::as_str(atlas_url).unwrap());
    if (atlas_image.is_none()) return false;
    auto         image = rstd::move(atlas_image).unwrap_unchecked();
    SceneTexture stex;
    stex.url    = atlas_url;
    stex.sample = image->header.sample;
    scene.RegisterTexture(String::make(rstd::cppstd::as_str(atlas_url).unwrap()), rstd::move(stex));
    scene.RegisterRuntimeImage(String::make(rstd::cppstd::as_str(atlas_url).unwrap()),
                               rstd::move(image));
    face.ClearDirtyRects();
    return true;
}

auto UserPropertyValue(Option<ref<rstd::json::Map>> user_props, std::string_view key)
    -> Option<ref<Json>> {
    if (key.empty()) return None();
    auto        props   = rstd_try(user_props);
    auto        value   = rstd_try(props->get(rstd::cppstd::as_str(key).unwrap()));
    const auto& payload = SceneUserPropertyPayload(*value);
    return Some(ref<Json>::from_raw_parts(rstd::addressof(payload)));
}

void ParseTextObj(ParseContext& context, wpscene::TextObject& obj) {
    if (! obj.visible && obj.visible_user.empty()) return;
    if (! obj.visible) {
        context.scene->MarkLayerVisibilityElidable(
            WallpaperLayerId { .value = static_cast<i32>(obj.id) });
    }
    MarkHiddenLinkSource(context, obj.id);

    // --- determine initial text + whether a runtime binding will rewrite it
    auto text_binding_it      = obj.field_bindings.scripts.find("text");
    bool has_text_script      = (text_binding_it != obj.field_bindings.scripts.end());
    auto pointsize_binding_it = obj.field_bindings.scripts.find("pointsize");
    bool has_pointsize_script = (pointsize_binding_it != obj.field_bindings.scripts.end());
    // Scripts can also drive `text` indirectly: a script attached to any
    // other field (commonly `visible`) writes `thisLayer.text = "..."` from
    // its update() side-effect (e.g. workshop 2283810443's clock). Transform
    // scripts alone should not force large dynamic text RTs.
    bool has_indirect_text_script = false;
    if (! has_text_script) {
        for (const auto& [_, sb] : obj.field_bindings.scripts) {
            if (sb.source.find(".text") != std::string::npos ||
                sb.source.find("[\"text\"]") != std::string::npos ||
                sb.source.find("['text']") != std::string::npos) {
                has_indirect_text_script = true;
                break;
            }
        }
    }
    const bool has_text_user = ! obj.text_user.empty();
    bool wants_dynamic_text = has_text_script || has_indirect_text_script || has_pointsize_script ||
                              has_text_user || context.scene_layer_text_writes;
    bool has_text_effect    = false;
    for (const auto& effect : obj.effects) {
        if (effect.visible || ! effect.visible_user.empty()) {
            has_text_effect = true;
            break;
        }
    }
    const bool copy_background_seed = has_text_effect || obj.copybackground;

    std::string s_text;
    if (obj.text.is_string()) {
        s_text = rstd::cppstd::to_string(*obj.text.as_str());
    } else if (obj.text.is_object()) {
        auto value = obj.text.get("value"_str);
        if (value.is_none()) value = obj.text.get("text"_str);
        if (value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) s_text = rstd::cppstd::to_string(*string);
        }
    }
    if (has_text_user) {
        auto value = UserPropertyValue(context.user_properties, obj.text_user.name);
        if (value.is_some()) {
            auto text = SceneJsonScalarString(**value);
            if (text.is_some()) s_text = rstd::cppstd::to_string(text->as_str());
        }
    }
    if (s_text.empty() && ! wants_dynamic_text) return;

    // --- font resolution: VFS first (WE shared /assets + pkg overlay),
    //     then host system font dirs.
    std::string font_name;
    if (obj.font.is_string()) {
        font_name = rstd::cppstd::to_string(*obj.font.as_str());
    } else if (obj.font.is_object()) {
        if (auto value = obj.font.get("value"_str); value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) font_name = rstd::cppstd::to_string(*string);
        }
    }

    text::FontCache::ResolvedBlob resolved;
    // `systemfont_<family>` is WE's alias for a host system font — never exists
    // in the pkg, so skip the VFS round-trip and let fontconfig resolve it.
    // Some scenes write it with a leading dir (e.g. `fonts/systemfont_arial`),
    // so match on the basename.
    const bool is_systemfont =
        std::filesystem::path(font_name).filename().native().starts_with("systemfont_");
    if (! font_name.empty() && ! is_systemfont) {
        // scene.json's `font` is a pkg-relative path, e.g. `fonts/2.ttf` or
        // `fonts/workshop/<id>/X.otf`. The pkg mounts at /assets so the full
        // VFS path is /assets/<font_name>.
        std::string vfs_path =
            (std::filesystem::path("/assets") / font_name).lexically_normal().native();
        auto blob = fs::ReadFileContent(*context.vfs, vfs_path);
        if (blob.is_ok() && ! blob->empty()) {
            auto blob_str = rstd::move(blob).unwrap_unchecked();
            auto bytes    = std::make_shared<std::vector<std::byte>>(blob_str.size());
            std::memcpy(bytes->data(), blob_str.data(), blob_str.size());
            resolved.bytes  = std::move(bytes);
            resolved.source = vfs_path;
        }
    }
    if (! resolved.bytes) {
        resolved = text::FontCache::ResolveSystemFont(font_name, /*fallback_to_any=*/true);
    }
    if (! resolved.bytes) {
        rstd_error("text '{}': could not resolve font '{}'", obj.name, font_name);
        return;
    }

    std::uint32_t px = TextPointSizeToPx(obj.pointsize);

    auto& font_cache = text::EnsureSceneFontCache(*context.scene);
    auto* face       = font_cache.GetFace(resolved.bytes, px);
    if (face == nullptr) {
        rstd_error("text '{}': FreeType failed to open '{}'", obj.name, resolved.source);
        return;
    }

    auto shader = text::GetTextSceneShader();
    if (! shader) {
        rstd_error("text '{}': text shader compile failed", obj.name);
        return;
    }
    auto copy_background_shader =
        copy_background_seed ? text::GetTextCopyBackgroundSceneShader() : nullptr;
    if (copy_background_seed && ! copy_background_shader) {
        rstd_error("text '{}': copy-background shader compile failed", obj.name);
        return;
    }

    // Populate the seed text's glyphs up front so the first SetText has the
    // initial layout's bbox. Runtime SetText calls (from the script actuator)
    // do their own Populate of the latest string each tick.
    {
        auto seed = text::DecodeUtf8(s_text);
        face->Populate(seed);
    }

    // --- atlas-texture registration. We snapshot the per-face CPU atlas
    // (seed glyphs + the white cell) and register it with the Scene.
    // TextureCache::CreateTex will pick this up on first material bind.
    // Subsequent glyph adds emit dirty rects which the renderer re-uploads
    // each frame via TextureCache::PumpFontAtlases.
    const std::string& atlas_url = face->AtlasUrl();
    if (! EnsureTextAtlas(*context.scene, *face)) {
        rstd_error("text '{}': atlas snapshot failed", obj.name);
        return;
    }

    // --- mesh capacity. Static text exactly fits its initial layout;
    //     dynamic text reserves headroom so SetText can grow
    //     the string at runtime without reallocating GPU buffers.
    std::size_t initial_codepoints = text::DecodeUtf8(s_text).size();
    bool        has_bg             = obj.opaquebackground;
    std::size_t peak_quads;
    if (wants_dynamic_text) {
        // The glyph mesh renders into the layer RT (sized below to the same
        // ceiling), so the only quads that can ever be visible are those that
        // fit the RT grid. Budget to that cap — terminal/log scripts (e.g.
        // 2268178377) append unbounded text but the layouter clips everything
        // past the RT anyway. Conservative narrow-glyph advance avoids
        // undercounting columns for tight fonts.
        const auto& fm  = face->Metrics();
        const float adv = std::max(1.0f, static_cast<float>(fm.pixel_size) * 0.25f);
        const float lh = fm.line_height > 1.0f ? fm.line_height : static_cast<float>(fm.pixel_size);
        const float obj_w        = obj.size[0] > 0.0f ? obj.size[0] : 1024.0f;
        const float obj_h        = obj.size[1] > 0.0f ? obj.size[1] : 256.0f;
        const float rt_w         = std::max(1024.0f, obj_w * 3.0f);
        const float rt_h         = std::max(256.0f, obj_h * 2.0f);
        const std::size_t cols   = static_cast<std::size_t>(std::ceil(rt_w / adv));
        const std::size_t rows   = static_cast<std::size_t>(std::ceil(rt_h / std::max(1.0f, lh)));
        const std::size_t rt_cap = std::clamp<std::size_t>(cols * rows, 64, 16384);
        peak_quads               = std::max<std::size_t>(initial_codepoints * 4, rt_cap);
        if (has_bg) ++peak_quads;
    } else {
        peak_quads = initial_codepoints + (has_bg ? 1u : 0u);
        if (peak_quads == 0) return;
    }

    auto sp_mesh = std::make_shared<SceneMesh>(/*dynamic=*/wants_dynamic_text);
    {
        SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord, VAttr::Color }),
                                usize(peak_quads * 4));
        sp_mesh->AddVertexArray(std::move(vertex));
        sp_mesh->AddIndexArray(SceneIndexArray(usize(peak_quads * 6)));
    }
    {
        SceneMaterial material;
        material.name     = "text";
        material.textures = { atlas_url };
        material.defines  = { "g_Texture0" };
        material.blenmode = copy_background_seed ? BlendMode::Translucent : BlendMode::Normal;
        material.customShader.shader = shader;
        sp_mesh->AddMaterial(std::move(material));
    }

    // --- layouter owns the cache (FontFace lifetime) + mesh ref + style.
    text::TextLayoutStyle style;
    style.color                 = { obj.color[0], obj.color[1], obj.color[2] };
    style.alpha                 = obj.alpha;
    style.brightness            = obj.brightness;
    style.opaquebackground      = has_bg;
    style.background_color      = { obj.backgroundcolor[0],
                                    obj.backgroundcolor[1],
                                    obj.backgroundcolor[2] };
    style.background_brightness = obj.backgroundbrightness;
    style.halign                = obj.horizontalalign.empty() ? obj.alignment : obj.horizontalalign;
    style.padding               = static_cast<float>(obj.padding);

    auto align_or_default = [](std::string      value,
                               std::string_view fallback,
                               std::string_view negative,
                               std::string_view positive) {
        if (! value.empty()) return value;
        if (fallback.find(negative) != std::string::npos) return std::string(negative);
        if (fallback.find(positive) != std::string::npos) return std::string(positive);
        return std::string("center");
    };
    const std::string initial_halign =
        align_or_default(obj.horizontalalign, obj.alignment, "left", "right");
    const std::string initial_valign =
        align_or_default(obj.verticalalign, obj.alignment, "top", "bottom");
    style.halign = initial_halign;

    auto layouter = std::make_shared<text::TextLayouter>(face, sp_mesh, style, peak_quads);
    layouter->SetText(s_text);
    auto current_text       = std::make_shared<std::string>(s_text);
    auto current_point_size = std::make_shared<double>(obj.pointsize);

    auto  initial_metrics = layouter->Metrics();
    float text_w          = initial_metrics.text_width;
    float text_h          = initial_metrics.text_height;
    float text_source_w   = initial_metrics.source_width;
    float text_source_h   = initial_metrics.source_height;
    if (text_w <= 0.0f || text_h <= 0.0f) {
        // Empty seed (scripted-only text). Fake a 1×1 bbox so SceneNode /
        // parallax setup still works; the runtime actuator scales the
        // compose node to actual text dims each tick.
        initial_metrics.text_width  = 1.0f;
        initial_metrics.text_height = 1.0f;
        text_w                      = initial_metrics.text_width;
        text_h                      = initial_metrics.text_height;
    }
    if (text_source_w <= 0.0f) initial_metrics.source_width = text_w;
    if (text_source_h <= 0.0f) initial_metrics.source_height = text_h;
    text_source_w = initial_metrics.source_width;
    text_source_h = initial_metrics.source_height;

    auto sp_node = Arc<SceneNode>::make(
        Vector3f(obj.origin.data()), Vector3f(obj.scale.data()), Vector3f(obj.angles.data()));
    const float text_bbox_w = text_w + 2.0f * style.padding;
    const float text_bbox_h = text_h + 2.0f * style.padding;
    sp_node->SetSize({ text_bbox_w, text_bbox_h });
    sp_node->AddMesh(sp_mesh);

    // sp_node renders into the layer's private ortho RT. Parallax must NOT
    // apply at this stage — the world-space mouse vector would shift glyphs
    // inside ppong_a, but the compose pass samples a fixed UV window, so the
    // shift would manifest as the text appearing to drift in the wrong frame
    // of reference. Parallax goes on compose_node below (world-space quad).
    context.text_uniform_configs.push(ParseContext::TextUniformConfigDraft {
        .node = sp_node.clone(),
    });

    // --- per-layer compose -------------------------------------------------
    // Render the glyphs into a private bbox-sized RT via an ortho camera
    // that maps text-mesh pixel coords 1:1 onto the RT, then composite that
    // RT onto _rt_default with a Translucent fullscreen-quad pass. The glyph
    // pass writes straight RGBA into ppong_a; composing applies alpha once.
    //
    // Glyphs render immediately before compose_node. Attaching the layer
    // camera to sp_node cancels parent transforms inside the private RT.
    struct TextAnchorState {
        std::string horizontal;
        std::string vertical;
        Vector3f    origin;
        float       width { 1.0f };
        float       height { 1.0f };
    };
    auto anchor_state = std::make_shared<TextAnchorState>(TextAnchorState {
        .horizontal = initial_halign,
        .vertical   = initial_valign,
        .origin     = Vector3f(obj.origin.data()),
        .width      = text_w,
        .height     = text_h,
    });

    auto compose_node =
        Arc<SceneNode>::make(Vector3f::Zero(), Vector3f::Ones(), Vector3f::Zero(), obj.name);
    // Layer RT must cover the source glyph bounds, not the main canvas.
    // Clock/date scripts often render a large text source and shrink it with
    // the scene transform when composing into the world.
    const float                    object_w = obj.size[0] > 0.0f ? obj.size[0] : text_bbox_w;
    const float                    object_h = obj.size[1] > 0.0f ? obj.size[1] : text_bbox_h;
    const text::TextGeometryPolicy geometry_policy {
        .frame_width        = object_w,
        .frame_height       = object_h,
        .dynamic            = wants_dynamic_text,
        .has_effect         = has_text_effect,
        .preserve_text_bbox = has_bg || obj.copybackground,
    };
    const auto initial_geometry = text::ResolveTextGeometry(geometry_policy, layouter->Metrics());
    const auto [initial_layer_w, initial_layer_h] = TextLayerExtent(initial_geometry);
    auto& scene                                   = *context.scene;
    auto  runtime_targets                         = std::make_shared<TextRuntimeTargets>(
        scene, mut_ref<WPUniformSceneState>::from_raw_parts(context.uniform_state.as_ptr()));
    {
        const std::string addr    = getAddr(sp_node.as_ptr());
        const std::string ppong_a = rstd::cppstd::to_string(OWE_EFFECT_PPONG_PREFIX_A) + addr;
        const std::string ppong_b = rstd::cppstd::to_string(OWE_EFFECT_PPONG_PREFIX_B) + addr;
        const std::string effect_final =
            rstd::cppstd::to_string(OWE_EFFECT_PPONG_PREFIX_A) + "text_final_" + addr;
        runtime_targets->camera_key   = addr;
        runtime_targets->ppong_a      = ppong_a;
        runtime_targets->ppong_b      = ppong_b;
        runtime_targets->effect_final = effect_final;
        runtime_targets->has_effect   = has_text_effect;
        runtime_targets->layer_w      = initial_layer_w;
        runtime_targets->layer_h      = initial_layer_h;

        // Per-layer ortho camera. effect_camera_node sits at origin so the
        // view matrix is identity; ortho extents = bbox so glyph pixel
        // coords (centered around 0) map directly to [-1, +1] NDC.
        auto text_camera = Arc<SceneCamera>::make(
            SceneCamera::MakeOrthographic(initial_layer_w, initial_layer_h, -1.0, 1.0));
        text_camera->AttatchNode(sp_node.as_ptr());
        scene.RegisterCamera(String::make(rstd::cppstd::as_str(addr).unwrap()),
                             text_camera.clone());

        SceneRenderTarget text_target {
            .width                = initial_layer_w,
            .height               = initial_layer_h,
            .allowReuse           = true,
            .force_clear          = ! copy_background_seed,
            .clear_on_first_write = false,
            .preserve_on_write    = copy_background_seed,
        };
        if (has_text_effect) {
            scene.RegisterRenderTarget(String::make(as_str(ppong_a).unwrap()), text_target);
            scene.RegisterRenderTarget(String::make(as_str(ppong_b).unwrap()), text_target);
            scene.RegisterRenderTarget(String::make(as_str(effect_final).unwrap()),
                                       rstd::move(text_target));
        } else {
            scene.RegisterRenderTarget(String::make(as_str(ppong_a).unwrap()),
                                       rstd::move(text_target));
        }

        compose_node->CopyTrans(*sp_node.as_ptr());
        compose_node->ID() = i32(obj.id);
        compose_node->SetSize({ initial_geometry.draw_width, initial_geometry.draw_height });
        scene.RegisterLayerLinkSource(WallpaperLayerId { .value = static_cast<i32>(obj.id) },
                                      *sp_node.as_ptr());

        auto layer = std::make_shared<SceneImageEffectLayer>(has_text_effect ? compose_node.as_ptr()
                                                                             : sp_node.as_ptr(),
                                                             static_cast<float>(initial_layer_w),
                                                             static_cast<float>(initial_layer_h),
                                                             ppong_a,
                                                             has_text_effect ? ppong_b : ppong_a);
        text_camera->AttatchImgEffect(layer);

        if (copy_background_seed) {
            auto bg_node = Arc<SceneNode>::make();
            bg_node->SetCamera("effect");
            auto bg_mesh = std::make_shared<SceneMesh>();
            bg_mesh->ChangeMeshDataFrom(*scene.DefaultEffectMesh());
            SceneMaterial bg_material;
            bg_material.name                = "text_copybackground";
            bg_material.textures            = { rstd::cppstd::to_string(SpecTex_Default) };
            bg_material.defines             = { "g_Texture0" };
            bg_material.blenmode            = BlendMode::Normal;
            bg_material.customShader.shader = copy_background_shader;
            bg_mesh->AddMaterial(std::move(bg_material));
            bg_node->AddMesh(bg_mesh);

            auto text_projection =
                std::make_shared<text::TextEffectProjectionState>(text::TextEffectProjectionState {
                    .node = compose_node.clone(),
                    .size = { initial_geometry.effect_frame_width,
                              initial_geometry.effect_frame_height },
                });
            context.text_uniform_configs.push(ParseContext::TextUniformConfigDraft {
                .node              = bg_node.clone(),
                .effect_projection = text_projection,
            });
            runtime_targets->effect_nodes.push_back(TextRuntimeEffectNode {
                .node = bg_node.as_ptr(), .text_projection = rstd::move(text_projection) });
            layer->AddPrefillNode(SceneImageEffectNode {
                .output    = ppong_a,
                .sceneNode = bg_node.clone(),
            });
        }

        WPUniformNodeConfigDraft compose_sv;
        compose_sv.parallax_depth            = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_sv.propagated_parallax_depth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };

        ShaderValueMap effect_base = NeutralColorUniforms(context.global_base_uniforms);

        struct LoadedTextMaterial {
            wpscene::Material        source;
            SceneMaterial            material;
            WPUniformNodeConfigDraft sv;
            WPShaderInfo             shader_info;
        };
        auto load_passthrough_material =
            [&](std::string_view input) -> std::optional<LoadedTextMaterial> {
            auto pt_json =
                LoadJsonFile(*context.vfs, "/assets/materials/util/effectpassthrough.json");
            if (! pt_json) {
                rstd_error("text '{}': parse effectpassthrough.json failed", obj.name);
                return std::nullopt;
            }
            wpscene::Material pt_mat;
            if (! pt_mat.FromJson(*pt_json)) {
                rstd_error("text '{}': Material::FromJson failed", obj.name);
                return std::nullopt;
            }
            if (pt_mat.textures.empty())
                pt_mat.textures.push_back(std::string(input));
            else
                pt_mat.textures[0] = std::string(input);

            SceneMaterial            mat;
            WPUniformNodeConfigDraft sv;
            WPShaderInfo             si;
            si.baseConstSvs = effect_base;
            if (! LoadMaterial(*context.vfs,
                               context.ShaderCachePath(),
                               context.shader_environment,
                               pt_mat,
                               &scene,
                               &mat,
                               &si)) {
                rstd_error("text '{}': compose LoadMaterial failed", obj.name);
                return std::nullopt;
            }
            LoadConstvalue(mat, pt_mat, si);
            mat.blenmode = BlendMode::Translucent;
            return LoadedTextMaterial {
                .source      = std::move(pt_mat),
                .material    = std::move(mat),
                .sv          = std::move(sv),
                .shader_info = std::move(si),
            };
        };

        if (has_text_effect) {
            SceneMaterial final_state;
            final_state.blenmode    = BlendMode::Normal;
            final_state.depth_test  = false;
            final_state.depth_write = false;
            layer->SetFullscreen(true);
            layer->SetFinalTarget(effect_final);
            layer->SetFinalMaterialState(final_state);

            for (const auto& wpeffobj : obj.effects) {
                if (! wpeffobj.visible && wpeffobj.visible_user.empty()) continue;

                auto effect             = std::make_shared<SceneImageEffect>();
                effect->name            = wpeffobj.name;
                effect->runtime_visible = wpeffobj.visible;
                if (! wpeffobj.visible_user.empty()) {
                    effect->visible_user_binding =
                        ToSceneUserVisibilityBinding(wpeffobj.visible_user);
                }

                const std::string                            inRT { ppong_a };
                std::unordered_map<std::string, std::string> fboMap;
                fboMap["previous"] = inRT;

                const std::string effaddr = getAddr(layer.get());
                for (const auto& wpfbo : wpeffobj.fbos) {
                    const std::string rtname =
                        rstd::str_::starts_with(as_str(wpfbo.name).unwrap(), WE_SPEC_PREFIX)
                            ? wpfbo.name + "_" + effaddr
                            : rstd::cppstd::to_string(WE_SPEC_PREFIX) + wpfbo.name + "_" + effaddr;
                    auto fbo_size = TextEffectFboExtent(initial_geometry, wpfbo.scale, wpfbo.fit);
                    scene.RegisterRenderTarget(String::make(as_str(rtname).unwrap()),
                                               SceneRenderTarget { .width      = fbo_size[0],
                                                                   .height     = fbo_size[1],
                                                                   .allowReuse = ! wpfbo.unique,
                                                                   .clear_on_first_write = true });
                    fboMap[wpfbo.name] = rtname;
                    runtime_targets->fbos.push_back(TextRuntimeFbo {
                        .name  = rtname,
                        .scale = wpfbo.scale,
                        .fit   = wpfbo.fit,
                    });
                }

                for (const auto& cmd : wpeffobj.commands) {
                    if (cmd.command != "copy") {
                        rstd_error("Unknown effect command: {}", cmd.command);
                        continue;
                    }
                    if (fboMap.count(cmd.target) + fboMap.count(cmd.source) < 2) {
                        rstd_error(
                            "Unknown effect command dst or src: {} {}", cmd.target, cmd.source);
                        continue;
                    }
                    effect->commands.push_back({ .cmd      = SceneImageEffect::CmdType::Copy,
                                                 .dst      = fboMap[cmd.target],
                                                 .src      = fboMap[cmd.source],
                                                 .afterpos = cmd.afterpos });
                }

                bool effect_ok = true;
                for (std::size_t i_mat = 0; i_mat < wpeffobj.materials.size(); ++i_mat) {
                    wpscene::Material wpmat = wpeffobj.materials.at(i_mat).clone();
                    std::string matOutRT { rstd::cppstd::to_string(OWE_EFFECT_PPONG_PREFIX_B) };
                    std::optional<wpscene::Material> user_texture_fallback;
                    if (wpeffobj.passes.size() > i_mat) {
                        const auto& pass = wpeffobj.passes.at(i_mat);
                        wpmat.MergePass(pass);
                        ApplyTextureBinds(wpmat, std::span(pass.bind), fboMap);
                        user_texture_fallback = wpmat.clone();
                        ApplyUserTextureBindings(context, wpmat);
                        if (! pass.target.empty()) {
                            if (fboMap.count(pass.target) == 0)
                                rstd_error("fbo {} not found", pass.target);
                            else
                                matOutRT = fboMap.at(pass.target);
                        }
                    }
                    for (auto& tex : wpmat.textures) {
                        if (ParseImageLayerCompositeId(as_str(tex).unwrap()) ==
                            static_cast<std::uint32_t>(obj.id))
                            tex = ppong_a;
                    }
                    if (wpmat.textures.empty()) wpmat.textures.resize(1);
                    if (wpmat.textures.at(0).empty()) wpmat.textures[0] = inRT;

                    auto         effect_node = Arc<SceneNode>::make();
                    WPShaderInfo shader_info;
                    shader_info.baseConstSvs = effect_base;
                    shader_info.baseConstSvs[rstd::cppstd::to_string(G_ETVP)] =
                        ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                    shader_info.baseConstSvs[rstd::cppstd::to_string(G_ETVPI)] =
                        ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());

                    SceneMaterial            mat;
                    WPUniformNodeConfigDraft sv;
                    sv.propagate_parallax_to_children = true;
                    sv.propagated_parallax_depth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
                    sv.parallax_depth            = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
                    sv.effect_projection_node    = Some(compose_node.clone());
                    sv.effect_projection_size    = { initial_geometry.effect_frame_width,
                                                     initial_geometry.effect_frame_height };
                    SceneShaderValueAnimationMap final_quad_shader_values;
                    if (! LoadMaterial(*context.vfs,
                                       context.ShaderCachePath(),
                                       context.shader_environment,
                                       wpmat,
                                       &scene,
                                       &mat,
                                       &shader_info)) {
                        effect_ok = false;
                        break;
                    }
                    LoadConstvalue(mat, wpmat, shader_info, &final_quad_shader_values);

                    auto mesh = std::make_shared<SceneMesh>();
                    mesh->AddMaterial(std::move(mat));
                    RegisterShaderUserVarIndex(&scene, mesh->Material(), wpmat, shader_info);
                    if (user_texture_fallback.has_value()) {
                        RegisterMaterialUserTextureIndex(
                            &scene, mesh->Material(), *user_texture_fallback, shader_info);
                    }
                    effect_node->AddMesh(mesh);
                    SetWPUniformConfig(context, effect_node, rstd::move(sv));
                    runtime_targets->effect_nodes.push_back(
                        TextRuntimeEffectNode { .node = effect_node.as_ptr() });
                    effect->nodes.push_back(SceneImageEffectNode {
                        .output                   = matOutRT,
                        .sceneNode                = effect_node.clone(),
                        .uses_unit_final_quad     = UsesUnitFinalQuad(wpmat),
                        .final_quad_shader_values = std::move(final_quad_shader_values),
                    });
                }

                if (effect_ok)
                    layer->AddEffect(effect);
                else
                    rstd_error("effect '{}' failed to load", wpeffobj.name);
            }

            auto resolve_node = Arc<SceneNode>::make();
            auto resolved     = load_passthrough_material(ppong_a);
            if (! resolved.has_value()) return;
            auto resolve_mesh = std::make_shared<SceneMesh>();
            resolve_mesh->AddMaterial(std::move(resolved->material));
            resolve_node->AddMesh(std::move(resolve_mesh));
            SetWPUniformConfig(context, resolve_node, rstd::move(resolved->sv));
            runtime_targets->effect_nodes.push_back(
                TextRuntimeEffectNode { .node = resolve_node.as_ptr() });
            auto resolve_effect  = std::make_shared<SceneImageEffect>();
            resolve_effect->name = "text_resolve";
            resolve_effect->nodes.push_back(SceneImageEffectNode {
                .output    = ppong_b,
                .sceneNode = resolve_node.clone(),
            });
            layer->SetFinalResolveEffect(std::move(resolve_effect));
        }

        auto compose_mesh = std::make_shared<SceneMesh>(/*dynamic=*/wants_dynamic_text);
        GenCardMesh(*compose_mesh,
                    { static_cast<float>(runtime_targets->layer_w),
                      static_cast<float>(runtime_targets->layer_h) });
        auto loaded = load_passthrough_material(has_text_effect ? effect_final : ppong_a);
        if (! loaded.has_value()) return;
        compose_sv                           = std::move(loaded->sv);
        compose_sv.parallax_depth            = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_sv.propagated_parallax_depth = { obj.parallaxDepth[0], obj.parallaxDepth[1] };
        compose_mesh->AddMaterial(std::move(loaded->material));
        RegisterShaderUserVarIndex(
            &scene, compose_mesh->Material(), loaded->source, loaded->shader_info);
        compose_node->AddMesh(compose_mesh);
        SetWPUniformConfig(context, compose_node, rstd::move(compose_sv));

        // Move sp_node into layer space — identity transform so the glyph
        // mesh renders at the ortho origin.
        sp_node->CopyTrans(SceneNode());
        sp_node->SetCamera(addr);
    }

    auto compose_hold      = SceneNodeArcHold(compose_node.clone());
    auto apply_text_anchor = [compose_hold, anchor_state]() {
        auto* compose_ptr = compose_hold.get();
        auto  contains    = [](const std::string& value, std::string_view token) {
            return value.find(token) != std::string::npos;
        };
        const auto& scale = compose_ptr->Scale();
        Vector3f    pos   = anchor_state->origin;
        if (contains(anchor_state->horizontal, "left"))
            pos.x() += anchor_state->width * scale.x() * 0.5f;
        if (contains(anchor_state->horizontal, "right"))
            pos.x() -= anchor_state->width * scale.x() * 0.5f;
        if (contains(anchor_state->vertical, "top"))
            pos.y() -= anchor_state->height * scale.y() * 0.5f;
        if (contains(anchor_state->vertical, "bottom"))
            pos.y() += anchor_state->height * scale.y() * 0.5f;
        compose_ptr->SetTranslate(pos);
    };

    // Per-frame compose-quad rebuild: world card sized to current visible
    // source bbox. The quad offset keeps glyphs at their logical text-box
    // position after the private RT path centers them for UV cropping.
    auto rebuild_compose = [compose_hold,
                            anchor_state,
                            apply_text_anchor,
                            runtime_targets,
                            geometry_policy,
                            text_padding = style.padding](text::TextLayoutMetrics metrics) {
        auto* compose_ptr   = compose_hold.get();
        metrics.padding     = text_padding;
        const auto geometry = text::ResolveTextGeometry(geometry_policy, metrics);
        (void)runtime_targets->Apply(geometry);
        anchor_state->width  = metrics.text_width;
        anchor_state->height = metrics.text_height;
        compose_ptr->SetSize({ geometry.draw_width, geometry.draw_height });
        apply_text_anchor();
        const float                  hx = geometry.draw_width * 0.5f;
        const float                  hy = geometry.draw_height * 0.5f;
        const float                  cx = geometry.draw_offset_x;
        const float                  cy = geometry.draw_offset_y;
        const rstd::array<float, 12> pos {
            cx - hx, cy - hy, 0.0f, cx - hx, cy + hy, 0.0f,
            cx + hx, cy - hy, 0.0f, cx + hx, cy + hy, 0.0f,
        };
        const float u_half =
            0.5f * std::min(1.0f, geometry.uv_source_width / float(runtime_targets->layer_w));
        const float v_half =
            0.5f * std::min(1.0f, geometry.uv_source_height / float(runtime_targets->layer_h));
        const float                 u_l = 0.5f - u_half;
        const float                 u_r = 0.5f + u_half;
        const float                 v_t = 0.5f - v_half;
        const float                 v_b = 0.5f + v_half;
        const rstd::array<float, 8> uv {
            u_l, v_b, u_l, v_t, u_r, v_b, u_r, v_t,
        };
        auto* mesh = compose_ptr->Mesh();
        if (mesh == nullptr) return;
        auto& v = mesh->GetVertexArray(usize(0));
        v.SetVertex(as_string_view(WE_IN_POSITION), pos.as_slice());
        v.SetVertex(as_string_view(WE_IN_TEXCOORD), uv.as_slice());
        mesh->SetDirty();
    };
    rebuild_compose(initial_metrics);

    auto apply_text_origin = [anchor_state, apply_text_anchor](const script::ScriptValue& value) {
        Vector3f current = anchor_state->origin;
        auto     next    = ScriptValueAsVec3(value, current);
        if (! next) return;
        anchor_state->origin = *next;
        apply_text_anchor();
    };
    auto apply_text_scale = [compose_hold, apply_text_anchor](const script::ScriptValue& value) {
        auto*    compose_ptr = compose_hold.get();
        Vector3f current     = compose_ptr->Scale();
        auto     next        = ScriptValueAsVec3(value, current);
        if (! next) return;
        compose_ptr->SetScale(*next);
        apply_text_anchor();
    };

    auto set_halign = [layouter, rebuild_compose, anchor_state](std::string_view align) {
        anchor_state->horizontal = std::string(align);
        layouter->SetHorizontalAlign(align);
        rebuild_compose(layouter->Metrics());
    };
    auto set_valign = [anchor_state, apply_text_anchor](std::string_view align) {
        anchor_state->vertical = std::string(align);
        apply_text_anchor();
    };
    auto set_pointsize = [scene          = context.scene.get(),
                          font_cache_ptr = &font_cache,
                          font_blob      = resolved.bytes,
                          sp_mesh,
                          layouter,
                          rebuild_compose,
                          current_text,
                          current_point_size](double next_point_size) {
        if (scene == nullptr || font_cache_ptr == nullptr || ! std::isfinite(next_point_size) ||
            next_point_size <= 0.0) {
            return;
        }
        auto* next_face = font_cache_ptr->GetFace(
            font_blob, TextPointSizeToPx(static_cast<float>(next_point_size)));
        if (next_face == nullptr) return;
        next_face->Populate(text::DecodeUtf8(*current_text));
        if (! EnsureTextAtlas(*scene, *next_face)) return;
        *current_point_size = next_point_size;
        if (auto* mat = sp_mesh->Material()) {
            (void)scene->SetMaterialTextureSlot(*mat, u32(), next_face->AtlasUrl());
        }
        layouter->SetFace(next_face);
        rebuild_compose(layouter->Metrics());
    };

    EnsureScriptScene(context).runtime().RegisterTextAlignSetters(
        compose_node.as_ptr(),
        anchor_state->horizontal,
        anchor_state->vertical,
        obj.pointsize,
        set_halign,
        set_valign,
        [current_point_size]() {
            return *current_point_size;
        },
        set_pointsize);

    // Transform-style script bindings (origin/scale/angles) animate the
    // composite quad in world space, not the layer-space glyph node.
    AssignNodeFieldAnimations(*compose_node.as_ptr(), obj.field_bindings);
    WireFieldScripts(
        context, compose_node, obj.field_bindings, apply_text_origin, apply_text_scale);
    if (! obj.visible) compose_node->SetVisible(false);
    if (! obj.visible_user.empty())
        compose_node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(obj.visible_user));

    // --- text-content actuator. Captures the layouter + a closure that
    // re-rasterises new codepoints, lays them out, and rebuilds the
    // compose quad to the new text dims. Runs on the render thread, which
    // is also the JS thread — no synchronization needed.
    auto set_text = [layouter, rebuild_compose, current_text](std::string_view s) {
        if (s == *current_text) return;
        *current_text = std::string(s);
        if (auto* active_face = layouter->Face()) active_face->Populate(text::DecodeUtf8(s));
        layouter->SetText(s);
        rebuild_compose(layouter->Metrics());
    };
    if (has_text_user) {
        context.scene->RegisterUserTextBinding(
            String::make(as_str(obj.text_user.name).unwrap()),
            Box<dyn<FnMut<void(ref<str>)>>>::make([set_text](ref<str> value) mutable {
                set_text(as_string_view(value));
            }));
    }
    if (has_text_script) {
        const auto& sb  = text_binding_it->second;
        auto&       ss  = EnsureScriptScene(context);
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::String,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       compose_node.as_ptr());
        if (fs) {
            SetScriptInitializationOrder(context, *fs, compose_node.as_ptr());
            ss.AddActuator({
                fs,
                [set_text](const script::ScriptValue& v) {
                    if (auto* p = std::get_if<script::StringValue>(&v)) set_text(p->s);
                },
            });
        }
    }
    if (has_pointsize_script) {
        const auto& sb  = pointsize_binding_it->second;
        auto&       ss  = EnsureScriptScene(context);
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::Scalar,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       compose_node.as_ptr());
        if (fs) {
            SetScriptInitializationOrder(context, *fs, compose_node.as_ptr());
            ss.AddActuator({
                fs,
                [set_pointsize](const script::ScriptValue& v) {
                    auto scalar = ScriptValueAsFloat(v);
                    if (scalar) set_pointsize(*scalar);
                },
            });
        }
    }
    // Scripts attached to non-text fields can mutate `thisLayer.text`
    // directly. Register the setter so NodeSetText routes those writes
    // back into the layouter. compose_node is the SceneNode every
    // field-bound script's `thisLayer` resolves to (WireFieldScripts at
    // line above).
    if (wants_dynamic_text) {
        EnsureScriptScene(context).runtime().RegisterTextSetter(compose_node.as_ptr(),
                                                                [set_text](std::string_view s) {
                                                                    set_text(s);
                                                                });
    }

    Vec<Arc<SceneNode>> text_before_nodes;
    text_before_nodes.push(sp_node.clone());
    RegisterNodeRef(context,
                    obj.id,
                    ParseContext::NodeRef {
                        obj.parent,
                        Some(compose_node.clone()),
                        None(),
                        String::make(rstd::cppstd::as_str(obj.attachment).unwrap()),
                        None(),
                        Some(Box<dyn<FnMut<void(const Vector3f&)>>>::make(
                            [anchor_state, apply_text_anchor](const Vector3f& offset) {
                                anchor_state->origin += offset;
                                apply_text_anchor();
                            })),
                        rstd::move(text_before_nodes),
                    });

    const char* scripted_tag = has_text_script            ? " [scripted]"
                               : has_indirect_text_script ? " [scripted-indirect]"
                                                          : "";
    rstd_info("text '{}': initial=\"{}\" px={} peak_quads={} bbox={}x{}{} ({})",
              obj.name,
              s_text,
              px,
              peak_quads,
              static_cast<int>(text_w),
              static_cast<int>(text_h),
              std::string_view(scripted_tag),
              resolved.source);
}

bool ResolveVisibleUserBinding(bool& visible, const wpscene::VisibleUserBinding& binding,
                               Option<ref<rstd::json::Map>> user_props) {
    if (binding.empty()) return false;
    auto value = UserPropertyValue(user_props, binding.name);
    if (value.is_some()) {
        if (auto resolved =
                ResolveSceneUserVisibilityBinding(ToSceneUserVisibilityBinding(binding), **value))
            visible = *resolved;
    }
    return true;
}

struct ObjectVisibilityInfo {
    std::uint32_t parent { 0 };
    bool          visible { true };
    bool          user_bound { false };
};

ObjectVisibilityInfo ResolveObjectVisibility(const Json&                  json_obj,
                                             Option<ref<rstd::json::Map>> user_props) {
    ObjectVisibilityInfo info;
    owe::GetJsonValue(json_obj, "parent", info.parent, false);
    wpscene::VisibleUserBinding binding;
    wpscene::ReadVisibleProperty(json_obj, info.visible, binding);
    info.user_bound = ! binding.empty();
    ResolveVisibleUserBinding(info.visible, binding, user_props);
    return info;
}

HashMap<std::int32_t, ObjectVisibilityInfo>
BuildObjectVisibilityInfo(const Json& json, Option<ref<rstd::json::Map>> user_props) {
    HashMap<std::int32_t, ObjectVisibilityInfo> out;
    auto                                        objects = json.get("objects"_str);
    if (objects.is_none()) return out;
    auto array = (*objects)->as_array();
    if (array.is_none()) return out;
    for (const auto& obj : **array) {
        if (! obj.is_object()) continue;
        std::int32_t id {};
        if (! owe::GetJsonValue(obj, "id", id, false)) continue;
        (void)out.insert(id, ResolveObjectVisibility(obj, user_props));
    }
    return out;
}

bool HasHiddenUserAncestor(std::uint32_t                                      id,
                           const HashMap<std::int32_t, ObjectVisibilityInfo>& objects) {
    HashSet<std::uint32_t> seen;
    auto                   object = objects.get(static_cast<std::int32_t>(id));
    if (object.is_none()) return false;
    std::uint32_t parent = (**object).parent;
    while (parent != 0 && seen.insert(parent)) {
        auto parent_object = objects.get(static_cast<std::int32_t>(parent));
        if (parent_object.is_none()) return false;
        if ((**parent_object).user_bound && ! (**parent_object).visible) return true;
        parent = (**parent_object).parent;
    }
    return false;
}

HashSet<std::int32_t> CollectHiddenLinkedSourceIds(const Json&                  json,
                                                   const HashSet<std::int32_t>& linked_source_ids,
                                                   Option<ref<rstd::json::Map>> user_props) {
    HashSet<std::int32_t> out;
    auto                  visibility_info = BuildObjectVisibilityInfo(json, user_props);
    linked_source_ids.iter().for_each([&](ref<std::int32_t> linked_id) {
        const auto id   = *linked_id;
        auto       info = visibility_info.get(id);
        if (info.is_none()) return;
        if (! (**info).visible ||
            HasHiddenUserAncestor(static_cast<std::uint32_t>(id), visibility_info)) {
            out.insert(id);
        }
    });
    return out;
}

SceneObjectVar MakeSceneObject(wpscene::ImageObject value) {
    return SceneObjectVar::Image(rstd::move(value));
}

SceneObjectVar MakeSceneObject(wpscene::ParticleObject value) {
    return SceneObjectVar::Particle(rstd::move(value));
}

SceneObjectVar MakeSceneObject(wpscene::SoundObject value) {
    return SceneObjectVar::Sound(rstd::move(value));
}

SceneObjectVar MakeSceneObject(wpscene::LightObject value) {
    return SceneObjectVar::Light(rstd::move(value));
}

SceneObjectVar MakeSceneObject(wpscene::TextObject value) {
    return SceneObjectVar::Text(rstd::move(value));
}

SceneObjectVar MakeSceneObject(wpscene::ModelObject value) {
    return SceneObjectVar::Model(rstd::move(value));
}

SceneObjectVar MakeSceneObject(wpscene::CameraObject value) {
    return SceneObjectVar::Camera(rstd::move(value));
}

template<typename T>
void AddSceneObject(Vec<SceneObjectVar>& objs, const Json& json_obj, fs::VFS& vfs,
                    wpscene::SceneVersion v, Option<ref<rstd::json::Map>> user_props,
                    Option<ref<HashSet<std::int32_t>>> linked_source_ids, bool force_invisible) {
    T scene_obj;
    if (! scene_obj.FromJson(json_obj, vfs, v)) {
        rstd_error("parse scene object failed, name: {}", scene_obj.name);
        return;
    }
    ResolveVisibleUserBinding(scene_obj.visible, scene_obj.visible_user, user_props);
    if constexpr (std::is_same_v<T, wpscene::ImageObject>) {
        for (auto& effect : scene_obj.effects)
            ResolveVisibleUserBinding(effect.visible, effect.visible_user, user_props);
    }
    if (force_invisible) scene_obj.visible = false;
    const bool preserve_hidden_link_source =
        ! scene_obj.visible && linked_source_ids.is_some() &&
        (*linked_source_ids)->contains(static_cast<std::int32_t>(scene_obj.id));
    const bool preserve_hidden_user_bound = ! scene_obj.visible && ! scene_obj.visible_user.empty();
    const bool preserve_hidden_visible_script =
        ! scene_obj.visible && scene_obj.field_bindings.scripts.count("visible") != 0;
    // Image objects keep going even when visible=false: another layer's
    // material may reference them via `_rt_imageLayerComposite_<id>`. The
    // render-graph builder later decides whether to actually emit passes.
    if constexpr (! std::is_same_v<T, wpscene::ImageObject>) {
        constexpr bool preserve_user_visibility = ! std::is_same_v<T, wpscene::SoundObject>;
        if (! scene_obj.visible && ! preserve_hidden_link_source &&
            ! (preserve_user_visibility &&
               (preserve_hidden_user_bound || preserve_hidden_visible_script)))
            return;
        if (preserve_hidden_link_source) scene_obj.visible = true;
    }
    objs.push(MakeSceneObject(rstd::move(scene_obj)));
}
} // namespace

namespace owe
{

Vec<SceneObjectVar> ExpandObjects(const Json& json, fs::VFS& vfs, wpscene::SceneVersion v,
                                  Option<ref<rstd::json::Map>>       user_props,
                                  Option<ref<HashSet<std::int32_t>>> linked_source_ids) {
    Vec<SceneObjectVar> scene_objs;
    auto                objects = json.get("objects"_str);
    if (objects.is_none()) return scene_objs;
    auto array = (*objects)->as_array();
    if (array.is_none()) return scene_objs;
    auto visibility_info = BuildObjectVisibilityInfo(json, user_props);
    for (const auto& obj : **array) {
        bool                 force_invisible = false;
        Option<std::int32_t> id;
        if (obj.is_object()) {
            std::int32_t value {};
            if (owe::GetJsonValue(obj, "id", value, false)) id = Some(value);
        }
        if (id.is_some()) {
            force_invisible =
                HasHiddenUserAncestor(static_cast<std::uint32_t>(*id), visibility_info);
        }
        // Order matters: text/model/camera kinds coexist with null
        // image/particle/sound/light fields, so the renderer-supported
        // kinds get first pick. Falls through to the parsing-only kinds
        // (no rendering yet) so the data stays absorbed.
        if (auto value = obj.get("image"_str); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::ImageObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("particle"_str); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::ParticleObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("sound"_str); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::SoundObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("light"_str); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::LightObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("text"_str); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::TextObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("model"_str); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::ModelObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        } else if (auto value = obj.get("camera"_str); value.is_some() && ! (*value)->is_null()) {
            AddSceneObject<wpscene::CameraObject>(
                scene_objs, obj, vfs, v, user_props, linked_source_ids, force_invisible);
        }
    }
    return scene_objs;
}

array<std::int32_t, 2> ResolveOrthoProjectionExtent(const wpscene::SceneMetadata& sc,
                                                    slice<SceneObjectVar>         scene_objs) {
    std::int32_t w = sc.general.orthogonalprojection.width;
    std::int32_t h = sc.general.orthogonalprojection.height;
    if (! sc.general.orthogonalprojection.auto_) return { w, h };
    w = 0;
    h = 0;
    for (usize index {}; index < scene_objs.len(); ++index) {
        const auto& object = scene_objs[index];
        if (! object.is_Image()) continue;
        const auto& img  = object.as_Image().value;
        auto        size = static_cast<std::int32_t>(img.size.at(0) * img.size.at(1));
        if (size > w * h) {
            w = static_cast<std::int32_t>(img.size.at(0));
            h = static_cast<std::int32_t>(img.size.at(1));
        }
    }
    return { w, h };
}

ParseContext BuildContext(fs::VFS& vfs, ref<str> scene_id, const wpscene::SceneMetadata& sc,
                          array<std::int32_t, 2>       ortho_extent,
                          Option<ref<rstd::json::Map>> user_properties,
                          Option<rstd::path::PathBuf>  shader_cache_dir) {
    ParseContext context;
    InitContext(context, vfs, sc, ortho_extent);
    ParseCamera(context, sc);
    context.user_properties = user_properties;
    if (shader_cache_dir.is_some()) {
        context.shader_cache_directory =
            Some(WPShaderCacheDirectory(rstd::move(shader_cache_dir).unwrap_unchecked()));
    }

    context.scene->RegisterRenderTarget(String::make(SpecTex_Default),
                                        SceneRenderTarget {
                                            .width             = context.ortho_w,
                                            .height            = context.ortho_h,
                                            .withDepth         = true,
                                            .bind              = { .enable = true, .screen = true },
                                            .preserve_on_write = true,
                                        });
    context.scene->RegisterRenderTarget(
        String::make(WE_MIP_MAPPED_FRAME_BUFFER),
        SceneRenderTarget {
            .width      = context.ortho_w,
            .height     = context.ortho_h,
            .has_mipmap = true,
            .bind       = { .enable = true, .name = rstd::cppstd::to_string(SpecTex_Default) },
        });

    context.scene->SetSceneId(String::make(scene_id));
    return context;
}

std::unordered_map<std::string, std::vector<owe::SceneNode*>>
SpawnCreateLayerAssetClones(ParseContext& context, std::int32_t owner_id, ref<str> source) {
    constexpr unsigned                                            pool_size = 8;
    std::unordered_map<std::string, std::vector<owe::SceneNode*>> out;
    const auto source_view = rstd::cppstd::as_string_view(source);
    if (source_view.find("createLayer") == std::string_view::npos) return out;

    auto owner = context.node_id_map.get(owner_id);
    if (owner.is_none() || (**owner).node.is_none()) return out;

    for (const auto& asset : DetectRegisteredAssets(source_view)) {
        if (! sstart_with(asset, "models/") || ! asset.ends_with(".json")) continue;
        auto size = ResolveImageAssetSize(context, asset);
        if (! size) continue;
        auto& nodes = out[asset];
        nodes.reserve(pool_size);
        for (unsigned i = 0; i < pool_size; ++i) {
            wpscene::ImageObject image;
            auto size_str = std::to_string((*size)[0]) + " " + std::to_string((*size)[1]);
            auto object   = rstd::json::Map::make();
            auto set      = [&](std::string_view key, Json value) {
                object.insert(::alloc::string::String::make(rstd::cppstd::as_str(key).unwrap()),
                              rstd::move(value));
            };
            set("id", rstd::into<Json>(i32(context.next_dynamic_layer_id--)));
            set("name", JsonFromStd("__createLayer:" + asset));
            set("image", JsonFromStd(asset));
            set("origin", JsonFromStd("0 0 0"));
            set("angles", JsonFromStd("0 0 0"));
            set("scale", JsonFromStd("1 1 1"));
            set("size", JsonFromStd(size_str));
            set("visible", rstd::into<Json>(true));
            auto json = Json::Object(rstd::move(object));
            if (! image.FromJson(json, *context.vfs)) continue;
            ParseImageObj(context, image);
            auto found = context.node_id_map.get(image.id);
            if (found.is_none() || (**found).node.is_none()) continue;
            auto node = (*(**found).node).clone();
            node->SetVisible(false);
            nodes.push_back(node.as_ptr());
            AddLayerClone(context, owner_id, rstd::move(node));
            (void)context.node_id_map.remove(image.id);
        }
        if (nodes.empty()) out.erase(asset);
    }
    return out;
}

void ResolveCreateLayerAssetRequests(ParseContext& context) {
    for (auto& req : context.create_layer_asset_requests) {
        if (! req.script) continue;
        auto queues = SpawnCreateLayerAssetClones(context, req.owner_id, req.source.as_str());
        for (auto& [asset, nodes] : queues) {
            req.script->AddAssetCloneQueue(std::move(asset), std::move(nodes));
        }
    }
    context.create_layer_asset_requests.clear();
}

void ProcessObjects(ParseContext& context, mut_ref<SceneObjectVar[]> scene_objs,
                    wavsen::audio::SoundManager* sm, ProcessOpts opts,
                    SceneLoadBenchRecorderView load_bench) {
    WPShaderParser::InitGlslang();
    IndexSystemMediaImageFallbacks(context, scene_objs.as_ref());

    for (usize index {}; index < scene_objs.len(); ++index) {
        auto& object = scene_objs[index];
        if (object.is_Image()) {
            if (! (opts.kinds & ProcessOpts::Image)) continue;
            auto span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::parse_object_image);
            ParseImageObj(context, object.as_Image().value);
        } else if (object.is_Particle()) {
            if (! (opts.kinds & ProcessOpts::Particle)) continue;
            auto span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::parse_object_particle);
            ParseParticleObj(context, object.as_Particle().value);
        } else if (object.is_Sound()) {
            if (! (opts.kinds & ProcessOpts::Sound) || ! sm) continue;
            auto span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::parse_object_sound);
            ParseSoundObj(context, object.as_Sound().value, *sm);
        } else if (object.is_Light()) {
            if (! (opts.kinds & ProcessOpts::Light)) continue;
            auto span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::parse_object_light);
            ParseLightObj(context, object.as_Light().value);
        } else if (object.is_Text()) {
            if (! (opts.kinds & ProcessOpts::Text)) continue;
            auto span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::parse_object_text);
            ParseTextObj(context, object.as_Text().value);
        } else if (object.is_Model()) {
            if (! (opts.kinds & ProcessOpts::Model)) continue;
            auto span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::parse_object_model);
            ParseModelObj(context, object.as_Model().value);
        } else {
            auto span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::parse_object_camera);
            ParseCameraObj(context, object.as_Camera().value);
        }
    }

    ResolveCreateLayerAssetRequests(context);
    WPShaderParser::FinalGlslang();
}

void FinalizeUniformSources(ParseContext& context) {
    auto& scene = *context.scene;
    scene.RebuildResourceIndex();

    auto active_camera = scene.ActiveCameraHandle();
    if (active_camera.is_none()) return;
    auto camera_for = [&](const SceneNode& node) -> Option<Arc<SceneCamera>> {
        if (! node.Camera().empty()) {
            return scene.CameraHandle(rstd::cppstd::as_str(node.Camera()).unwrap());
        }
        if (node.Perspective()) {
            return scene.CameraHandle("global_perspective"_str);
        }
        return Some((*active_camera).clone());
    };
    auto camera_resolver = Arc<WPUniformCameraResolver>::make((*active_camera).clone());
    auto camera_names    = scene.CameraNames();
    camera_resolver->Reserve(camera_names.len());
    for (usize index {}; index < camera_names.len(); ++index) {
        const auto& name   = camera_names[index];
        auto        camera = scene.CameraHandle(name.as_str());
        if (camera.is_some()) camera_resolver->Add(name.clone(), rstd::move(*camera));
    }

    auto ortho = scene.Ortho();
    context.uniform_state->SetOrtho(static_cast<float>(ortho[usize()].to_primitive()),
                                    static_cast<float>(ortho[usize(1)].to_primitive()));
    scene.Runtime().RegisterSystem(WPUniformRuntimeSystem { context.uniform_state.clone() });

    auto registrar = dyn<UniformSourceRegistrar>::from_ref(scene);
    auto writer    = dyn<UniformAttachmentWriter>::from_ref(scene);

    const auto frame_source = registrar->Register(
        Box<dyn<UniformSource>>::make(WPFrameUniformSource { context.uniform_state.clone() }));
    const auto audio_source = registrar->Register(
        Box<dyn<UniformSource>>::make(WPAudioUniformSource { context.uniform_state.clone() }));
    (void)writer->AttachGlobal(frame_source, 0);
    (void)writer->AttachGlobal(audio_source, 0);

    Vec<ref<SceneLight>> lights;
    auto                 owned_lights = scene.Lights();
    lights.reserve(owned_lights.len());
    for (usize index {}; index < owned_lights.len(); ++index)
        lights.push(owned_lights[index].as_ref());
    const auto light_source = registrar->Register(
        Box<dyn<UniformSource>>::make(WPLightUniformSource { rstd::move(lights) }));
    (void)writer->AttachGlobal(light_source, 0);

    for (auto& draft : context.text_uniform_configs) {
        auto node_id = scene.ResourceIndex().nodeId(*draft.node);
        if (node_id.is_none()) continue;
        auto state               = std::make_shared<text::TextUniformState>(draft.node.clone());
        state->camera            = camera_for(*draft.node);
        state->active_camera     = Some((*active_camera).clone());
        state->effect_projection = draft.effect_projection;
        const auto source        = registrar->Register(
            Box<dyn<UniformSource>>::make(text::TextUniformSource { rstd::move(state) }));
        (void)writer->AttachNode(*node_id, source, 0);
    }

    for (auto& entry : context.uniform_configs) {
        auto& draft   = entry.config;
        auto  node_id = scene.ResourceIndex().nodeId(*entry.node);
        if (node_id.is_none()) continue;

        auto state = Arc<WPUniformNodeState>::make(entry.node.clone(), camera_resolver.clone());
        state->propagated_parallax_depth      = draft.propagated_parallax_depth;
        state->propagate_parallax_to_children = draft.propagate_parallax_to_children;
        state->use_camera_eye_position        = draft.use_camera_eye_position;
        state->effect_projection_size         = draft.effect_projection_size;
        if (draft.effect_projection_node.is_some()) {
            state->effect_projection_node = Some((*draft.effect_projection_node).clone());
        }
        context.uniform_state->SetNodeState(*node_id, state.clone());

        const auto transform = registrar->Register(Box<dyn<UniformSource>>::make(
            WPTransformUniformSource { context.uniform_state.clone(), rstd::move(state) }));
        const auto color     = registrar->Register(
            Box<dyn<UniformSource>>::make(WPColorUniformSource { entry.node.clone() }));
        const auto texture =
            registrar->Register(Box<dyn<UniformSource>>::make(WPTextureUniformSource {}));
        (void)writer->AttachNode(*node_id, transform, 0);
        (void)writer->AttachNode(*node_id, color, 0);
        (void)writer->AttachNode(*node_id, texture, 0);
    }

    for (auto& draft : context.particle_trail_uniform_configs) {
        auto node_id = scene.ResourceIndex().nodeId(*draft.node);
        if (node_id.is_none()) continue;
        const auto source = registrar->Register(Box<dyn<UniformSource>>::make(
            WPParticleTrailUniformSource { rstd::move(draft.uniform_state) }));
        (void)writer->AttachNode(*node_id, source, 10);
    }

    std::unordered_map<WPPuppetLayer*, UniformSourceId> puppet_sources;
    context.puppet_layers->by_node.iter().for_each([&](auto entry) {
        auto [node_ref, layer_ref] = entry;
        auto*       node           = *node_ref;
        const auto& layer          = *layer_ref;
        if (node == nullptr) return;
        auto node_id = scene.ResourceIndex().nodeId(*node);
        if (node_id.is_none()) return;
        auto [source, inserted] = puppet_sources.try_emplace(layer.as_ptr().as_raw_ptr());
        if (inserted) {
            source->second = registrar->Register(
                Box<dyn<UniformSource>>::make(WPPuppetUniformSource { layer.clone() }));
        }
        (void)writer->AttachNode(*node_id, source->second, 10);
    });
}

Box<Scene> FinalizeScene(ParseContext& context) {
    // Single attach phase. Each registered node was created in JSON
    // declaration order (node_id_order) but not yet inserted into the scene
    // graph. Walk that order and AppendChild to parent (or root). Result:
    // child lists at every depth match scene.json declaration order, which
    // is what WE treats as z-order.
    int attached = 0, missing_parent = 0;
    for (auto id : context.node_id_order) {
        auto found = context.node_id_map.get_mut(id);
        if (found.is_none() || (**found).node.is_none()) continue;
        auto&                        ref         = **found;
        SceneNode*                   parent_node = context.scene->RootMut().as_raw_ptr();
        const ParseContext::NodeRef* parent_ref  = nullptr;
        if (ref.parent_id != 0) {
            auto parent = context.node_id_map.get(static_cast<std::int32_t>(ref.parent_id));
            if (parent.is_none() || (**parent).node.is_none()) {
                missing_parent++;
                continue;
            }
            parent_node = (*(**parent).node).as_ptr();
            parent_ref  = &**parent;
        }
        // Named MDAT anchors provide the child's local frame in the parent
        // puppet's bind space.
        if (! ref.attachment.is_empty() && parent_ref && parent_ref->puppet.is_some()) {
            const auto& puppet           = **parent_ref->puppet;
            auto        attachment_index = puppet.attachmentIndex(ref.attachment.as_str());
            if (attachment_index.is_some()) {
                auto apply_bind_offset = [&]() {
                    auto anchor = puppet.attachmentBindTransform(*attachment_index);
                    if (anchor.is_none()) return;
                    if (ref.apply_attachment_offset.is_some()) {
                        (*ref.apply_attachment_offset)->operator()(anchor->translation());
                    } else {
                        (*ref.node)->SetLocalFrame(anchor->matrix().cast<double>() *
                                                   (*ref.node)->LocalFrame());
                    }
                };
                if (ref.apply_attachment_offset.is_none() && parent_ref->puppet_layer.is_some()) {
                    SceneNode* node       = (*ref.node).as_ptr();
                    auto       layer      = CopyableArcHold((*parent_ref->puppet_layer).clone());
                    auto       local_base = node->LocalFrame();
                    auto       update     = [node,
                                             layer,
                                             attachment_index = *attachment_index,
                                             local_base       = rstd::move(local_base)](f64 time) {
                        auto anchor =
                            layer.value->attachmentTransform(attachment_index, time.to_primitive());
                        if (anchor.is_none()) return;
                        node->SetLocalFrame(anchor->matrix().cast<double>() * local_base);
                    };
                    update(context.scene->Runtime().Frame().elapsed);
                    context.scene->RegisterTransformUpdater(
                        Box<dyn<FnMut<void(f64)>>>::make(rstd::move(update)));
                } else {
                    apply_bind_offset();
                }
            }
        }
        for (auto& before_node : ref.ordered_before_nodes) {
            parent_node->AppendChild(before_node.clone());
        }
        parent_node->AppendChild((*ref.node).clone());
        attached++;

        // Attach this layer's fanout clones (audio bars) right after it, so
        // all bars sit at the template's z-position in the parent child list.
        if (auto clones = context.layer_clones.get_mut(id); clones.is_some()) {
            for (auto& clone : **clones) {
                parent_node->AppendChild(rstd::move(clone));
                attached++;
            }
        }
    }
    rstd_info("attach: {}/{} nodes ({} missing parents)",
              attached,
              context.node_id_map.len(),
              missing_parent);

    // If any object during the visit installed a script binding, hand the
    // ScriptScene off to the Scene now. The renderer ticks it once per
    // frame via owe::script::TickSceneScripts. Empty ScriptScenes are
    // skipped so image-only pkgs don't pay any runtime cost.
    if (context.script_scene.is_some() && ! (*context.script_scene)->empty()) {
        // Hand the scene root to the JS runtime so `thisScene.getLayer(name)`
        // can resolve against the live graph. The renderer also ticks the
        // ScriptScene once per frame via owe::script::TickSceneScripts.
        (*context.script_scene)->runtime().SetScene(context.scene.get());
        (*context.script_scene)->runtime().SetSceneRoot(context.scene->RootMut().as_raw_ptr());
        owe::script::InstallScriptScene(*context.scene,
                                        context.script_scene.take().unwrap_unchecked());
    }
    if (context.particle_runtime.is_some()) {
        context.scene->Runtime().RegisterSystem(context.particle_runtime.take().unwrap(),
                                                SceneRuntimeSchedule::BeforeRender);
    }
    if (context.shader_cache_directory.is_some()) {
        context.scene->InstallExtension(Box<WPShaderCacheDirectory>::make(
            rstd::move(context.shader_cache_directory).unwrap_unchecked()));
    }
    FinalizeUniformSources(context);
    return rstd::move(context.scene);
}

void BuildBloomPostProcess(ParseContext& context, fs::VFS& vfs, const wpscene::SceneGeneral& g) {
    auto& scene = *context.scene;

    auto declare_rt = [&](std::string name, float inv_scale) {
        SceneRenderTarget rt {};
        rt.width       = 2;
        rt.height      = 2;
        rt.allowReuse  = true;
        rt.bind.enable = true;
        rt.bind.screen = true;
        rt.bind.scale  = inv_scale;
        scene.RegisterRenderTarget(String::make(as_str(name).unwrap()), rstd::move(rt));
    };
    declare_rt("_rt_bloom_mip1", g.hdr ? 0.5f : 0.25f);
    declare_rt("_rt_bloom_mip2", 0.25f);
    declare_rt("_rt_bloom_combine", 1.0f);

    const std::unordered_map<std::string, std::string> fboMap {
        { "previous", rstd::cppstd::to_string(SpecTex_Default) },
        { "_rt_default", rstd::cppstd::to_string(SpecTex_Default) },
        { "_rt_bloom_mip1", "_rt_bloom_mip1" },
        { "_rt_bloom_mip2", "_rt_bloom_mip2" },
        { "_rt_bloom_combine", "_rt_bloom_combine" },
    };

    auto pp  = Box<ScenePostProcess>::make();
    pp->name = "__bloom";

    auto add_pass = [&](const char* mat_relpath,
                        std::vector<wpscene::MaterialPassBindItem>
                                                                binds,
                        std::string                             output_rt,
                        std::function<void(wpscene::Material&)> mutate         = nullptr,
                        std::function<void(WPShaderInfo&)>      configure_info = nullptr) -> bool {
        auto jMat = LoadJsonFile(vfs, std::string("/assets/") + mat_relpath);
        if (! jMat) {
            rstd_error("bloom: parse material json failed {}", mat_relpath);
            return false;
        }
        wpscene::Material wpmat;
        if (! wpmat.FromJson(*jMat)) {
            rstd_error("bloom: Material::FromJson failed: {}", mat_relpath);
            return false;
        }
        ApplyTextureBinds(wpmat, std::span(binds), fboMap);
        if (mutate) mutate(wpmat);

        WPShaderInfo wpShaderInfo;
        wpShaderInfo.baseConstSvs = context.global_base_uniforms;
        if (configure_info) configure_info(wpShaderInfo);

        auto                     pp_node = Arc<SceneNode>::make();
        SceneMaterial            material;
        WPUniformNodeConfigDraft svData;
        if (! LoadMaterial(vfs,
                           context.ShaderCachePath(),
                           context.shader_environment,
                           wpmat,
                           &scene,
                           &material,
                           &wpShaderInfo)) {
            rstd_error("bloom: LoadMaterial failed: {}", mat_relpath);
            return false;
        }
        LoadConstvalue(material, wpmat, wpShaderInfo);

        auto pp_mesh = std::make_shared<SceneMesh>();
        pp_mesh->ChangeMeshDataFrom(*scene.DefaultEffectMesh());
        pp_mesh->AddMaterial(std::move(material));
        RegisterShaderUserVarIndex(&scene, pp_mesh->Material(), wpmat, wpShaderInfo);
        pp_node->AddMesh(pp_mesh);

        // Camera name drives CustomShaderPass color-write mask: empty or
        // "global" cameras strip the A bit (intent: swapchain ignores A
        // for direct local display). But waywallen DMA-BUF forwarding
        // negotiates COLOR_ALPHA_PREMUL; if A=0 reaches the consumer with
        // non-zero RGB, KWin reads it as premultiplied-transparent and
        // composites additively against the desktop -> washed-out tint.
        // Anchor to the existing "effect" cam (2x2 ortho, identity for
        // our NDC fullscreen quads) so A=1.0 from the shader survives.
        pp_node->SetCamera("effect");
        SetWPUniformConfig(context, pp_node, rstd::move(svData));

        pp->steps.push(ScenePostProcessStep::Pass(ScenePostProcessPass {
            .node   = rstd::move(pp_node),
            .output = std::move(output_rt),
        }));
        return true;
    };

    if (g.hdr) {
        auto hdr_offsets = [](float source_scale) {
            float x = 1.0f / (1920.0f * source_scale);
            float y = 1.0f / (1080.0f * source_scale);
            return std::array { x, y, -x, -y };
        };
        auto set_render_var = [](WPShaderInfo& info, std::array<float, 4> value) {
            info.baseConstSvs[rstd::cppstd::to_string(G_RENDERVAR0)] = value;
        };
        float threshold = g.bloomhdrthreshold;
        float knee      = threshold * g.bloomhdrfeather;
        float scatter   = g.bloomhdrscatter > 0.0f ? g.bloomhdrscatter : 1.0f;

        if (! add_pass(
                "materials/util/hdr_downsample_bloom.json",
                { { "previous", 0 } },
                "_rt_bloom_mip1",
                [&](wpscene::Material& m) {
                    m.constantshadervalues["bloomstrength"] = { g.bloomhdrstrength };
                    m.constantshadervalues["blend"]         = {
                        threshold,
                        threshold - knee,
                        2.0f * knee,
                        knee > 0.0f ? 0.25f / knee : 0.0f,
                    };
                    m.constantshadervalues["bloomtint"] = {
                        g.bloomtint[0],
                        g.bloomtint[1],
                        g.bloomtint[2],
                    };
                },
                [&](WPShaderInfo& info) {
                    set_render_var(info, hdr_offsets(1.0f));
                }))
            return;

        if (! add_pass("materials/util/hdr_downsample.json",
                       { { "_rt_bloom_mip1", 0 } },
                       "_rt_bloom_mip2",
                       nullptr,
                       [&](WPShaderInfo& info) {
                           set_render_var(info, hdr_offsets(0.5f));
                       }))
            return;

        if (! add_pass(
                "materials/util/hdr_upsample.json",
                { { "_rt_bloom_mip2", 0 } },
                "_rt_bloom_mip1",
                [&](wpscene::Material& m) {
                    m.constantshadervalues["scatter"] = { scatter };
                },
                [&](WPShaderInfo& info) {
                    set_render_var(info, hdr_offsets(0.25f));
                }))
            return;

        if (! add_pass("materials/util/combine_hdr_upsample_linear.json",
                       { { "previous", 0 }, { "_rt_bloom_mip1", 1 } },
                       "_rt_bloom_combine",
                       nullptr,
                       [&](WPShaderInfo& info) {
                           set_render_var(info, { 1.0f, 0.0f, 0.0f, 0.0f });
                       }))
            return;
    } else {
        if (! add_pass("materials/util/downsample_quarter_bloom.json",
                       { { "previous", 0 } },
                       "_rt_bloom_mip1",
                       [&](wpscene::Material& m) {
                           m.constantshadervalues["bloomstrength"]  = { g.bloomstrength };
                           m.constantshadervalues["bloomthreshold"] = { g.bloomthreshold };
                           m.constantshadervalues["bloomtint"]      = {
                               g.bloomtint[0],
                               g.bloomtint[1],
                               g.bloomtint[2],
                           };
                       }))
            return;

        if (! add_pass("materials/util/downsample_eighth_blur_v.json",
                       { { "_rt_bloom_mip1", 0 } },
                       "_rt_bloom_mip2"))
            return;

        if (! add_pass(
                "materials/util/blur_h_bloom.json", { { "_rt_bloom_mip2", 0 } }, "_rt_bloom_mip1"))
            return;

        if (! add_pass("materials/util/combine_ldr.json",
                       { { "previous", 0 }, { "_rt_bloom_mip1", 1 } },
                       "_rt_bloom_combine"))
            return;
    }

    pp->steps.push(ScenePostProcessStep::Copy(ScenePostProcessCopy {
        .src = "_rt_bloom_combine",
        .dst = rstd::cppstd::to_string(SpecTex_Default),
    }));

    (void)scene.RegisterPostProcess(rstd::move(pp));
}

} // namespace owe

auto WPSceneParser::Parse(ref<str> scene_id, ref<wpscene::SceneDocument> document,
                          mut_ref<fs::VFS> vfs, mut_ref<wavsen::audio::SoundManager> sound,
                          SceneParseOptions options) -> Result<ParsedScene, SceneParseError> {
    auto&       vfs_ref    = *vfs.as_raw_ptr();
    auto&       sound_ref  = *sound.as_raw_ptr();
    auto        total_span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_total);
    const auto& json       = document->root_json;
    const auto& sc         = document->metadata;
    rstd_info("scene: pkg_version={} scene_json_version={}",
              static_cast<unsigned>(sc.pkg_version),
              static_cast<unsigned>(sc.scene_json_version));

    auto linked_source_ids = CollectLinkedSourceIdsFromJson(json);
    auto scene_objs        = [&] {
        auto span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_expand_objects);
        return ExpandObjects(
            json,
            vfs_ref,
            sc.pkg_version,
            options.user_properties,
            Some(ref<HashSet<std::int32_t>>::from_raw_parts(rstd::addressof(linked_source_ids))));
    }();
    auto       context_span  = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_context);
    const auto ortho_extent  = ResolveOrthoProjectionExtent(sc, scene_objs.as_slice());
    auto       context       = BuildContext(vfs_ref,
                                            scene_id,
                                            sc,
                                            ortho_extent,
                                            options.user_properties,
                                            rstd::move(options.shader_cache_dir));
    auto       runtime_input = Arc<WPUniformRuntimeInput>::make(context.uniform_state.clone());
    context.scene_layer_text_writes = SceneWritesLayerText(scene_objs.as_slice());
    context.hidden_link_source_ids =
        CollectHiddenLinkedSourceIds(json, linked_source_ids, options.user_properties);

    // Single JSON-order walk:
    // - record every object's id (and parent_id) in declaration order so the
    //   final attach phase can rebuild the scene tree with matching child
    //   ordering — z-order in WE is JSON declaration order.
    // - for transform-only "container" layers (no image/particle/sound/light/
    //   text/model/camera field, e.g. workshop 3327063360's "组件"), create the
    //   bare SceneNode here so ParseImageObj children can find their parent.
    //   Their `visible:false` form is preserved as a parent anchor.
    if (auto objects = json.get("objects"_str); objects.is_some()) {
        auto object_array = (*objects)->as_array();
        if (object_array.is_none()) {
            return Err(SceneParseError {
                .kind    = SceneParseErrorKind::ObjectExpansion,
                .message = String::make("scene objects must be an array"_str),
            });
        }
        auto visibility_info = BuildObjectVisibilityInfo(json, options.user_properties);
        auto has_kind        = [](const Json& o) {
            for (auto key : rstd::array<ref<str>, 7> { "image"_str,
                                                       "particle"_str,
                                                       "sound"_str,
                                                       "light"_str,
                                                       "text"_str,
                                                       "model"_str,
                                                       "camera"_str }) {
                if (auto value = o.get(key); value.is_some() && ! (*value)->is_null()) return true;
            }
            return false;
        };
        auto read_vec3 = [](const Json& o, const char* key, std::array<float, 3>& out) {
            owe::GetJsonValue(o, key, out, false);
        };
        for (const auto& o : **object_array) {
            if (! o.is_object()) continue;
            std::int32_t id {};
            if (! owe::GetJsonValue(o, "id", id, false)) continue;
            (void)context.script_initialization_orders.insert(
                id, static_cast<std::uint64_t>(context.node_id_order.len().to_primitive()));
            context.node_id_order.emplace_back(id);
            std::uint32_t parent = 0;
            owe::GetJsonValue(o, "parent", parent, false);
            (void)context.object_parent_ids.insert(id, parent);
            bool solid = false;
            owe::GetJsonValue(o, "solid", solid, false);
            if (solid) context.solid_layer_ids.insert(id);

            if (has_kind(o)) continue;
            std::string name;
            owe::GetJsonValue(o, "name", name, false);
            std::array<float, 3> origin { 0, 0, 0 }, scale { 1, 1, 1 }, angles { 0, 0, 0 };
            read_vec3(o, "origin", origin);
            read_vec3(o, "scale", scale);
            read_vec3(o, "angles", angles);
            auto node = Arc<SceneNode>::make(
                Vector3f(origin.data()), Vector3f(scale.data()), Vector3f(angles.data()), name);
            node->ID() = i32(id);
            std::array<float, 2> parallax_depth { 0.0f, 0.0f };
            bool                 disable_propagation = false;
            owe::GetJsonValue(o, "parallaxDepth", parallax_depth, false);
            owe::GetJsonValue(o, "disablepropagation", disable_propagation, false);
            if (parallax_depth[0] != 0.0f || parallax_depth[1] != 0.0f || disable_propagation) {
                WPUniformNodeConfigDraft sv_data;
                sv_data.propagate_parallax_to_children = ! disable_propagation;
                sv_data.parallax_depth                 = { parallax_depth[0], parallax_depth[1] };
                sv_data.propagated_parallax_depth      = { parallax_depth[0], parallax_depth[1] };
                SetWPUniformConfig(context, node, rstd::move(sv_data));
            }
            auto visibility = visibility_info.get(id);
            if (visibility.is_some()) {
                bool visible =
                    (**visibility).visible &&
                    ! HasHiddenUserAncestor(static_cast<std::uint32_t>(id), visibility_info);
                if (! visible) (void)context.scene->SetNodeVisible(*node, false);
            }
            wpscene::VisibleUserBinding visible_user;
            wpscene::ReadVisibleUserBinding(o, visible_user);
            if (! visible_user.empty())
                node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(visible_user));
            wpscene::FieldBindings fb;
            wpscene::AbsorbAllFieldBindings(o, fb);
            WireFieldScripts(context, node, fb);
            std::string attachment;
            owe::GetJsonValue(o, "attachment", attachment, false);
            RegisterNodeRef(
                context,
                id,
                ParseContext::NodeRef { parent,
                                        Some(node.clone()),
                                        None(),
                                        String::make(rstd::cppstd::as_str(attachment).unwrap()),
                                        None() });
        }
    }
    (void)context_span.finish();

    {
        auto span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_objects);
        ProcessObjects(
            context, scene_objs.as_mut_slice().as_mut_ref(), &sound_ref, {}, options.load_bench);
    }

    {
        auto span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_post_process);
        if (sc.general.bloom) {
            BuildBloomPostProcess(context, vfs_ref, sc.general);
        }
    }
    auto finalize_span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_finalize);
    return Ok(ParsedScene {
        .scene         = FinalizeScene(context),
        .runtime_input = rstd::move(runtime_input),
    });
}
