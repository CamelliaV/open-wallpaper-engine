export module wescene.scene;
import eigen;
import rstd;
import wescene.core;
import wescene.json;
import rstd.cppstd;
import wescene.types;
import wescene.spec_names;

// SceneLight + SceneLightType live in this partition. Re-exported here so
// existing `import wescene.scene` consumers see them transparently.
export import :visibility;
export import :lighting;

using namespace rstd::prelude;

export namespace owe
{

// ============================================================================
// SceneShader.h
// ============================================================================

using ShaderValueInter = rstd::array<float, 16>;

class ShaderValue {
public:
    using value_type = float;

public:
    ShaderValue()  = default;
    ~ShaderValue() = default;

    ShaderValue(const ShaderValue&)            = default;
    ShaderValue& operator=(const ShaderValue&) = default;

    ShaderValue(const value_type& value) noexcept { fromSpan(spanone { value }); }
    template<typename Range>
    ShaderValue(const Range& range) noexcept {
        fromSpan(range);
    }
    ShaderValue(const value_type* ptr, usize num) noexcept { fromSpan({ ptr, num }); }

    static ShaderValue fromMatrix(const Eigen::Ref<const Eigen::MatrixXf>& mat) {
        return ShaderValue(std::span { mat.data(), static_cast<usize>(mat.size()) });
    }
    static ShaderValue fromMatrix(const Eigen::Ref<const Eigen::MatrixXd>& mat) {
        const Eigen::Ref<const Eigen::MatrixXf>& matf = mat.cast<float>();
        return fromMatrix(matf);
    };
    const auto& operator[](usize index) const { return _value()[index]; }
    auto&       operator[](usize index) { return m_dynamic ? m_dvalue[index] : m_value[index]; }

    auto  data() const noexcept { return _value().data(); };
    usize size() const noexcept { return m_size; };

    void setSize(usize v) noexcept { m_size = std::min(v, _value().size()); }

private:
    void fromSpan(std::span<const value_type> s) noexcept;

    std::span<const value_type> _value() const noexcept {
        if (m_dynamic) return m_dvalue;
        return m_value;
    }
    bool                    m_dynamic { false };
    ShaderValueInter        m_value;
    std::vector<value_type> m_dvalue;
    usize                   m_size { 0 };
};

using ShaderValues   = Map<std::string, ShaderValue>;
using ShaderValueMap = ShaderValues;
using ShaderCode     = std::vector<unsigned int>;

struct ShaderAttribute {
public:
    std::string name;
    u32         location;
};

struct SceneShader {
public:
    u32         id { 0 };
    std::string name;

    std::vector<ShaderCode> codes;

    std::vector<ShaderAttribute> attrs;
    ShaderValues                 default_uniforms;
};

inline usize SceneShaderStageCodeHash(const ShaderCode& code) {
    usize seed { 0 };
    utils::hash_combine(seed, code.size());
    for (auto word : code) utils::hash_combine(seed, word);
    return seed;
}

inline usize SceneShaderCodeHash(std::span<const ShaderCode> codes) {
    usize seed { 0 };
    utils::hash_combine(seed, codes.size());
    for (const auto& code : codes) utils::hash_combine(seed, SceneShaderStageCodeHash(code));
    return seed;
}

inline usize SceneShaderCodeHash(const SceneShader& shader) {
    return SceneShaderCodeHash(shader.codes);
}

struct SceneShaderTextureCompileInfo {
    bool                 enabled { false };
    rstd::array<bool, 3> components { false, false, false };
};

struct SceneShaderVariantStage {
    ShaderType                    stage { ShaderType::VERTEX };
    std::string                   source_key;
    std::string                   source;
    Set<unsigned>                 active_texture_slots;
    Map<std::string, std::string> uniforms;
    usize                         code_hash { 0 };
};

struct SceneShaderDefaultTexture {
    i32         slot { 0 };
    std::string texture;
};

struct SceneShaderVariantDesc {
    std::string scene_id;
    std::string shader_name;

    Map<std::string, std::string>          input_combos;
    Map<std::string, std::string>          resolved_combos;
    Map<std::string, std::string>          uniform_aliases;
    ShaderValues                           default_uniforms;
    std::vector<SceneShaderDefaultTexture> default_textures;
    std::vector<std::string>               texture_slots;

    std::vector<SceneShaderTextureCompileInfo> texture_infos;
    std::vector<SceneShaderVariantStage>       stages;
    usize                                      descriptor_layout_hash { 0 };
    bool                                       geometry_shader_enabled { false };

    bool Valid() const { return ! shader_name.empty() && ! stages.empty(); }
};

// ============================================================================
// SceneTexture.h
// ============================================================================

struct SceneTexture {
    std::string     url;
    TextureSample   sample;
    bool            isSprite { false };
    SpriteAnimation spriteAnim;
};

// ============================================================================
// SceneRenderTarget.h
// ============================================================================

struct SceneRenderTarget {
    struct Bind {
        bool        enable { false };
        std::string name {};
        bool        screen { false };
        double      scale { 1.0 };
    };

    i32 width;
    i32 height;
    // Keep authored layout dimensions separate from backend-limited allocation dimensions.
    i32      physical_width { 0 };
    i32      physical_height { 0 };
    bool     allowReuse { false };
    bool     withDepth { false };
    bool     has_mipmap { false };
    unsigned mipmap_level { 1 };
    // 1 disables MSAA; only screen RT is opted-in by VulkanRender at init.
    unsigned      sample_count { 1 };
    TextureSample sample { TextureWrap::CLAMP_TO_EDGE,
                           TextureWrap::CLAMP_TO_EDGE,
                           TextureFilter::LINEAR,
                           TextureFilter::LINEAR };
    Bind          bind {};

    // Force VK_ATTACHMENT_LOAD_OP_CLEAR with transparent clear color on
    // every pass that writes to this RT. Needed for per-layer compose RTs
    // where the only writer is a Translucent draw and so the default
    // load-op (LOAD) would leak the previous frame's pixels through —
    // visible as ghosting when the rendered string changes (e.g. clock
    // text "12:00" → "12:01" leaves "12:00"'s glyphs underneath).
    bool force_clear { false };
    bool clear_on_first_write { false };
    // Later graph versions of this RT keep earlier color content. Use this
    // for composition targets, not transient effect outputs.
    bool preserve_on_write { false };

    i32 PhysicalWidth() const { return physical_width > 0 ? physical_width : width; }
    i32 PhysicalHeight() const { return physical_height > 0 ? physical_height : height; }
};

// ============================================================================
// SceneIndexArray.h
// ============================================================================

class SceneIndexArray : NoCopy {
    constexpr static usize Unit_Byte_Size { sizeof(u32) };

public:
    SceneIndexArray(usize indexCount);
    SceneIndexArray(std::span<const u32> data);

    SceneIndexArray(SceneIndexArray&&) noexcept;
    ~SceneIndexArray() = default;

    void Assign(usize index, std::span<const u32> data) {
        if (! IncreaseCheckSet((index + data.size()) * Unit_Byte_Size)) return;
        std::copy(data.begin(), data.end(), m_data.begin() + index);
        BumpDataGeneration();
    }

    const u32* Data() const { return m_data.is_empty() ? nullptr : m_data.begin(); }
    usize      DataCount() const { return m_size; }
    usize      DataSizeOf() const { return m_size * Unit_Byte_Size; }

    usize RenderDataCount() const noexcept {
        return m_render_size > m_size ? m_size : m_render_size;
    }
    void SetRenderDataCount(usize val) noexcept { m_render_size = val; }

    usize CapacityCount() const { return m_data.len(); }
    usize CapacitySizeof() const { return m_data.len() * Unit_Byte_Size; }
    u64   DataGeneration() const { return m_generation; }

    u32  ID() const { return m_id; }
    void SetID(u32 id) { m_id = id; }

private:
    bool IncreaseCheckSet(usize size);
    void BumpDataGeneration() noexcept { ++m_generation; }

    Vec<u32> m_data;
    usize    m_size { 0 };

    usize m_render_size { std::numeric_limits<usize>::max() };

    u32 m_id { std::numeric_limits<u32>::max() };
    u64 m_generation { 1 };
};

// ============================================================================
// SceneVertexArray.h
// ============================================================================

class SceneVertexArray : NoCopy {
public:
    struct SceneVertexAttribute {
        std::string name;
        VertexType  type;
        bool        padding { true };
    };
    struct SceneVertexAttributeOffset {
        SceneVertexAttribute attr;
        usize                offset;
    };

    SceneVertexArray(const std::vector<SceneVertexAttribute>& attrs, usize count);
    ~SceneVertexArray() = default;

    SceneVertexArray(SceneVertexArray&&) noexcept;
    SceneVertexArray& operator=(SceneVertexArray&&) noexcept;

    bool AddVertex(const float*);
    bool SetVertex(std::string_view name, std::span<const float> data) noexcept;
    bool SetVertexs(usize index, std::span<const float> data) noexcept;

    // Drops the active size to zero without releasing capacity. Subsequent
    // SetVertexs calls regrow it. Used by per-frame dynamic geners (rope
    // particles) so the high-water mark from a previous frame doesn't keep
    // VertexCount() inflated when fewer segments are emitted this frame.
    void ResetSize() noexcept;

    bool GetOption(std::string_view) const;
    void SetOption(std::string_view, bool);

    const float* Data() const { return m_data.is_empty() ? nullptr : m_data.begin(); }
    usize        DataSize() const { return m_size; }
    usize        DataSizeOf() const { return m_size * sizeof(float); }
    usize        VertexCount() const { return m_size / m_oneSize; }
    usize        CapacitySize() const { return m_data.len(); }
    usize        CapacitySizeOf() const { return m_data.len() * sizeof(float); }
    usize        OneSize() const { return m_oneSize; }
    usize        OneSizeOf() const { return m_oneSize * sizeof(float); }
    u64          DataGeneration() const { return m_generation; }

    const auto&                                  Attributes() const { return m_attributes; }
    Map<std::string, SceneVertexAttributeOffset> GetAttrOffsetMap() const;

    u32  ID() const { return m_id; }
    void SetID(u32 id) { m_id = id; }

    static u8 TypeCount(VertexType);
    static u8 RealAttributeSize(const SceneVertexAttribute&);

private:
    bool TrySetSize(usize) noexcept;
    void BumpDataGeneration() noexcept { ++m_generation; }

    std::vector<SceneVertexAttribute> m_attributes;

    Map<std::string, bool> m_options;

    Vec<float> m_data;
    usize      m_oneSize { 0 };
    usize      m_size { 0 };

    u32 m_id;
    u64 m_generation { 1 };
};

// Build a SceneVertexAttribute vector from compile-time VertexAttrSpec literals.
// Lets callsites write `MakeAttrSet({VAttr::Position, VAttr::TexCoord})` instead
// of hand-typing string/type pairs.
inline std::vector<SceneVertexArray::SceneVertexAttribute>
MakeAttrSet(std::span<const VertexAttrSpec> specs) {
    std::vector<SceneVertexArray::SceneVertexAttribute> out;
    out.reserve(specs.size());
    for (auto& s : specs) out.push_back({ std::string(s.name), s.type });
    return out;
}

inline std::vector<SceneVertexArray::SceneVertexAttribute>
MakeAttrSet(std::initializer_list<VertexAttrSpec> specs) {
    return MakeAttrSet(std::span<const VertexAttrSpec>(specs.begin(), specs.size()));
}

// ============================================================================
// SceneMaterial.h
// ============================================================================

struct SceneAnimationCurve;

struct SceneShaderValueAnimation {
    ShaderValue                          base;
    std::shared_ptr<SceneAnimationCurve> curve;
};

struct SceneMaterialCustomShader {
    std::shared_ptr<SceneShader>                shader;
    ShaderValues                                constValues;
    Map<std::string, SceneShaderValueAnimation> valueAnimations;
    std::optional<SceneShaderVariantDesc>       variant;
    // Set when constValues was mutated outside of prepare()/parse — e.g. a
    // RenderSetUserProperty handler writing a new user-property value. The
    // pass's per-frame update_op picks this up, re-writes the affected cbuffer
    // members, and clears the flag.
    bool dirty { false };
};

enum class SceneMaterialTextureDependency
{
    Empty,
    Imported,
    RenderTarget,
    LinkRenderTarget,
    MipMappedFramebuffer,
};

inline SceneMaterialTextureDependency ClassifySceneMaterialTexture(std::string_view texture) {
    if (texture.empty()) return SceneMaterialTextureDependency::Empty;
    if (IsSpecLinkTex(texture)) return SceneMaterialTextureDependency::LinkRenderTarget;
    if (sstart_with(texture, WE_MIP_MAPPED_FRAME_BUFFER))
        return SceneMaterialTextureDependency::MipMappedFramebuffer;
    if (IsSpecTex(texture)) return SceneMaterialTextureDependency::RenderTarget;
    return SceneMaterialTextureDependency::Imported;
}

inline bool IsLocalSceneMaterialTextureDependency(SceneMaterialTextureDependency dep) {
    return dep == SceneMaterialTextureDependency::Empty ||
           dep == SceneMaterialTextureDependency::Imported;
}

inline bool CanRefreshSceneMaterialTextureBinding(std::string_view old_texture,
                                                  std::string_view new_texture,
                                                  std::string_view pass_output = {}) {
    if (old_texture == new_texture) return true;
    if ((! old_texture.empty() && old_texture == pass_output) ||
        (! new_texture.empty() && new_texture == pass_output))
        return false;
    auto old_dep = ClassifySceneMaterialTexture(old_texture);
    auto new_dep = ClassifySceneMaterialTexture(new_texture);
    return IsLocalSceneMaterialTextureDependency(old_dep) &&
           IsLocalSceneMaterialTextureDependency(new_dep);
}

using SceneMaterialDirtyFlags = u32;

enum class SceneMaterialDirty : SceneMaterialDirtyFlags
{
    Resources = 1u << 0u,
    Pipeline  = 1u << 1u,
    Graph     = 1u << 2u,
};

inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyNone { 0u };
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyResources {
    static_cast<SceneMaterialDirtyFlags>(SceneMaterialDirty::Resources),
};
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyPipeline {
    static_cast<SceneMaterialDirtyFlags>(SceneMaterialDirty::Pipeline),
};
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyGraph {
    static_cast<SceneMaterialDirtyFlags>(SceneMaterialDirty::Graph),
};
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyAll {
    SceneMaterialDirtyResources | SceneMaterialDirtyPipeline | SceneMaterialDirtyGraph,
};

inline bool SceneShaderVariantHasActiveTextureMetadata(const SceneShaderVariantDesc& desc) {
    for (const auto& stage : desc.stages) {
        if (! stage.active_texture_slots.empty()) return true;
    }
    return false;
}

inline Set<unsigned> SceneShaderVariantActiveTextureSlots(const SceneShaderVariantDesc& desc) {
    Set<unsigned> slots;
    for (const auto& stage : desc.stages) {
        slots.insert(stage.active_texture_slots.begin(), stage.active_texture_slots.end());
    }
    return slots;
}

inline Map<std::string, std::string>
SceneShaderVariantUniformLayout(const SceneShaderVariantDesc& desc) {
    Map<std::string, std::string> uniforms;
    for (const auto& stage : desc.stages) {
        for (const auto& [name, ty] : stage.uniforms) uniforms[name] = ty;
    }
    return uniforms;
}

inline bool SameSceneShaderVariantStageSet(const SceneShaderVariantDesc& lhs,
                                           const SceneShaderVariantDesc& rhs) {
    if (lhs.stages.size() != rhs.stages.size()) return false;
    for (usize i = 0; i < lhs.stages.size(); ++i) {
        if (lhs.stages[i].stage != rhs.stages[i].stage) return false;
    }
    return true;
}

inline bool SameSceneShaderVariantUniformShape(const SceneShaderVariantDesc& lhs,
                                               const SceneShaderVariantDesc& rhs) {
    auto lhs_layout = SceneShaderVariantUniformLayout(lhs);
    auto rhs_layout = SceneShaderVariantUniformLayout(rhs);
    if (! lhs_layout.empty() || ! rhs_layout.empty()) return lhs_layout == rhs_layout;

    if (lhs.default_uniforms.size() != rhs.default_uniforms.size()) return false;
    for (const auto& [name, value] : lhs.default_uniforms) {
        auto it = rhs.default_uniforms.find(name);
        if (it == rhs.default_uniforms.end() || it->second.size() != value.size()) return false;
    }
    return true;
}

inline bool SceneShaderVariantHasCodeHashes(const SceneShaderVariantDesc& desc) {
    for (const auto& stage : desc.stages) {
        if (stage.code_hash != 0) return true;
    }
    return false;
}

inline bool SameSceneShaderVariantCodeHashes(const SceneShaderVariantDesc& lhs,
                                             const SceneShaderVariantDesc& rhs) {
    const bool lhs_has_hashes = SceneShaderVariantHasCodeHashes(lhs);
    const bool rhs_has_hashes = SceneShaderVariantHasCodeHashes(rhs);
    if (! lhs_has_hashes && ! rhs_has_hashes) return true;
    if (lhs.stages.size() != rhs.stages.size()) return false;
    for (usize i = 0; i < lhs.stages.size(); ++i) {
        if (lhs.stages[i].code_hash != rhs.stages[i].code_hash) return false;
    }
    return true;
}

inline bool SameSceneShaderVariantDescriptorLayout(const SceneShaderVariantDesc& lhs,
                                                   const SceneShaderVariantDesc& rhs) {
    if (lhs.descriptor_layout_hash == 0 && rhs.descriptor_layout_hash == 0) return true;
    return lhs.descriptor_layout_hash == rhs.descriptor_layout_hash;
}

inline SceneMaterialDirtyFlags
ClassifySceneShaderVariantMutation(const SceneShaderVariantDesc& current,
                                   const SceneShaderVariantDesc& next) {
    if (! current.Valid() || ! next.Valid()) return SceneMaterialDirtyGraph;
    if (current.shader_name != next.shader_name) return SceneMaterialDirtyGraph;
    if (SceneShaderVariantHasActiveTextureMetadata(current) ||
        SceneShaderVariantHasActiveTextureMetadata(next)) {
        if (SceneShaderVariantActiveTextureSlots(current) !=
            SceneShaderVariantActiveTextureSlots(next))
            return SceneMaterialDirtyGraph;
    }

    SceneMaterialDirtyFlags flags = SceneMaterialDirtyNone;
    if (! SameSceneShaderVariantStageSet(current, next)) flags |= SceneMaterialDirtyPipeline;
    if (! SameSceneShaderVariantUniformShape(current, next)) {
        flags |= SceneMaterialDirtyResources | SceneMaterialDirtyPipeline;
    }
    if (current.resolved_combos != next.resolved_combos ||
        current.input_combos != next.input_combos ||
        current.texture_infos.size() != next.texture_infos.size() ||
        current.geometry_shader_enabled != next.geometry_shader_enabled) {
        flags |= SceneMaterialDirtyResources | SceneMaterialDirtyPipeline;
    }
    if (! SameSceneShaderVariantCodeHashes(current, next)) flags |= SceneMaterialDirtyPipeline;
    if (! SameSceneShaderVariantDescriptorLayout(current, next)) {
        flags |= SceneMaterialDirtyResources | SceneMaterialDirtyPipeline;
    }
    if (flags == SceneMaterialDirtyNone && current.stages.size() == next.stages.size()) {
        for (usize i = 0; i < current.stages.size(); ++i) {
            if (current.stages[i].source_key != next.stages[i].source_key ||
                current.stages[i].source != next.stages[i].source) {
                flags |= SceneMaterialDirtyPipeline;
                break;
            }
        }
    }
    return flags;
}

struct SceneMaterial {
public:
    SceneMaterial() = default;
    SceneMaterial(const SceneMaterial& other) { copyFrom(other); }
    SceneMaterial(SceneMaterial&& other) noexcept { moveFrom(std::move(other)); }
    SceneMaterial& operator=(const SceneMaterial& other) {
        if (this != &other) copyFrom(other);
        return *this;
    }
    SceneMaterial& operator=(SceneMaterial&& other) noexcept {
        if (this != &other) moveFrom(std::move(other));
        return *this;
    }

    void SetDirty(SceneMaterialDirtyFlags flags) { m_dirty_flags.fetch_or(flags); }
    void SetResourceDirty() { SetDirty(SceneMaterialDirtyResources); }
    void SetPipelineDirty() { SetDirty(SceneMaterialDirtyPipeline); }
    void SetGraphDirty() { SetDirty(SceneMaterialDirtyGraph); }
    SceneMaterialDirtyFlags DirtyFlags() const { return m_dirty_flags.load(); }
    SceneMaterialDirtyFlags
    ConsumeDirtyFlags(SceneMaterialDirtyFlags mask = SceneMaterialDirtyAll) {
        SceneMaterialDirtyFlags old = m_dirty_flags.load();
        while (! m_dirty_flags.compare_exchange_weak(old, old & ~mask)) {
        }
        return old & mask;
    }

    bool SetBlendMode(BlendMode value) {
        if (blenmode == value) return false;
        blenmode = value;
        SetPipelineDirty();
        return true;
    }
    bool SetDepthTest(bool value) {
        if (depth_test == value) return false;
        depth_test = value;
        SetPipelineDirty();
        return true;
    }
    bool SetDepthWrite(bool value) {
        if (depth_write == value) return false;
        depth_write = value;
        SetPipelineDirty();
        return true;
    }
    bool SetCullMode(CullMode value) {
        if (cull_mode == value) return false;
        cull_mode = value;
        SetPipelineDirty();
        return true;
    }
    bool SetShaderValue(std::string uniform_name, const ShaderValue& value) {
        if (uniform_name.empty()) return false;
        auto shaped                            = ShapeShaderValue(uniform_name, value);
        customShader.constValues[uniform_name] = shaped;
        if (auto it = customShader.valueAnimations.find(uniform_name);
            it != customShader.valueAnimations.end()) {
            it->second.base = shaped;
        }
        customShader.dirty = true;
        return true;
    }
    bool SetShaderValueAnimation(std::string                          uniform_name,
                                 std::shared_ptr<SceneAnimationCurve> curve);
    bool TickShaderValueAnimations(double runtime);
    bool SetShaderVariant(std::shared_ptr<SceneShader> shader, SceneShaderVariantDesc variant) {
        if (! shader || ! variant.Valid()) return false;
        SceneMaterialDirtyFlags flags =
            customShader.variant.has_value()
                ? ClassifySceneShaderVariantMutation(*customShader.variant, variant)
                : SceneMaterialDirtyGraph;
        if (flags == SceneMaterialDirtyNone) return false;
        if (! variant.texture_slots.empty() &&
            SceneShaderVariantHasActiveTextureMetadata(variant)) {
            textures    = variant.texture_slots;
            auto active = SceneShaderVariantActiveTextureSlots(variant);
            for (usize i = 0; i < textures.size(); ++i) {
                if (! active.contains(static_cast<unsigned>(i))) textures[i].clear();
            }
        }
        customShader.shader  = std::move(shader);
        customShader.variant = std::move(variant);
        SetDirty(flags);
        return true;
    }

    std::string              name;
    std::vector<std::string> textures;
    std::vector<std::string> defines;

    bool hasSprite { false };

    SceneMaterialCustomShader customShader;
    BlendMode                 blenmode { BlendMode::Disable };
    bool                      depth_test { false };
    bool                      depth_write { false };
    CullMode                  cull_mode { CullMode::None };

private:
    ShaderValue ShapeShaderValue(std::string_view uniform_name, const ShaderValue& value) const {
        if (value.size() != 1) return value;

        usize target_size = 0;
        if (auto it = customShader.constValues.find(std::string(uniform_name));
            it != customShader.constValues.end()) {
            target_size = it->second.size();
        }
        if (target_size <= 1 && customShader.shader) {
            if (auto it = customShader.shader->default_uniforms.find(std::string(uniform_name));
                it != customShader.shader->default_uniforms.end()) {
                target_size = it->second.size();
            }
        }
        if (target_size <= 1 || target_size > 4) return value;

        std::vector<float> shaped(target_size, value[0]);
        return ShaderValue(std::span<const float>(shaped));
    }

    void copyFrom(const SceneMaterial& other) {
        name         = other.name;
        textures     = other.textures;
        defines      = other.defines;
        hasSprite    = other.hasSprite;
        customShader = other.customShader;
        blenmode     = other.blenmode;
        depth_test   = other.depth_test;
        depth_write  = other.depth_write;
        cull_mode    = other.cull_mode;
        m_dirty_flags.store(other.m_dirty_flags.load());
    }
    void moveFrom(SceneMaterial&& other) {
        name         = std::move(other.name);
        textures     = std::move(other.textures);
        defines      = std::move(other.defines);
        hasSprite    = other.hasSprite;
        customShader = std::move(other.customShader);
        blenmode     = other.blenmode;
        depth_test   = other.depth_test;
        depth_write  = other.depth_write;
        cull_mode    = other.cull_mode;
        m_dirty_flags.store(other.m_dirty_flags.load());
    }

    std::atomic<SceneMaterialDirtyFlags> m_dirty_flags { SceneMaterialDirtyNone };
};

// ============================================================================
// SceneMesh.h
// ============================================================================

using SceneMeshDirtyFlags = u32;

enum class SceneMeshDirty : SceneMeshDirtyFlags
{
    Data   = 1u << 0u,
    Layout = 1u << 1u,
};

inline constexpr SceneMeshDirtyFlags SceneMeshDirtyNone { 0u };
inline constexpr SceneMeshDirtyFlags SceneMeshDirtyData {
    static_cast<SceneMeshDirtyFlags>(SceneMeshDirty::Data),
};
inline constexpr SceneMeshDirtyFlags SceneMeshDirtyLayout {
    static_cast<SceneMeshDirtyFlags>(SceneMeshDirty::Layout),
};
inline constexpr SceneMeshDirtyFlags SceneMeshDirtyAll {
    SceneMeshDirtyData | SceneMeshDirtyLayout,
};

class SceneMesh {
public:
    // Per-part draw ranges into one submesh's index array. When empty, the
    // submesh is drawn as one DrawIndexed call covering all its indices; when
    // populated (V21 puppets with parts[] block), one DrawIndexed call is
    // issued per range in vector order — matching the file's z-order. All
    // ranges in a submesh share the submesh's material slot.
    struct DrawRange {
        u32 first_index;
        u32 index_count;
    };

    // = glTF "primitive": one vertex-stream set + one index array + one
    // material slot. A SceneMesh holds >= 1 Submesh; today most paths emit
    // exactly one (single-slot compat); WPSceneParser will emit N for
    // .mdl meshes with mesh_count > 1.
    struct Submesh {
        std::vector<SceneVertexArray> vertex_arrays;
        std::vector<SceneIndexArray>  index_arrays;
        std::vector<DrawRange>        draw_ranges;
        u32                           material_slot { 0 };
        // Non-empty value redirects this submesh's pass output to the
        // named RT (instead of the SceneNode's default). Used by puppet
        // clipping-mask submeshes to write into a shared `_rt_puppet_mask`
        // that the main puppet pass samples via g_Texture8.
        std::string output_override;
    };

    SceneMesh(bool dynamic = false)
        : m_dynamic(dynamic), m_dirty(false), m_data(std::make_shared<Data>()) {}

    MeshPrimitive Primitive() const { return m_primitive; }
    u32           PointSize() const { return m_pointSize; }

    bool        Dynamic() const { return m_dynamic; }
    const auto& Dirty() const { return m_dirty; }
    auto&       Dirty() { return m_dirty; }
    void        SetDirty(SceneMeshDirtyFlags flags = SceneMeshDirtyData) {
        m_dirty_flags.fetch_or(flags);
        m_dirty.store(true);
    }
    void                SetLayoutDirty() { SetDirty(SceneMeshDirtyLayout); }
    SceneMeshDirtyFlags DirtyFlags() const { return m_dirty_flags.load(); }
    SceneMeshDirtyFlags ConsumeDirtyFlags(SceneMeshDirtyFlags mask = SceneMeshDirtyAll) {
        SceneMeshDirtyFlags old = m_dirty_flags.load();
        while (! m_dirty_flags.compare_exchange_weak(old, old & ~mask)) {
        }
        auto consumed = old & mask;
        if ((old & ~mask) == SceneMeshDirtyNone) m_dirty.store(false);
        return consumed;
    }

    u32  ID() const { return m_id; };
    void SetID(u32 v) { m_id = v; };

    void SetPrimitive(MeshPrimitive v) { m_primitive = v; }
    void SetPointSize(u32 v) { m_pointSize = v; }

    // ---- New submesh API ----
    const std::vector<Submesh>& Submeshes() const { return m_data->submeshes; }
    std::vector<Submesh>&       Submeshes() { return m_data->submeshes; }

    // Materials are per-mesh-instance, NOT shared via ChangeMeshDataFrom — same
    // contract as the legacy m_material field.
    const std::vector<std::shared_ptr<SceneMaterial>>& MaterialSlots() const { return m_materials; }
    std::vector<std::shared_ptr<SceneMaterial>>&       MaterialSlots() { return m_materials; }

    // ---- Legacy single-slot compat (routes through submeshes[0] / materials[0]) ----
    usize VertexCount() const { return submesh0().vertex_arrays.size(); }
    usize IndexCount() const { return submesh0().index_arrays.size(); }

    const SceneVertexArray& GetVertexArray(usize index) const {
        return submesh0().vertex_arrays[index];
    }
    const SceneIndexArray& GetIndexArray(usize index) const {
        return submesh0().index_arrays[index];
    }
    SceneVertexArray& GetVertexArray(usize index) { return ensureSubmesh0().vertex_arrays[index]; }
    SceneIndexArray&  GetIndexArray(usize index) { return ensureSubmesh0().index_arrays[index]; }

    void AddIndexArray(SceneIndexArray&& array) {
        ensureSubmesh0().index_arrays.emplace_back(std::move(array));
    }
    void AddVertexArray(SceneVertexArray&& array) {
        ensureSubmesh0().vertex_arrays.emplace_back(std::move(array));
    }
    void AddMaterial(SceneMaterial&& material) {
        m_materials.push_back(std::make_shared<SceneMaterial>(std::move(material)));
    }

    SceneMaterial* Material() { return m_materials.empty() ? nullptr : m_materials[0].get(); }

    void ChangeMeshDataFrom(const SceneMesh& o) { m_data = o.m_data; }

    const std::vector<DrawRange>& DrawRanges() const {
        static const std::vector<DrawRange> kEmpty;
        return m_data->submeshes.empty() ? kEmpty : m_data->submeshes[0].draw_ranges;
    }
    void SetDrawRanges(std::vector<DrawRange> ranges) {
        ensureSubmesh0().draw_ranges = std::move(ranges);
    }

private:
    struct Data {
        std::vector<Submesh> submeshes;
    };

    Submesh& ensureSubmesh0() {
        if (m_data->submeshes.empty()) m_data->submeshes.emplace_back();
        return m_data->submeshes[0];
    }
    const Submesh& submesh0() const {
        static const Submesh kEmpty;
        return m_data->submeshes.empty() ? kEmpty : m_data->submeshes[0];
    }

    u32               m_id { std::numeric_limits<u32>::max() };
    MeshPrimitive     m_primitive { MeshPrimitive::TRIANGLE };
    u32               m_pointSize { 1 };
    bool              m_dynamic;
    std::atomic<bool> m_dirty;

    std::shared_ptr<Data>                       m_data;      // shared via ChangeMeshDataFrom
    std::vector<std::shared_ptr<SceneMaterial>> m_materials; // per-instance
    std::atomic<SceneMeshDirtyFlags>            m_dirty_flags { SceneMeshDirtyNone };
};

class SceneNode;
struct SceneImageEffect;
class SceneImageEffectLayer;
struct ScenePostProcess;

// ============================================================================
// SceneCamera.h
// ============================================================================

class SceneCamera {
public:
    explicit SceneCamera(i32 width, i32 height, float near, float far)
        : m_width(width),
          m_height(height),
          m_aspect(m_width / m_height),
          m_nearClip(near),
          m_farClip(far),
          m_perspective(false) {}

    explicit SceneCamera(float aspect, float near, float far, float fov)
        : m_aspect(aspect), m_nearClip(near), m_farClip(far), m_fov(fov), m_perspective(true) {}

    SceneCamera(const SceneCamera& cam) { Clone(cam); }

    void Update();

    void AttatchNode(SceneNode*);

    bool   IsPerspective() const { return m_perspective; }
    bool   AllowCameraShake() const { return m_allowCameraShake; }
    void   SetAllowCameraShake(bool value) { m_allowCameraShake = value; }
    double Aspect() const { return m_aspect; }
    double Width() const { return m_width; }
    double Height() const { return m_height; }
    double NearClip() const { return m_nearClip; }
    double FarClip() const { return m_farClip; }
    double Fov() const { return m_fov; }

    void SetWidth(double value) {
        m_width  = value;
        m_aspect = m_width / m_height;
    }
    void SetHeight(double value) {
        m_height = value;
        m_aspect = m_width / m_height;
    }
    void SetAspect(double aspect) { m_aspect = aspect; }
    void SetFov(double value) { m_fov = value; }

    // Explicit eye/center/up view, used by perspective scenes (general
    // isOrtho==false) whose camera is given in WE world units rather than the
    // 2D pixel-space node placement. Once set, the view comes from LookAt and
    // the attached node (if any) is ignored.
    void SetLookAt(const Eigen::Vector3d& eye, const Eigen::Vector3d& center,
                   const Eigen::Vector3d& up) {
        m_eye    = eye;
        m_center = center;
        m_up     = up;
        m_lookat = true;
    }
    bool IsLookAt() const { return m_lookat; }

    void  AttatchImgEffect(std::shared_ptr<SceneImageEffectLayer> eff) { m_imgEffect = eff; }
    bool  HasImgEffect() const { return (bool)m_imgEffect; }
    auto& GetImgEffect() { return m_imgEffect; }

    Eigen::Vector3d GetPosition() const;
    Eigen::Vector3d GetDirection() const;

    // Lazy: recomputes from m_node->ModelTrans() on every call. Cheap when
    // the attached node hasn't moved (UpdateTrans early-exits on clean
    // m_dirty), correctness-keeping when scripts / parent-chain attachment
    // shift the node between frames.
    Eigen::Matrix4d GetViewMatrix();
    Eigen::Matrix4d GetViewProjectionMatrix();

    rstd::Option<SceneNode*> GetAttachedNode() const {
        if (m_node == nullptr) return rstd::None();
        return rstd::Some<SceneNode*>(m_node);
    }

    void Clone(const SceneCamera& cam) {
        m_width            = cam.m_width;
        m_height           = cam.m_height;
        m_aspect           = cam.m_aspect;
        m_nearClip         = cam.m_nearClip;
        m_farClip          = cam.m_farClip;
        m_fov              = cam.m_fov;
        m_perspective      = cam.m_perspective;
        m_allowCameraShake = cam.m_allowCameraShake;
        m_lookat           = cam.m_lookat;
        m_eye              = cam.m_eye;
        m_center           = cam.m_center;
        m_up               = cam.m_up;
        m_node             = cam.m_node;
    }

private:
    void CalculateViewProjectionMatrix();

    double m_width { 1.0f };
    double m_height { 1.0f };
    double m_aspect { 16.0f / 9.0f };
    double m_nearClip { 0.01f };
    double m_farClip { 1000.0f };
    double m_fov { 45.0f };
    bool   m_perspective { false };
    bool   m_allowCameraShake { true };

    bool            m_lookat { false };
    Eigen::Vector3d m_eye { Eigen::Vector3d::Zero() };
    Eigen::Vector3d m_center { -Eigen::Vector3d::UnitZ() };
    Eigen::Vector3d m_up { Eigen::Vector3d::UnitY() };

    Eigen::Matrix4d m_viewMat { Eigen::Matrix4d::Identity() };
    Eigen::Matrix4d m_viewProjectionMat { Eigen::Matrix4d::Identity() };

    SceneNode*                             m_node { nullptr };
    std::shared_ptr<SceneImageEffectLayer> m_imgEffect { nullptr };
};

struct SceneAnimationKey {
    i32   frame { 0 };
    float value { 0.0f };
    bool  front_enabled { false };
    float front_x { 0.0f };
    float front_y { 0.0f };
    bool  back_enabled { false };
    float back_x { 0.0f };
    float back_y { 0.0f };
};

struct SceneAnimationCurve {
    std::vector<SceneAnimationKey> c0;
    std::vector<SceneAnimationKey> c1;
    std::vector<SceneAnimationKey> c2;
    float                          fps { 30.0f };
    i32                            length { 0 };
    std::string                    mode;
    bool                           wraploop { false };
    bool                           relative { false };

    bool            Empty() const;
    float           EvaluateScalar(float base, double runtime) const;
    Eigen::Vector3f EvaluateVec3(const Eigen::Vector3f& base, double runtime) const;
};

struct SceneCameraLookAtKey {
    float           frame { 0.0f };
    Eigen::Vector3f eye { Eigen::Vector3f::Zero() };
    Eigen::Vector3f center { Eigen::Vector3f::Zero() };
    Eigen::Vector3f up { Eigen::Vector3f::UnitY() };
};

struct SceneCameraLookAtTrack {
    float                             duration { 0.0f };
    std::vector<SceneCameraLookAtKey> keys;
};

class SceneCameraPath {
public:
    std::string                         camera_name;
    std::shared_ptr<SceneCamera>        camera;
    SceneNode*                          node { nullptr };
    Eigen::Vector3f                     default_translate { Eigen::Vector3f::Zero() };
    Eigen::Vector3f                     default_rotation { Eigen::Vector3f::Zero() };
    Eigen::Vector3f                     path_translate_bias { Eigen::Vector3f::Zero() };
    Eigen::Vector3f                     path_rotation_bias { Eigen::Vector3f::Zero() };
    double                              default_width { 1.0 };
    double                              default_height { 1.0 };
    double                              default_fov { 50.0 };
    Eigen::Vector3f                     origin_base { Eigen::Vector3f::Zero() };
    Eigen::Vector3f                     rotation_base { Eigen::Vector3f::Zero() };
    float                               zoom_base { 1.0f };
    float                               fov_base { 50.0f };
    bool                                perspective { false };
    bool                                enabled { true };
    bool                                default_lookat { false };
    Eigen::Vector3f                     default_eye { Eigen::Vector3f::Zero() };
    Eigen::Vector3f                     default_center { -Eigen::Vector3f::UnitZ() };
    Eigen::Vector3f                     default_up { Eigen::Vector3f::UnitY() };
    float                               lookat_fps { 1.0f };
    std::vector<SceneCameraLookAtTrack> lookat_tracks;
    SceneAnimationCurve                 origin_curve;
    SceneAnimationCurve                 rotation_curve;
    SceneAnimationCurve                 zoom_curve;
    SceneAnimationCurve                 fov_curve;
    SceneUserVisibilityBinding          visible_user_binding;

    void CaptureViewport();
    void SetEnabled(bool value) { enabled = value; }
    bool Tick(double runtime);
    bool ApplyDefault();
};

class SceneSoundControl {
public:
    virtual ~SceneSoundControl() = default;

    virtual void Play()                  = 0;
    virtual void Stop()                  = 0;
    virtual void Pause()                 = 0;
    virtual bool IsPlaying() const       = 0;
    virtual void SetVolume(float volume) = 0;
};

// ============================================================================
// SceneNode.h
// ============================================================================

// Lifetime invariant — tree topology is frozen post-parse.
//
// `m_children` / `m_parent` are written only during parse-time construction
// (WPSceneParser AppendChild / SpawnLayerClones). Once `Scene` is shipped to
// the render thread via `RenderSetScene`, no code adds, removes, or reorders
// nodes. The only exception is `SetParentAnchor`, used by
// SceneImageEffectLayer::ResolveEffect to re-anchor an effect's composite
// node onto its layer's worldNode for transform inheritance — both nodes
// are parse-time creations and survive for the Scene's lifetime, so the
// re-anchor never dangles.
//
// post-parse mutations restricted to render thread: m_translate / m_scale /
// m_rotation, m_visible, m_user_alpha, m_brightness, m_color, m_tex_anim,
// m_dirty (all driven by script ticks and shader-value updates).
//
// Practical consequence: every non-owning `SceneNode*` reference held by
// downstream subsystems (FieldScript::Impl::node, EngineHostState::text_setters,
// SceneImageEffectLayer::m_worldNode, SceneCamera::m_node, m_parent itself)
// is valid for the Scene's lifetime by construction. The dtor's parent
// back-link clearing below is a defence against Scene teardown ordering,
// where a child held by an external Arc (e.g. actuator closures)
// can outlive its parent inside the std::list destructor.
class SceneNode : NoCopy, NoMove {
public:
    SceneNode()
        : m_name(),
          m_dirty(true),
          m_translate(Eigen::Vector3f::Zero()),
          m_scale { 1.0f, 1.0f, 1.0f },
          m_rotation(Eigen::Vector3f::Zero()) {}
    SceneNode(const Eigen::Vector3f& translate, const Eigen::Vector3f& scale,
              const Eigen::Vector3f& rotation, const std::string& name = "")
        : m_name(name),
          m_dirty(true),
          m_translate(translate),
          m_scale(scale),
          m_rotation(rotation) {};

    // Scene-teardown safety: an external holder (SceneCamera::m_node,
    // ParticleSubSystem::m_owner_node.lock(), actuator closures) can keep a
    // child alive past its parent's destruction while the std::list is being
    // torn down. Clear the back-link so the survivor's UpdateTrans /
    // HitTestNode falls back to local trans instead of dereferencing freed
    // memory.
    ~SceneNode() {
        if (m_parent) {
            auto& anchors = m_parent->m_transform_anchors;
            anchors.erase(std::remove(anchors.begin(), anchors.end(), this), anchors.end());
        }
        for (auto& c : m_children) {
            if (c) c->m_parent = nullptr;
        }
        for (auto* anchor : m_transform_anchors) {
            if (anchor && anchor->m_parent == this) anchor->m_parent = nullptr;
        }
    }

    const auto& Camera() const { return m_cameraName; }
    void        SetCamera(const std::string& name) { m_cameraName = name; }
    bool        Perspective() const { return m_perspective; }
    void        SetPerspective(bool value) { m_perspective = value; }
    void        AddMesh(std::shared_ptr<SceneMesh> mesh) { m_mesh = mesh; }
    void        AppendChild(rstd::sync::Arc<SceneNode> sub) {
        sub->m_parent = this;
        // Stale ModelTrans on the child (cached without this new
        // parent context) would persist for the rest of the frame
        // otherwise — force a recompute on next UpdateTrans.
        sub->MarkTransDirty();
        m_children.push_back(rstd::move(sub));
    }
    Eigen::Matrix4d GetLocalTrans() const;

    const auto& Translate() const { return m_translate; }
    const auto& Rotation() const { return m_rotation; }
    const auto& Scale() const { return m_scale; }
    void        SetRotation(Eigen::Vector3f v) {
        m_rotation = v;
        MarkTransDirty();
    }
    void SetTranslate(Eigen::Vector3f v) {
        m_translate = v;
        MarkTransDirty();
    }
    void SetScale(Eigen::Vector3f v) {
        m_scale = v;
        MarkTransDirty();
    }

    // Local content size (image / text bbox). Zero means "unknown"; scripts
    // reading `thisLayer.size` then fall back to the legacy 100×100 stub.
    const auto& Size() const { return m_size; }
    void        SetSize(Eigen::Vector2f v) { m_size = v; }

    // Script-driven per-frame overrides. The renderer maps these onto the
    // shader's available runtime tint uniforms without touching baked values
    // until script writes occur.
    //
    // Visibility writes are runtime alpha updates too: hidden nodes force 0,
    // and visible nodes restore the baked alpha after a prior hide.
    bool IsAlphaOverridden() const {
        return m_alpha_overridden || m_visible_overridden ||
               (m_alpha_source != nullptr && m_alpha_source->IsAlphaOverridden());
    }
    float EffectiveAlpha() const {
        float alpha = m_visible ? (m_alpha_overridden ? m_user_alpha : m_base_alpha) : 0.0f;
        if (m_alpha_source != nullptr && m_alpha_source->IsAlphaOverridden())
            alpha *= m_alpha_source->EffectiveAlpha();
        return alpha;
    }
    bool  Visible() const { return m_visible; }
    float UserAlpha() const { return m_user_alpha; }
    void  SetVisible(bool v) {
        m_visible            = v;
        m_visible_overridden = true;
    }
    void SetUserAlpha(float v) {
        m_user_alpha       = v;
        m_alpha_overridden = true;
    }
    void SetOriginAnimation(SceneAnimationCurve curve) {
        m_origin_base  = m_translate;
        m_origin_curve = Some(rstd::move(curve));
    }
    void SetScaleAnimation(SceneAnimationCurve curve) {
        m_scale_base  = m_scale;
        m_scale_curve = Some(rstd::move(curve));
    }
    void SetRotationAnimation(SceneAnimationCurve curve) {
        m_rotation_base  = m_rotation;
        m_rotation_curve = Some(rstd::move(curve));
    }
    void SetAlphaAnimation(SceneAnimationCurve curve) { m_alpha_curve = Some(rstd::move(curve)); }
    void TickFieldAnimations(double runtime);
    void SetAlphaSource(SceneNode* node) { m_alpha_source = node; }

    const std::string& VisibleUserKey() const { return m_visible_user_binding.key; }
    void               SetVisibleUserKey(std::string k) {
        m_visible_user_binding = SceneUserVisibilityBinding { .key = std::move(k) };
    }
    const SceneUserVisibilityBinding& VisibleUserBinding() const { return m_visible_user_binding; }
    void                              SetVisibleUserBinding(SceneUserVisibilityBinding binding) {
        m_visible_user_binding = std::move(binding);
    }

    bool  IsBrightnessOverridden() const { return m_brightness_overridden; }
    float Brightness() const { return m_brightness; }
    void  SetBrightness(float v) {
        m_brightness            = v;
        m_brightness_overridden = true;
    }

    bool                   IsColorOverridden() const { return m_color_overridden; }
    const Eigen::Vector3f& Color() const { return m_color; }
    void                   SetColor(Eigen::Vector3f v) {
        m_color            = v;
        m_color_overridden = true;
    }
    const Eigen::Vector3f& BaseColor() const { return m_base_color; }
    float                  BaseAlpha() const { return m_base_alpha; }
    void                   SetBaseColor(Eigen::Vector3f color, float alpha) {
        m_base_color = color;
        m_base_alpha = alpha;
    }

    // Per-texture-slot script-driven sprite-animation override. Wallpaper
    // Engine's setFrame(n) / play() / stop() control map onto these:
    //   current_frame >= 0  → renderer pins to that frame, ignoring elapsed-
    //                         time advancement.
    //   playing == false    → renderer holds the current auto-advance frame.
    //   default { -1, true }→ regular auto-advance.
    struct TextureAnimatorState {
        int  current_frame { -1 };
        bool playing { true };
    };
    TextureAnimatorState&       TexAnim() { return m_tex_anim; }
    const TextureAnimatorState& TexAnim() const { return m_tex_anim; }

    void Play() {
        if (m_sound_control) {
            m_sound_control->Play();
            return;
        }
        m_layer_playing = true;
    }
    void Stop() {
        if (m_sound_control) {
            m_sound_control->Stop();
            return;
        }
        m_layer_playing = false;
    }
    void Pause() {
        if (m_sound_control) {
            m_sound_control->Pause();
            return;
        }
        m_layer_playing = false;
    }
    bool IsPlaying() const {
        if (m_sound_control) return m_sound_control->IsPlaying();
        return m_layer_playing;
    }
    void SetSoundControl(std::shared_ptr<SceneSoundControl> c) { m_sound_control = std::move(c); }
    SceneSoundControl* SoundControl() const { return m_sound_control.get(); }

    void CopyTrans(const SceneNode& node) {
        m_translate = node.m_translate;
        m_scale     = node.m_scale;
        m_rotation  = node.m_rotation;
        MarkTransDirty();
    }

    void            UpdateTrans();
    Eigen::Matrix4d ModelTrans() const { return m_trans; };

    SceneMesh*                        Mesh() { return m_mesh.get(); }
    const std::shared_ptr<SceneMesh>& MeshShared() const { return m_mesh; }
    bool HasMaterial() const { return m_mesh && m_mesh->Material() != nullptr; };

    const auto& GetChildren() const { return m_children; }
    auto&       GetChildren() { return m_children; }

    const std::string& Name() const { return m_name; }
    SceneNode*         Parent() const { return m_parent; }

    // Anchor for transform-only inheritance. The node does NOT join `p`'s
    // children, so TraverseNode never visits it through `p`. Used for the
    // SceneImageEffectLayer composite quad: the quad needs spImgNode's
    // world transform but must not be rendered twice in scene-tree traversal.
    void SetParentAnchor(SceneNode* p) {
        if (m_parent == p) {
            MarkTransDirty();
            return;
        }
        if (m_parent) {
            auto& anchors = m_parent->m_transform_anchors;
            anchors.erase(std::remove(anchors.begin(), anchors.end(), this), anchors.end());
        }
        m_parent = p;
        if (m_parent) {
            auto& anchors = m_parent->m_transform_anchors;
            if (std::find(anchors.begin(), anchors.end(), this) == anchors.end()) {
                anchors.push_back(this);
            }
        }
        MarkTransDirty();
    }

    // BFS over self + descendants; returns first node whose Name() matches.
    SceneNode* FindByName(std::string_view name);

    i32  ID() const { return m_id; }
    i32& ID() { return m_id; }

private:
    void MarkTransDirty();

    i32         m_id { -1 };
    std::string m_name;

    bool            m_dirty;
    Eigen::Matrix4d m_trans;

    Eigen::Vector3f m_translate { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f m_scale { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f m_rotation { 0.0f, 0.0f, 0.0f };
    Eigen::Vector2f m_size { 0.0f, 0.0f };

    bool                               m_visible { true };
    SceneUserVisibilityBinding         m_visible_user_binding {};
    bool                               m_visible_overridden { false };
    float                              m_user_alpha { 1.0f };
    bool                               m_alpha_overridden { false };
    SceneNode*                         m_alpha_source { nullptr };
    Eigen::Vector3f                    m_origin_base { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f                    m_scale_base { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f                    m_rotation_base { 0.0f, 0.0f, 0.0f };
    Option<SceneAnimationCurve>        m_origin_curve;
    Option<SceneAnimationCurve>        m_scale_curve;
    Option<SceneAnimationCurve>        m_rotation_curve;
    Option<SceneAnimationCurve>        m_alpha_curve;
    float                              m_brightness { 1.0f };
    bool                               m_brightness_overridden { false };
    Eigen::Vector3f                    m_color { 1.0f, 1.0f, 1.0f };
    bool                               m_color_overridden { false };
    Eigen::Vector3f                    m_base_color { 1.0f, 1.0f, 1.0f };
    float                              m_base_alpha { 1.0f };
    TextureAnimatorState               m_tex_anim {};
    bool                               m_layer_playing { true };
    std::shared_ptr<SceneSoundControl> m_sound_control;

    std::shared_ptr<SceneMesh> m_mesh;

    std::string m_cameraName;
    bool        m_perspective { false };

    // Raw back-link. Safe because tree topology is frozen post-parse (see
    // class header) and the dtor clears children's m_parent before any
    // out-of-order teardown can dereference a stale pointer.
    SceneNode* m_parent { nullptr };

    std::list<rstd::sync::Arc<SceneNode>> m_children;
    std::vector<SceneNode*>               m_transform_anchors;
};

// ============================================================================
// SceneImageEffectLayer.h
// ============================================================================

struct SceneImageEffectNode {
    std::string                                 output; // render target
    rstd::sync::Arc<SceneNode>                  sceneNode;
    bool                                        uses_unit_final_quad { false };
    Map<std::string, SceneShaderValueAnimation> final_quad_shader_values;
};

struct SceneImageEffect {
    enum class CmdType
    {
        Copy,
    };
    struct Command {
        CmdType     cmd { CmdType::Copy };
        std::string dst;
        std::string src;
        i32         afterpos { 0 };
    };
    std::string                     name;
    std::vector<Command>            commands;
    std::list<SceneImageEffectNode> nodes;
    SceneUserVisibilityBinding      visible_user_binding;
    bool                            runtime_visible { true };
};

class SceneImageEffectLayer {
public:
    SceneImageEffectLayer(SceneNode* node, float w, float h, std::string_view pingpong_a,
                          std::string_view pingpong_b);

    void AddEffect(const std::shared_ptr<SceneImageEffect>& node) {
        m_effects.push_back(node);
        m_resolved = false;
    }
    bool SetEffectRuntimeVisible(SceneImageEffect& effect, bool visible) {
        if (effect.runtime_visible == visible) return false;
        effect.runtime_visible = visible;
        m_resolved             = false;
        return true;
    }
    usize                             EffectCount() const { return m_effects.size(); }
    auto&                             GetEffect(usize index) { return m_effects.at(index); }
    std::shared_ptr<SceneImageEffect> FindEffect(std::string_view name) {
        auto it = std::find_if(m_effects.begin(), m_effects.end(), [name](const auto& effect) {
            return effect && effect->name == name;
        });
        return it == m_effects.end() ? nullptr : *it;
    }
    bool HasRuntimeVisibleEffect() const {
        return std::any_of(m_effects.begin(), m_effects.end(), [](const auto& effect) {
            return effect && effect->runtime_visible;
        });
    }
    bool RequiresIntermediateTarget() const {
        return m_final_resolve_effect || m_effects.empty() || HasRuntimeVisibleEffect();
    }
    bool HasRenderEffects() const { return m_final_resolve_effect || HasRuntimeVisibleEffect(); }
    const auto&      FirstTarget() const { return m_pingpong_a; }
    SceneMesh&       FinalMesh() { return *m_final_mesh.get(); }
    const SceneMesh& FinalMesh() const { return *m_final_mesh.as_ptr(); }
    void             AddPrefillNode(SceneImageEffectNode node) {
        m_prefill_nodes.push_back(std::move(node));
        m_resolved = false;
    }
    auto& PrefillNodes() { return m_prefill_nodes; }
    void  SetFullscreen(bool value) {
        fullscreen = value;
        m_resolved = false;
    }
    void SetFinalBlend(BlendMode m) {
        m_final_blend = m;
        m_resolved    = false;
    }
    void SetFinalMaterialState(const SceneMaterial& material) {
        m_final_blend       = material.blenmode;
        m_final_depth_test  = material.depth_test;
        m_final_depth_write = material.depth_write;
        m_final_cull_mode   = material.cull_mode;
        m_resolved          = false;
    }
    void SetFinalTarget(std::string t) {
        m_final_target = std::move(t);
        m_resolved     = false;
    }
    const auto& FinalTarget() const { return m_final_target; }
    void        SetFinalCamera(std::string camera) {
        if (m_final_camera == camera) return;
        m_final_camera = std::move(camera);
        m_resolved     = false;
    }
    void SetFinalResolveEffect(std::shared_ptr<SceneImageEffect> effect) {
        m_final_resolve_effect = std::move(effect);
        m_resolved             = false;
    }
    const auto& ResolvedEffects() const { return m_resolved_effects; }
    void        SetFinalLocal(bool value) {
        m_final_local = value;
        m_resolved    = false;
    }
    void SetSkipWhenNoRuntimeEffect(bool value) { m_skip_when_no_runtime_effect = value; }
    bool SkipWhenNoRuntimeEffect() const { return m_skip_when_no_runtime_effect; }

    // Idempotent: second and later calls are no-ops until any of the
    // mutating setters above (or AddEffect) flips m_resolved back to false.
    void ResolveEffect(const SceneMesh& defualt_mesh, std::string_view effect_cam);

private:
    struct EffectNodeResolveState {
        std::string        output;
        std::vector<usize> pingpong_input_slots;
    };

    struct EffectCommandResolveState {
        std::string src;
        std::string dst;
    };

    SceneNode*  m_worldNode;
    float       m_width { 1.0f };
    float       m_height { 1.0f };
    std::string m_pingpong_a;
    std::string m_pingpong_b;

    bool           fullscreen { false };
    bool           m_final_local { false };
    Box<SceneMesh> m_final_mesh;
    BlendMode      m_final_blend;
    bool           m_final_depth_test { false };
    bool           m_final_depth_write { false };
    CullMode       m_final_cull_mode { CullMode::None };
    std::string    m_final_target { SpecTex_Default };
    std::string    m_final_camera;
    bool           m_skip_when_no_runtime_effect { false };
    bool           m_resolved { false };

    std::vector<std::shared_ptr<SceneImageEffect>>                    m_effects;
    std::shared_ptr<SceneImageEffect>                                 m_final_resolve_effect;
    std::vector<SceneImageEffect*>                                    m_resolved_effects;
    std::vector<SceneImageEffectNode>                                 m_prefill_nodes;
    std::unordered_map<SceneImageEffectNode*, EffectNodeResolveState> m_node_resolve_state;
    std::unordered_map<SceneImageEffect::Command*, EffectCommandResolveState>
        m_command_resolve_state;
};

struct SceneImageEffectRef {
    SceneImageEffectLayer*            layer { nullptr };
    std::shared_ptr<SceneImageEffect> effect;
};

// ============================================================================
// ScenePostProcess.h
// First-class global post-process. Each pass uses the scene's default
// fullscreen NDC quad as its mesh - no camera, no transform, no image.
// Runs after main scene-graph traversal and writes through SpecTex_Default.
// ============================================================================

struct ScenePostProcessPass {
    rstd::sync::Arc<SceneNode> node;   // synthetic; mesh + material only
    std::string                output; // RT key; empty -> SpecTex_Default
};

struct ScenePostProcessCopy {
    std::string src;
    std::string dst;
};

struct ScenePostProcess {
    using Step = std::variant<ScenePostProcessPass, ScenePostProcessCopy>;
    std::string       name;
    std::vector<Step> steps;
};

// SceneLight + SceneLightType live in the `wescene.scene:lighting` partition
// (see src/Scene/Lighting/Lighting.cppm).

// ============================================================================
// Particle.h
// ============================================================================

struct Particle {
    struct InitValue {
        Eigen::Vector3f color { 1.0f, 1.0f, 1.0f };
        float           alpha { 1.0f };
        float           size { 20 };
        float           lifetime { 1.0f };
    };
    Eigen::Vector3f position { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f color { 1.0f, 1.0f, 1.0f };
    float           alpha { 1.0f };
    float           size { 20 };
    float           lifetime { 1.0f };

    Eigen::Vector3f rotation { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f velocity { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f acceleration { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f angularVelocity { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f angularAcceleration { 0.0f, 0.0f, 0.0f };

    float     random { 0.0f };
    bool      mark_new { true };
    InitValue init {};
};

// ============================================================================
// ParticleEmitter.h
// ============================================================================

struct ParticleControlpoint {
    bool            link_mouse { false };
    bool            worldspace { false };
    Eigen::Vector3d base_offset { 0, 0, 0 };
    Eigen::Vector3d offset { 0, 0, 0 };
};

struct ParticleInfo {
    std::span<Particle>                   particles;
    std::span<const ParticleControlpoint> controlpoints;
    Eigen::Matrix3d                       world_from_local_dir { Eigen::Matrix3d::Identity() };
    Eigen::Matrix3d                       local_from_world_dir { Eigen::Matrix3d::Identity() };
    bool                                  world_space { false };
    double                                time;
    double                                time_pass;
};

using ParticleInitOp     = std::function<void(Particle&, double)>;
using ParticleOperatorOp = std::function<void(const ParticleInfo&)>;

using ParticleEmittOp = std::function<void(
    std::vector<Particle>&, std::vector<ParticleInitOp>&, u32 maxcount, double timepass,
    std::span<const float> audio_average, std::span<const ParticleControlpoint> controlpoints)>;

struct ParticleAudioResponse {
    bool                  enable { false };
    float                 amount { 1.0f };
    float                 exponent { 1.0f };
    rstd::array<float, 2> frequency { 0.0f, 15.0f };
    rstd::array<float, 2> bounds { 0.0f, 1.0f };
};

struct ParticleFollowAnchor {
    bool  trail_renderer { false };
    float length { 0.0f };
    float max_length { 0.0f };
    float texture_ratio { 1.0f };
};

struct ParticleBoxEmitterArgs {
    rstd::array<float, 3> directions;
    rstd::array<float, 3> minDistance;
    rstd::array<float, 3> maxDistance;
    float                 emitSpeed;
    rstd::array<float, 3> orgin;
    bool                  one_per_frame;
    bool                  sort;
    u32                   instantaneous;
    float                 minSpeed;
    float                 maxSpeed;
    float                 duration { 0.0f };
    i32                   controlpoint { 0 };
    ParticleAudioResponse audio_response;

    static ParticleEmittOp MakeEmittOp(ParticleBoxEmitterArgs);
};

struct ParticleSphereEmitterArgs {
    rstd::array<float, 3> directions;
    float                 minDistance;
    float                 maxDistance;
    float                 emitSpeed;
    rstd::array<float, 3> orgin;
    rstd::array<i32, 3>   sign;
    bool                  one_per_frame;
    bool                  sort;
    u32                   instantaneous;
    float                 minSpeed;
    float                 maxSpeed;
    float                 duration { 0.0f };
    i32                   controlpoint { 0 };
    ParticleAudioResponse audio_response;

    static ParticleEmittOp MakeEmittOp(ParticleSphereEmitterArgs);
};

// IParticleRawGener uses ParticleRawGenSpecOp; declare the spec types early.
struct ParticleRawGenSpec {
    float* lifetime;
};
using ParticleRawGenSpecOp = std::function<void(const Particle&, const ParticleRawGenSpec&)>;

// ============================================================================
// ParticleModify.h
// ============================================================================

namespace ParticleModify
{

inline void Move(Particle& p, const Eigen::Vector3d& acc) noexcept {
    p.position = (p.position.cast<double>() + acc).cast<float>();
}
inline void Move(Particle& p, double x, double y, double z) noexcept { Move(p, { x, y, z }); }

inline void MoveTo(Particle& p, const Eigen::Vector3d& pos) noexcept {
    p.position = pos.cast<float>();
}
inline void MoveTo(Particle& p, double x, double y, double z) noexcept { MoveTo(p, { x, y, z }); }

inline void MoveToNegZ(Particle& p) noexcept { p.position.z() = -std::abs(p.position.z()); }

inline void MoveByTime(Particle& p, double t) noexcept { Move(p, p.velocity.cast<double>() * t); }

inline void MoveMultiply(Particle& p, const Eigen::Vector3d& para) noexcept {
    p.position = para.cwiseProduct(p.position.cast<double>()).cast<float>();
}
inline void MoveMultiply(Particle& p, double x, double y, double z) noexcept {
    MoveMultiply(p, { x, y, z });
}

inline void MoveApplySign(Particle& p, i32 x, i32 y, i32 z) noexcept {
    if (x != 0) {
        p.position[0] = std::abs(p.position[0]) * (float)x;
    }
    if (y != 0) {
        p.position[1] = std::abs(p.position[1]) * (float)y;
    }
    if (z != 0) {
        p.position[2] = std::abs(p.position[2]) * (float)z;
    }
}
inline void SphereDirectOffset(Particle& p, const Eigen::Vector3d& base, double direct) noexcept {
    using namespace Eigen;
    Vector3d axis  = base.cross(p.position.cast<double>()).normalized();
    Affine3d trans = Affine3d::Identity();
    trans.prerotate(AngleAxis<double>(direct, axis));
    p.position = (trans * p.position.cast<double>()).cast<float>();
}

inline void RotatePos(Particle& p, double x, double y, double z) noexcept {
    using namespace Eigen;
    Affine3d trans = Affine3d::Identity();

    trans.prerotate(AngleAxis<double>(y, Vector3d::UnitY()));
    trans.prerotate(AngleAxis<double>(x, Vector3d::UnitX()));
    trans.prerotate(AngleAxis<double>(-z, Vector3d::UnitZ()));
    p.position = (trans * p.position.cast<double>()).cast<float>();
}

inline void ChangeLifetime(Particle& p, double l) noexcept { p.lifetime += l; }

inline double LifetimePos(const Particle& p) {
    if (p.lifetime < 0) return 1.0;
    return 1.0 - (p.lifetime / p.init.lifetime);
}

inline double LifetimePassed(const Particle& p) noexcept { return p.init.lifetime - p.lifetime; }

inline bool LifetimeOk(const Particle& p) noexcept { return p.lifetime > 0.0f; }

void ChangeRotation(Particle&, float x, float y, float z);

inline void ChangeColor(Particle& p, const Eigen::Vector3d& c) noexcept {
    p.color = (p.color.cast<double>() + c).cast<float>();
}
inline void ChangeColor(Particle& p, double r, double g, double b) { ChangeColor(p, { r, g, b }); }

inline void ChangeRotation(Particle& p, const Eigen::Vector3d& r) noexcept {
    p.rotation = (p.rotation.cast<double>() + r).cast<float>();
}
inline void ChangeRotation(Particle& p, double x, double y, double z) {
    ChangeRotation(p, { x, y, z });
}

inline void ChangeVelocity(Particle& p, const Eigen::Vector3d& v) noexcept {
    p.velocity = (p.velocity.cast<double>() + v).cast<float>();
}
inline void ChangeVelocity(Particle& p, double x, double y, double z) noexcept {
    ChangeVelocity(p, { x, y, z });
}
inline void Accelerate(Particle& p, const Eigen::Vector3d& acc, double t) noexcept {
    ChangeVelocity(p, acc * t);
}

inline void ChangeAngularVelocity(Particle& p, const Eigen::Vector3d& v) noexcept {
    p.angularVelocity = (p.angularVelocity.cast<double>() + v).cast<float>();
}
inline void ChangeAngularVelocity(Particle& p, double x, double y, double z) noexcept {
    ChangeAngularVelocity(p, { x, y, z });
}
inline void AngularAccelerate(Particle& p, const Eigen::Vector3d& acc, double t) noexcept {
    ChangeAngularVelocity(p, acc * t);
}

inline void Rotate(Particle& p, const Eigen::Vector3d& r) noexcept {
    p.rotation = (p.rotation.cast<double>() + r).cast<float>();
}
inline void Rotate(Particle& p, double x, double y, double z) noexcept { Rotate(p, { x, y, z }); }

inline void RotateByTime(Particle& p, double t) noexcept {
    Rotate(p, p.angularVelocity.cast<double>() * t);
}

inline void MutiplyAlpha(Particle& p, double a) { p.alpha *= a; }
inline void MutiplySize(Particle& p, double s) { p.size *= s; }

inline void MutiplyColor(Particle& p, const Eigen::Vector3d& c) {
    p.color = c.cwiseProduct(p.color.cast<double>()).cast<float>();
}
inline void MutiplyColor(Particle& p, double r, double g, double b) {
    MutiplyColor(p, { r, g, b });
}
inline void MutiplyVelocity(Particle& p, double m) { p.velocity *= m; }

inline void ChangeSize(Particle& p, double s) { p.size += s; }
inline void ChangeAlpha(Particle& p, double a) { p.alpha += a; }

inline void InitLifetime(Particle& p, float l) noexcept {
    p.lifetime      = l;
    p.init.lifetime = l;
}
inline void InitSize(Particle& p, double s) {
    p.size      = s;
    p.init.size = s;
}
inline void InitAlpha(Particle& p, double a) {
    p.alpha      = a;
    p.init.alpha = a;
}
inline void InitColor(Particle& p, double r, double g, double b) {
    Eigen::Vector3d c { r, g, b };
    p.color      = c.cast<float>();
    p.init.color = p.color;
}

inline void InitVelocity(Particle& p, const Eigen::Vector3d& v) { p.velocity = v.cast<float>(); }
inline void InitVelocity(Particle& p, double x, double y, double z) {
    InitVelocity(p, { x, y, z });
}

inline void MutiplyInitLifeTime(Particle& p, double m) {
    p.lifetime *= m;
    p.init.lifetime = p.lifetime;
}
inline void MutiplyInitAlpha(Particle& p, double m) {
    p.alpha *= m;
    p.init.alpha = p.alpha;
}
inline void MutiplyInitSize(Particle& p, double m) {
    p.size *= m;
    p.init.size = p.size;
}
inline void MutiplyInitColor(Particle& p, double r, double g, double b) {
    MutiplyColor(p, { r, g, b });
    p.init.color = p.color;
}

inline void Reset(Particle& p) {
    p.alpha = p.init.alpha;
    p.size  = p.init.size;
    p.color = p.init.color;
}

inline void MarkOld(Particle& p) { p.mark_new = false; }
inline bool IsNew(const Particle& p) { return p.mark_new; }

inline const Eigen::Vector3f& GetPos(const Particle& p) { return p.position; }
inline const Eigen::Vector3f& GetVelocity(const Particle& p) { return p.velocity; }
inline const Eigen::Vector3f& GetAngular(const Particle& p) { return p.rotation; }

}; // namespace ParticleModify

// ============================================================================
// ParticleSystem.h
// ============================================================================

enum class ParticleAnimationMode
{
    SEQUENCE,
    RANDOMONE,
};

class ParticleSystem;
class ParticleSubSystem;

// Per-slot trail history for rope-head particles. positions[head] is the
// newest; len counts valid samples (0..capacity). Capacity is decided by the
// SubSystem and is the same for every slot in one instance.
struct ParticleTrail {
    std::vector<Eigen::Vector3f> positions;
    u16                          head { 0 };
    u16                          len { 0 };

    void Reset() noexcept {
        head = 0;
        len  = 0;
    }
    void Push(const Eigen::Vector3f& p) noexcept {
        if (positions.empty()) return;
        head            = static_cast<u16>((head + 1) % positions.size());
        positions[head] = p;
        if (len < positions.size()) len++;
    }
    // Returns oldest -> newest sample at logical index i in [0, len).
    Eigen::Vector3f At(u16 i) const noexcept {
        // newest is at head; oldest is len-1 back from head.
        auto cap = static_cast<u16>(positions.size());
        auto idx = static_cast<u16>((head + cap - (len - 1 - i)) % cap);
        return positions[idx];
    }
};

class ParticleInstance : NoCopy, NoMove {
public:
    struct BoundedData {
        ParticleInstance*        parent { nullptr };
        const ParticleSubSystem* parent_subsystem { nullptr };
        isize                    particle_idx { -1 };

        bool            pre_lifetime_ok { true };
        Eigen::Vector3f pos { 0.0f, 0.0f, 0.0f };
    };

    void Refresh();

    bool IsDeath() const;
    void SetDeath(bool);

    bool IsNoLiveParticle() const;
    void SetNoLiveParticle(bool);

    std::span<const Particle> Particles() const;
    std::vector<Particle>&    ParticlesVec();

    // Parallel to ParticlesVec(); each slot has its own ring buffer of past
    // positions. SubSystem allocates capacity at construction (0 = no trail).
    std::span<const ParticleTrail> Trails() const;
    std::vector<ParticleTrail>&    TrailsVec();

    BoundedData&       GetBoundedData();
    const BoundedData& GetBoundedData() const;

private:
    bool                       m_is_death { false };
    bool                       m_no_live_particle { false };
    std::vector<Particle>      m_particles;
    std::vector<ParticleTrail> m_trails;
    BoundedData                m_bounded_data;
};

class ParticleSubSystem : NoCopy, NoMove {
public:
    enum class SpawnType
    {
        STATIC,
        EVENT_FOLLOW,
        EVENT_SPAWN,
        EVENT_DEATH,
    };

public:
    ParticleSubSystem(ParticleSystem& p, std::shared_ptr<SceneMesh> sm, u32 maxcount, double rate,
                      u32 maxcount_instance, double probability, SpawnType type,
                      ParticleRawGenSpecOp specOp, ParticleFollowAnchor follow_anchor = {},
                      u32 trail_length = 0, double start_time = 0.0, bool world_space = false);
    ~ParticleSubSystem();

    void Emitt();

    ParticleInstance* QueryNewInstance();

    void AddEmitter(ParticleEmittOp&&);
    void AddInitializer(ParticleInitOp&&);
    void AddOperator(ParticleOperatorOp&&);

    void AddChild(Box<ParticleSubSystem>&&);

    std::span<const ParticleControlpoint> Controlpoints() const;
    std::span<ParticleControlpoint>       Controlpoints();

    // The SceneNode this subsystem renders through. Owned by the scene graph;
    // used at
    // Emitt() time to map world-space inputs (link_mouse cursor) into
    // the particle's local emit space via the node's inverse model.
    void SetOwnerNode(SceneNode* n) { m_owner_node = n; }

    SpawnType       Type() const;
    u32             MaxInstanceCount() const;
    Eigen::Vector3f FollowPosition(const Particle& p) const;

private:
    void Tick(double frame_time, bool update_mesh);
    void Warmup();
    void Advance(double frame_time, bool update_mesh);

    ParticleSystem&              m_sys;
    std::shared_ptr<SceneMesh>   m_mesh;
    SceneNode*                   m_owner_node { nullptr };
    std::vector<ParticleEmittOp> m_emiters;

    std::vector<ParticleInitOp>     m_initializers;
    std::vector<ParticleOperatorOp> m_operators;

    rstd::array<ParticleControlpoint, 8> m_controlpoints;

    ParticleRawGenSpecOp m_genSpecOp;
    ParticleFollowAnchor m_follow_anchor;
    u32                  m_maxcount;
    double               m_rate;
    double               m_time;
    double               m_start_time { 0.0 };
    bool                 m_world_space { false };
    bool                 m_started { false };

    Vec<Box<ParticleSubSystem>> m_children;
    Vec<Box<ParticleInstance>>  m_instances;

    u32       m_maxcount_instance { 1 };
    double    m_probability { 1.0f };
    SpawnType m_spawn_type { SpawnType::STATIC };
    u32       m_trail_length { 0 };

public:
    u32 TrailLength() const { return m_trail_length; }
};

// ============================================================================
// Interface/IParticleRawGener.h (relocated here so it can use ParticleInstance / SceneMesh)
// ============================================================================

class IParticleRawGener {
public:
    IParticleRawGener()          = default;
    virtual ~IParticleRawGener() = default;

    virtual void GenGLData(std::span<const Box<ParticleInstance>>, SceneMesh&,
                           ParticleRawGenSpecOp&) = 0;
};

class Scene;
class ParticleSystem : NoCopy, NoMove {
public:
    ParticleSystem(Scene& scene): scene(scene) {};
    ~ParticleSystem() = default;

    void Emitt();

    Scene& scene;

    Vec<Box<ParticleSubSystem>>        subsystems;
    std::unique_ptr<IParticleRawGener> gener;
};

// ============================================================================
// WPParticleRawGener.h
// ============================================================================

class WPParticleRawGener : public IParticleRawGener {
public:
    WPParticleRawGener() {};
    virtual ~WPParticleRawGener() {};

    virtual void GenGLData(std::span<const Box<ParticleInstance>>, SceneMesh&,
                           ParticleRawGenSpecOp&);
};

// ============================================================================
// Interface/IShaderValueUpdater.h (relocated)
// ============================================================================

using sprite_map_t    = Map<usize, SpriteAnimation>;
using UpdateUniformOp = std::function<void(std::string_view, ShaderValue)>;
using ExistsUniformOp = std::function<bool(std::string_view)>;

class IShaderValueUpdater : NoCopy, NoMove {
public:
    IShaderValueUpdater()          = default;
    virtual ~IShaderValueUpdater() = default;

    virtual void FrameBegin()                                                      = 0;
    virtual void InitUniforms(SceneNode*, const ExistsUniformOp&)                  = 0;
    virtual void UpdateUniforms(SceneNode*, sprite_map_t&, const UpdateUniformOp&) = 0;
    virtual void FrameEnd()                                                        = 0;

    virtual void MouseInput(double x, double y)                     = 0;
    virtual void SetTexelSize(float x, float y)                     = 0;
    virtual void SetScreenSize(i32 w, i32 h)                        = 0;
    virtual void SetAudioSpectrum(std::span<const float, 64> left,
                                  std::span<const float, 64> right) = 0;
    virtual void SetCameraParallaxMouseInfluence(float value)       = 0;
    virtual void SetCameraShakeEnabled(bool value)                  = 0;
    virtual void SetCameraShakeAmplitude(float value)               = 0;
    virtual void SetCameraShakeSpeed(float value)                   = 0;
    virtual void SetCameraShakeRoughness(float value)               = 0;
};

// ============================================================================
// Interface/IImageParser.h (relocated)
// ============================================================================

class IImageParser {
public:
    IImageParser()                                                        = default;
    virtual ~IImageParser()                                               = default;
    virtual std::shared_ptr<Image>              Parse(const std::string&) = 0;
    virtual std::vector<std::shared_ptr<Image>> ParseMany(std::span<const std::string> names) {
        std::vector<std::shared_ptr<Image>> images;
        images.reserve(names.size());
        for (const auto& name : names) images.push_back(Parse(name));
        return images;
    }
    virtual ImageHeader ParseHeader(const std::string&) = 0;
};

struct SceneNodeId {
    u32 index { std::numeric_limits<u32>::max() };
    u32 generation { 0 };
};

struct SceneMaterialId {
    u32 index { std::numeric_limits<u32>::max() };
    u32 generation { 0 };
};

struct SceneMeshId {
    u32 index { std::numeric_limits<u32>::max() };
    u32 generation { 0 };
};

struct SceneDrawItemId {
    u32 index { std::numeric_limits<u32>::max() };
    u32 generation { 0 };
};

struct SceneTextureId {
    u32 index { std::numeric_limits<u32>::max() };
    u32 generation { 0 };
};

struct SceneRenderTargetId {
    u32 index { std::numeric_limits<u32>::max() };
    u32 generation { 0 };
};

struct SceneCameraId {
    u32 index { std::numeric_limits<u32>::max() };
    u32 generation { 0 };
};

struct WallpaperLayerId {
    i32 value { -1 };
};

struct SceneLayerId {
    i32 value { -1 };
    u32 generation { 0 };
};

struct SceneDrawItemRecord {
    SceneDrawItemId id;
    SceneNodeId     node;
    SceneMeshId     mesh;
    SceneMaterialId material;
    u32             submesh_index { 0 };
};

struct DrawItemView {
    SceneNode*                node { nullptr };
    SceneMesh*                mesh { nullptr };
    const SceneMesh::Submesh* submesh { nullptr };
    SceneMaterial*            material { nullptr };
    u32                       submesh_index { 0 };
};

class SceneResourceIndex : NoCopy, NoMove {
public:
    SceneResourceIndex() = default;

    void Rebuild(Scene& scene, u32 generation);

    u32  Generation() const { return m_generation; }
    bool Empty() const { return m_scene == nullptr; }

    std::span<const SceneDrawItemRecord> DrawItems() const {
        return { m_draw_items.data(), m_draw_items.size() };
    }
    std::span<SceneNode* const> Nodes() const { return { m_nodes.data(), m_nodes.size() }; }

    Option<SceneNodeId>         nodeId(const SceneNode& node) const;
    Option<SceneMeshId>         meshId(const SceneMesh& mesh) const;
    Option<SceneMaterialId>     materialId(const SceneMaterial& material) const;
    Option<SceneDrawItemId>     drawItemFor(SceneNodeId node, u32 submesh_index) const;
    Option<SceneTextureId>      textureId(std::string_view url) const;
    Option<SceneRenderTargetId> renderTargetId(std::string_view key) const;
    Option<SceneCameraId>       cameraId(std::string_view name) const;

    Option<DrawItemView>            resolve(SceneDrawItemId id) const;
    SceneNode*                      node(SceneNodeId id) const;
    SceneMesh*                      mesh(SceneMeshId id) const;
    SceneMaterial*                  material(SceneMaterialId id) const;
    const SceneTexture*             texture(SceneTextureId id) const;
    const SceneRenderTarget*        renderTarget(SceneRenderTargetId id) const;
    SceneRenderTarget*              mutableRenderTarget(SceneRenderTargetId id) const;
    SceneCamera*                    camera(SceneCameraId id) const;
    std::span<SceneMesh* const>     Meshes() const { return { m_meshes.data(), m_meshes.size() }; }
    std::span<SceneMaterial* const> Materials() const {
        return { m_materials.data(), m_materials.size() };
    }

private:
    Scene* m_scene { nullptr };
    u32    m_generation { 0 };

    std::vector<SceneNode*>          m_nodes;
    std::vector<SceneMesh*>          m_meshes;
    std::vector<SceneMaterial*>      m_materials;
    std::vector<std::string>         m_texture_keys;
    std::vector<std::string>         m_render_target_keys;
    std::vector<std::string>         m_camera_keys;
    std::vector<SceneDrawItemRecord> m_draw_items;

    std::unordered_map<const SceneNode*, SceneNodeId>         m_node_ids;
    std::unordered_map<const SceneMesh*, SceneMeshId>         m_mesh_ids;
    std::unordered_map<const SceneMaterial*, SceneMaterialId> m_material_ids;
    std::unordered_map<std::string, SceneTextureId>           m_texture_ids;
    std::unordered_map<std::string, SceneRenderTargetId>      m_render_target_ids;
    std::unordered_map<std::string, SceneCameraId>            m_camera_ids;
};

struct RenderSceneVersion {
    u64 value { 0 };
};

struct RenderItemId {
    u32 index { std::numeric_limits<u32>::max() };
    u64 generation { 0 };

    bool Valid() const noexcept {
        return index != std::numeric_limits<u32>::max() && generation != 0;
    }
};

struct RenderTextureDescId {
    u32 index { std::numeric_limits<u32>::max() };
    u64 generation { 0 };
};

struct RenderTargetDescId {
    u32 index { std::numeric_limits<u32>::max() };
    u64 generation { 0 };
};

struct RenderItemRecord {
    RenderItemId               id;
    SceneDrawItemId            scene_draw_item;
    SceneNodeId                scene_node;
    SceneMeshId                scene_mesh;
    SceneMaterialId            scene_material;
    WallpaperLayerId           source_layer;
    u32                        submesh_index { 0 };
    Option<RenderTargetDescId> output_override;
};

struct RenderTextureDescRecord {
    RenderTextureDescId id;
    SceneTextureId      scene_texture;
    std::string         key;
    SceneTexture        desc;
};

struct RenderTargetDescRecord {
    RenderTargetDescId  id;
    SceneRenderTargetId scene_render_target;
    std::string         key;
    SceneRenderTarget   desc;
};

struct RenderLinkSourceRecord {
    WallpaperLayerId   source_layer;
    std::string        render_target_key;
    RenderTargetDescId render_target;
};

struct SceneMeshDirtyEvent {
    SceneMeshId         mesh;
    SceneMeshDirtyFlags flags { SceneMeshDirtyNone };
};

struct SceneMaterialDirtyEvent {
    SceneMaterialId         material;
    SceneMaterialDirtyFlags flags { SceneMaterialDirtyNone };
};

struct SceneMaterialTextureSlotMutation {
    bool                    changed { false };
    Option<SceneMaterialId> material;
};

struct SceneShaderVariantMutation {
    std::shared_ptr<SceneShader> shader;
    SceneShaderVariantDesc       variant;
};

struct SceneMaterialShaderVariantMutation {
    bool                    changed { false };
    Option<SceneMaterialId> material;
};

enum class SceneUserPropertyDiagnosticCode
{
    SceneVfsUnavailable,
    UnsupportedShaderComboValue,
    MissingShaderVariantDescriptor,
    ShaderComboCompileFailed,
};

struct SceneUserPropertyDiagnostic {
    std::string                     key;
    SceneUserPropertyDiagnosticCode code {
        SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue
    };
    std::string material;
    std::string combo;
    std::string message;
};

class RenderSceneSnapshot {
public:
    RenderSceneSnapshot() = default;

    void Rebuild(Scene& scene, RenderSceneVersion version);

    RenderSceneVersion Version() const { return m_version; }

    std::span<const RenderItemRecord> RenderItems() const {
        return { m_render_items.data(), m_render_items.size() };
    }
    std::span<const RenderTextureDescRecord> TextureDescs() const {
        return { m_texture_descs.data(), m_texture_descs.size() };
    }
    std::span<const RenderTargetDescRecord> RenderTargetDescs() const {
        return { m_render_target_descs.data(), m_render_target_descs.size() };
    }

    const RenderItemRecord*        renderItem(RenderItemId id) const;
    const RenderTextureDescRecord* textureDesc(RenderTextureDescId id) const;
    const RenderTargetDescRecord*  renderTargetDesc(RenderTargetDescId id) const;

    Option<RenderItemId>          renderItemFor(SceneDrawItemId id) const;
    Option<RenderTextureDescId>   textureDescId(std::string_view key) const;
    Option<RenderTargetDescId>    renderTargetDescId(std::string_view key) const;
    std::span<const RenderItemId> renderItemsFor(WallpaperLayerId id) const;
    std::span<const RenderItemId> renderItemsFor(SceneMaterialId id) const;
    std::span<const RenderItemId> renderItemsFor(SceneMeshId id) const;
    const RenderLinkSourceRecord* linkSource(WallpaperLayerId id) const;
    const Set<i32>&               LinkedLayerIds() const { return m_linked_layer_ids; }
    bool                          HasLinkConsumer(WallpaperLayerId id) const;

private:
    RenderSceneVersion m_version;

    std::vector<RenderItemRecord>        m_render_items;
    std::vector<RenderTextureDescRecord> m_texture_descs;
    std::vector<RenderTargetDescRecord>  m_render_target_descs;

    std::unordered_map<u64, RenderItemId>                m_render_item_ids;
    std::unordered_map<std::string, RenderTextureDescId> m_texture_desc_ids;
    std::unordered_map<std::string, RenderTargetDescId>  m_render_target_desc_ids;
    std::unordered_map<i32, std::vector<RenderItemId>>   m_source_layer_items;
    std::unordered_map<u64, std::vector<RenderItemId>>   m_material_render_items;
    std::unordered_map<u64, std::vector<RenderItemId>>   m_mesh_render_items;
    std::vector<RenderLinkSourceRecord>                  m_link_sources;
    std::unordered_map<i32, u32>                         m_link_source_ids;
    Set<i32>                                             m_linked_layer_ids;
};

RenderSceneSnapshot ExtractRenderSceneSnapshot(Scene& scene);

// ============================================================================
// Scene.h
// ============================================================================

class Scene : NoCopy, NoMove {
public:
    Scene();
    ~Scene();

    std::unordered_map<std::string, SceneTexture>      textures;
    std::unordered_map<std::string, SceneRenderTarget> renderTargets;

    std::unordered_map<std::string, std::shared_ptr<SceneCamera>> cameras;
    std::unordered_map<std::string, std::vector<std::string>>     linkedCameras;
    std::vector<std::shared_ptr<SceneCameraPath>>                 camera_paths;

    // WE layer IDs the render-graph build may elide when nothing links to
    // them, or route to `_rt_link_<id>` when something does. Two flavours
    // land here:
    //   * `visible: false` at parse time (user-hidden layer that may still
    //     act as a link source for another layer's composite).
    //   * no-effect fullscreen / compose layers (recurring identity
    //     passthrough on `_rt_default` — only useful as a link snapshot
    //     point if referenced).
    Set<i32> elidable_layer_ids;
    Set<i32> static_elidable_layer_ids;
    Set<i32> visibility_elidable_layer_ids;

    Vec<Box<SceneLight>> lights;

    // user-property key → list of (material pointer, GLSL uniform name) pairs
    // pulled out of every material's shader-side `u_*` annotations during
    // parse. Reads sit at `WPUniformVar::material` (UI key) / `name` (GLSL
    // identifier). Lets a future RenderSetUserProperty handler push the new
    // value into the affected materials' `customShader.constValues` without
    // a per-frame walk over the scene tree.
    Map<std::string, std::vector<std::pair<SceneMaterial*, std::string>>> shader_user_var_index;

    struct ShaderComboUserBinding {
        SceneMaterial*                material { nullptr };
        std::string                   combo;
        std::string                   fallback;
        Map<std::string, std::string> options;
    };
    Map<std::string, std::vector<ShaderComboUserBinding>> shader_combo_user_index;

    // user-property key → list of (override state, field name) pairs for the
    // particle layers whose instanceoverride was authored as `{user:"<key>"}`.
    // Mutated by RenderSetUserProperty; the shared_ptr is also captured by
    // every initializer / operator closure on the relevant subsystem. The
    // state is type-erased because the real type
    // `owe::wpscene::ParticleInstanceoverride` is attached to wescene.pkg.parse,
    // which already imports wescene.scene — pulling it in here would create
    // a cycle.
    struct ParticleOverrideBinding {
        std::shared_ptr<void> state;
        std::string           field;
    };
    Map<std::string, std::vector<ParticleOverrideBinding>> particle_user_var_index;

    Map<std::string, std::vector<std::shared_ptr<SceneSoundControl>>> sound_volume_user_index;

    struct ImagePropertyBinding {
        SceneNode*                  node { nullptr };
        std::vector<SceneMaterial*> materials;
    };
    Map<std::string, std::vector<ImagePropertyBinding>> image_color_user_index;
    Map<std::string, std::vector<ImagePropertyBinding>> image_alpha_user_index;

    void RegisterUserTextBinding(std::string key, std::function<void(std::string_view)> setter) {
        m_text_user_index[std::move(key)].push_back(std::move(setter));
    }
    bool ApplyUserTextBindings(std::string_view key, const Json& property) {
        auto it = m_text_user_index.find(key);
        if (it == m_text_user_index.end()) return false;

        auto value = SceneJsonScalarString(SceneUserPropertyPayload(property));
        if (value.is_none()) return false;
        for (const auto& setter : it->second) setter(*value);
        return true;
    }

    struct MaterialTextureUserBinding {
        SceneMaterial* material { nullptr };
        u32            slot { 0 };
        std::string    fallback;
    };
    Map<std::string, std::vector<MaterialTextureUserBinding>> material_texture_user_index;

    Map<std::string, std::vector<std::string>> camera_parallax_user_var_index;

    Map<std::string, std::vector<std::string>> camera_shake_user_var_index;

    Map<std::string, std::vector<std::shared_ptr<SceneCameraPath>>> camera_path_user_index;

    Option<SceneImageEffectRef> FindNodeImageEffect(const SceneNode& node, std::string_view name);
    bool SetImageEffectRuntimeVisible(const SceneImageEffectRef& ref, bool visible);
    bool ConsumeRenderGraphDirty() {
        bool dirty           = m_render_graph_dirty;
        m_render_graph_dirty = false;
        return dirty;
    }
    bool ApplyUserNodeVisibilityBindings(std::string_view key, const Json& property);
    bool ApplyUserImageEffectVisibilityBindings(std::string_view key, const Json& property);
    bool ApplyUserLightVisibilityBindings(std::string_view key, const Json& property);
    bool ApplyUserCameraPathVisibilityBindings(std::string_view key, const Json& property);

    // Scene-tree root. After parse handoff to the render thread, the tree
    // shape under `sceneGraph` is immutable until Scene destruction (see the
    // invariant on SceneNode). Render-graph build is read-only; script ticks
    // only mutate per-node transform / visibility fields.
    rstd::sync::Arc<SceneNode>           sceneGraph;
    std::unique_ptr<IShaderValueUpdater> shaderValueUpdater;
    std::unique_ptr<IImageParser>        imageParser;

    // Opaque holder for fs::VFS. fs::VFS is module-attached to wescene.fs; if
    // we forward-declare it here it would conflict with the module-attached
    // declaration in any TU that imports wescene.fs and imports wescene.scene.
    using VFSDeleterFn = void (*)(void*) noexcept;
    std::unique_ptr<void, VFSDeleterFn> vfs;

    // Same opaque-pointer pattern for the per-Scene scenescript runtime.
    // The concrete type is `owe::script::ScriptScene` (defined in
    // wescene-script), but Scene itself lives in wescene-base which sits
    // upstream of wescene-script — so we keep it opaque here. The renderer
    // ticks it once per frame via `owe::script::TickSceneScripts`.
    using ScriptDeleterFn = void (*)(void*) noexcept;
    std::unique_ptr<void, ScriptDeleterFn> script_scene { nullptr, [](void*) noexcept {
                                                         } };

    // Scene-owned text::FontCache. Multiple text objects with matching
    // (font_blob, pixel_size) share a FontFace + its 1024² atlas. Populated
    // lazily by ParseTextObj via text::EnsureSceneFontCache.
    using FontCacheDeleterFn = void (*)(void*) noexcept;
    std::unique_ptr<void, FontCacheDeleterFn> font_cache { nullptr, [](void*) noexcept {
                                                          } };

    std::string scene_id { "unknown_id" };

    bool first_frame_ok { false };

    SceneMesh default_effect_mesh;

    std::vector<std::shared_ptr<ScenePostProcess>> post_processes;

    Box<ParticleSystem> paritileSys;

    SceneCamera* activeCamera;

    rstd::array<float, 2>               pointerPosition { 0.5f, 0.5f };
    rstd::array<std::atomic<float>, 16> audioAverage {};

    i32                   ortho[2] { 1920, 1080 };
    rstd::array<float, 3> clearColor { 1.0f, 1.0f, 1.0f };
    std::string           clearColorUserKey;

    double elapsingTime { 0.0f }, frameTime { 0.0f };
    void   PassFrameTime(double t) {
        frameTime = t;
        elapsingTime += t;
    }
    void                                     TickNodeFieldAnimations();
    std::vector<std::function<void(double)>> transform_updaters;
    void                                     TickTransformUpdaters();

    void UpdateLinkedCamera(const std::string& name) {
        if (linkedCameras.count(name) != 0) {
            auto& cams = linkedCameras.at(name);
            for (auto& cam : cams) {
                if (cameras.count(cam) != 0) {
                    cameras.at(cam)->Clone(*cameras.at(name));
                    cameras.at(cam)->Update();
                }
            }
        }
    }

    void        TickCameraPaths();
    void        TickMaterialShaderAnimations();
    void        CaptureCameraPathViewports();
    std::string EnsureLinkRenderTarget(WallpaperLayerId source_layer, const SceneNode& source_node);
    bool        EnsureTextureDescriptor(std::string_view key);
    bool        SetMaterialShaderValue(SceneMaterial& material, std::string_view uniform_name,
                                       const ShaderValue& value);
    SceneMaterialTextureSlotMutation SetMaterialTextureSlot(SceneMaterial& material, u32 slot,
                                                            std::string_view texture);
    SceneMaterialShaderVariantMutation
         SetMaterialShaderVariant(SceneMaterial& material, SceneShaderVariantMutation mutation);
    void MarkLayerStaticElidable(WallpaperLayerId id);
    void MarkLayerVisibilityElidable(WallpaperLayerId id);
    void RegisterRenderGroup(WallpaperLayerId id, std::string camera) {
        m_render_group_cameras[id.value] = std::move(camera);
    }
    Option<std::string_view> RenderGroupCamera(WallpaperLayerId id) const {
        auto it = m_render_group_cameras.find(id.value);
        if (it == m_render_group_cameras.end()) return None();
        return Some(std::string_view(it->second));
    }
    bool                                 SetNodeVisible(SceneNode& node, bool visible);
    std::vector<SceneMeshDirtyEvent>     ConsumePreparedMeshDirtyEvents();
    std::vector<SceneMaterialDirtyEvent> ConsumePreparedMaterialDirtyEvents();
    void                                 ClearUserPropertyDiagnostics(std::string_view key);
    void AddUserPropertyDiagnostic(SceneUserPropertyDiagnostic diagnostic);
    std::span<const SceneUserPropertyDiagnostic> UserPropertyDiagnostics() const {
        return { m_user_property_diagnostics.data(), m_user_property_diagnostics.size() };
    }

    void                      RebuildResourceIndex();
    SceneResourceIndex&       ResourceIndex() { return m_resource_index; }
    const SceneResourceIndex& ResourceIndex() const { return m_resource_index; }
    u32                       ResourceGeneration() const { return m_resource_generation; }

private:
    Map<std::string, std::vector<std::function<void(std::string_view)>>> m_text_user_index;

    void RebuildElidableLayerIds();

    u32                                      m_resource_generation { 0 };
    SceneResourceIndex                       m_resource_index;
    bool                                     m_render_graph_dirty { false };
    Map<i32, std::string>                    m_render_group_cameras;
    std::vector<SceneUserPropertyDiagnostic> m_user_property_diagnostics;
};

} // namespace owe
