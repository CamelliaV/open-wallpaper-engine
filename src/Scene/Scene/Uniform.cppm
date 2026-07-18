export module wescene.scene:uniform;
import :id;
import :runtime;
import eigen;
import rstd;
import rstd.cppstd;
import wescene.core;

using namespace rstd::prelude;

export namespace owe
{

using ShaderValueInter = rstd::array<float, 16>;

struct UniformValueView {
    const f32* data { nullptr };
    usize      size { 0 };
};

class ShaderValue {
public:
    using value_type = float;

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
    }

    const auto& operator[](usize index) const { return value()[index]; }
    auto& operator[](usize index) { return m_dynamic ? m_dynamic_value[index] : m_value[index]; }

    auto  data() const noexcept { return value().data(); }
    usize size() const noexcept { return m_size; }
    auto  View() const noexcept -> UniformValueView { return { data(), size() }; }

    void setSize(usize size) noexcept { m_size = rstd::cmp::min(size, value().size()); }

private:
    void fromSpan(std::span<const value_type> values) noexcept;

    std::span<const value_type> value() const noexcept {
        if (m_dynamic) return m_dynamic_value;
        return m_value;
    }

    bool                    m_dynamic { false };
    ShaderValueInter        m_value;
    std::vector<value_type> m_dynamic_value;
    usize                   m_size { 0 };
};

using UniformValue   = ShaderValue;
using ShaderValues   = Map<std::string, ShaderValue>;
using ShaderValueMap = ShaderValues;

struct UniformOutputId {
    u32 value { rstd::u32_::MAX };

    bool        Valid() const noexcept { return value != rstd::u32_::MAX; }
    friend bool operator==(const UniformOutputId&, const UniformOutputId&) = default;
};

struct UniformSourceId {
    u32 index { rstd::u32_::MAX };
    u32 generation { 0 };

    bool        Valid() const noexcept { return index != rstd::u32_::MAX && generation != 0; }
    friend bool operator==(const UniformSourceId&, const UniformSourceId&) = default;
};

struct UniformSourceAttachment {
    UniformSourceId source;
    i32             priority { 0 };
};

enum class UniformScalarType : u8
{
    Float32,
};

struct UniformValueShape {
    UniformScalarType scalar { UniformScalarType::Float32 };
    u32               min_elements { 0 };
    u32               max_elements { 0 };

    static auto Float(u32 elements) -> UniformValueShape {
        return { .min_elements = elements, .max_elements = elements };
    }
    static auto FloatRange(u32 min_elements, u32 max_elements) -> UniformValueShape {
        return { .min_elements = min_elements, .max_elements = max_elements };
    }
};

struct UniformError {
    String message;
};

struct UniformBindingSink {
    using Trait                  = UniformBindingSink;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformBindingSink;

        auto Bind(UniformOutputId output, std::string_view shader_member,
                  UniformValueShape shape = {}) -> Result<bool, UniformError> {
            return rstd::trait_call<0>(this, output, shader_member, shape);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Bind>;
};

struct UniformValueSink {
    using Trait                  = UniformValueSink;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformValueSink;

        bool Wants(UniformOutputId output) const { return rstd::trait_call<0>(this, output); }
        auto Write(UniformOutputId output, UniformValueView value) -> Result<empty, UniformError> {
            return rstd::trait_call<1>(this, output, value);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Wants, &T::Write>;
};

struct UniformTextureView {
    bool                has_extent { false };
    rstd::array<f32, 2> source_extent { 0.0f, 0.0f };
    rstd::array<f32, 2> sample_extent { 0.0f, 0.0f };
    bool                has_mipmap { false };
    f32                 mipmap_level { 0.0f };
    bool                has_transform { false };
    rstd::array<f32, 4> rotation { 1.0f, 0.0f, 0.0f, 1.0f };
    rstd::array<f32, 2> translation { 0.0f, 0.0f };
    u64                 revision { 1 };
};

struct UniformResourceView {
    using Trait                  = UniformResourceView;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformResourceView;

        auto Texture(usize texture_index) const -> Option<UniformTextureView> {
            return rstd::trait_call<0>(this, texture_index);
        }
        auto Viewport() const -> rstd::array<f32, 2> { return rstd::trait_call<1>(this); }
        auto TexelSize() const -> rstd::array<f32, 2> { return rstd::trait_call<2>(this); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Texture, &T::Viewport, &T::TexelSize>;
};

struct UniformUpdateContext {
    using Trait                  = UniformUpdateContext;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformUpdateContext;

        auto Frame() const -> ref<SceneFrame> { return rstd::trait_call<0>(this); }
        auto Resources() const -> ref<dyn<UniformResourceView>> {
            return rstd::trait_call<1>(this);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Frame, &T::Resources>;
};

struct UniformSource {
    using Trait                  = UniformSource;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformSource;

        auto Describe(mut_ref<dyn<UniformBindingSink>> sink) const -> Result<empty, UniformError> {
            return rstd::trait_call<0>(this, sink);
        }
        auto Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
            return rstd::trait_call<1>(this, context);
        }
        auto Evaluate(ref<dyn<UniformUpdateContext>> context,
                      mut_ref<dyn<UniformValueSink>> sink) const -> Result<empty, UniformError> {
            return rstd::trait_call<2>(this, context, sink);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Describe, &T::Version, &T::Evaluate>;
};

struct UniformSourceRegistrar {
    using Trait                  = UniformSourceRegistrar;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformSourceRegistrar;

        auto Register(Box<dyn<UniformSource>> source) -> UniformSourceId {
            return rstd::trait_call<0>(this, rstd::move(source));
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Register>;
};

struct UniformAttachmentWriter {
    using Trait                  = UniformAttachmentWriter;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformAttachmentWriter;

        bool AttachGlobal(UniformSourceId source, i32 priority = 0) {
            return rstd::trait_call<0>(this, source, priority);
        }
        bool AttachNode(SceneNodeId node, UniformSourceId source, i32 priority = 0) {
            return rstd::trait_call<1>(this, node, source, priority);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::AttachGlobal, &T::AttachNode>;
};

struct UniformSourceCatalog {
    using Trait                  = UniformSourceCatalog;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformSourceCatalog;

        auto Resolve(UniformSourceId id) const -> Option<ref<dyn<UniformSource>>> {
            return rstd::trait_call<0>(this, id);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Resolve>;
};

struct UniformAttachmentCatalog {
    using Trait                  = UniformAttachmentCatalog;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformAttachmentCatalog;

        auto GlobalSources() const -> slice<UniformSourceAttachment> {
            return rstd::trait_call<0>(this);
        }
        auto NodeSources(SceneNodeId node) const -> slice<UniformSourceAttachment> {
            return rstd::trait_call<1>(this, node);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::GlobalSources, &T::NodeSources>;
};

class SceneUniformRegistry {
public:
    auto Register(Box<dyn<UniformSource>> source) -> UniformSourceId {
        auto id = UniformSourceId {
            .index      = static_cast<u32>(m_sources.len()),
            .generation = m_generation,
        };
        m_sources.push(rstd::move(source));
        return id;
    }

    template<typename T>
    auto RegisterSource(T source) -> UniformSourceId {
        return Register(Box<dyn<UniformSource>>::make(rstd::move(source)));
    }

    auto Resolve(UniformSourceId id) const -> Option<ref<dyn<UniformSource>>> {
        if (! id.Valid() || id.generation != m_generation || id.index >= m_sources.len()) {
            return None();
        }
        return Some(m_sources[id.index].as_ref());
    }

    bool AttachGlobal(UniformSourceId source, i32 priority = 0) {
        if (Resolve(source).is_none()) return false;
        return AttachUnique(m_global_sources, source, priority);
    }

    bool AttachNode(SceneNodeId node, UniformSourceId source, i32 priority = 0) {
        if (! node.Valid() || Resolve(source).is_none()) return false;
        auto key         = IdKey(node);
        auto attachments = m_node_sources.get_mut(key);
        if (attachments.is_none()) {
            (void)m_node_sources.insert(key, Vec<UniformSourceAttachment>::make());
            attachments = m_node_sources.get_mut(key);
        }
        return AttachUnique(**attachments, source, priority);
    }

    auto GlobalSources() const -> slice<UniformSourceAttachment> {
        return m_global_sources.as_slice();
    }

    auto NodeSources(SceneNodeId node) const -> slice<UniformSourceAttachment> {
        auto found = m_node_sources.get(IdKey(node));
        return found.is_some() ? (**found).as_slice() : slice<UniformSourceAttachment> {};
    }

    void Reset() {
        m_sources.clear();
        m_global_sources.clear();
        m_node_sources.clear();
        ++m_generation;
        if (m_generation == 0) ++m_generation;
    }

    usize Size() const noexcept { return m_sources.len(); }

private:
    static u64 IdKey(SceneNodeId id) {
        return (static_cast<u64>(id.generation) << 32) | static_cast<u64>(id.index);
    }

    static bool AttachUnique(Vec<UniformSourceAttachment>& attachments, UniformSourceId source,
                             i32 priority) {
        for (auto& attachment : attachments) {
            if (attachment.source != source) continue;
            attachment.priority = priority;
            return true;
        }
        attachments.push(UniformSourceAttachment { .source = source, .priority = priority });
        return true;
    }

    Vec<Box<dyn<UniformSource>>>                                  m_sources;
    Vec<UniformSourceAttachment>                                  m_global_sources;
    rstd::collections::HashMap<u64, Vec<UniformSourceAttachment>> m_node_sources;
    u32                                                           m_generation { 1 };
};

} // namespace owe
