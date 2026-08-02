export module wescene.pkg.parse:global_uniform;
import rstd;
import wescene.scene;
import wescene.spec_names;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe
{

enum class GlobalUniformProducer : rstd::uint8_t
{
    Frame,
    Audio,
    Light,
    Shadow,
};

struct GlobalUniformField {
    ref<str>              name;
    ref<str>              alias;
    ref<str>              type;
    GlobalUniformProducer producer;
    UniformOutputId       output;
    UniformValueShape     shape;
    u32                   offset;
};

inline constexpr u32      kGlobalUniformSet { u32(0) };
inline constexpr u32      kGlobalUniformBinding { u32(0) };
inline constexpr u32      kDrawUniformSet { u32(1) };
inline constexpr u32      kDrawUniformBinding { u32(0) };
inline constexpr u64      kGlobalUniformSchemaIdentity { u64(0x5745474c4f424102ULL) };
inline constexpr usize    kGlobalUniformBlockSize { usize(4432) };
inline constexpr ref<str> kGlobalUniformBlockName { "ww_GlobalUniforms"_str };
inline constexpr ref<str> kDrawUniformBlockName { "ww_DrawUniforms"_str };

inline auto GlobalUniformFields() -> slice<GlobalUniformField> {
    static const array<GlobalUniformField, 20> fields {
        GlobalUniformField { G_TIME,
                             {},
                             "float"_str,
                             GlobalUniformProducer::Frame,
                             UniformOutputId { u32(0) },
                             UniformValueShape::Float(u32(1)),
                             u32(0) },
        GlobalUniformField { G_FRAMETIME,
                             {},
                             "float"_str,
                             GlobalUniformProducer::Frame,
                             UniformOutputId { u32(1) },
                             UniformValueShape::Float(u32(1)),
                             u32(4) },
        GlobalUniformField { G_DAYTIME,
                             G_DAYTIME_LEGACY,
                             "float"_str,
                             GlobalUniformProducer::Frame,
                             UniformOutputId { u32(2) },
                             UniformValueShape::Float(u32(1)),
                             u32(8) },
        GlobalUniformField { G_POINTERPOSITION,
                             {},
                             "vec2"_str,
                             GlobalUniformProducer::Frame,
                             UniformOutputId { u32(3) },
                             UniformValueShape::Float(u32(2)),
                             u32(16) },
        GlobalUniformField { G_POINTERPOSITIONLAST,
                             {},
                             "vec2"_str,
                             GlobalUniformProducer::Frame,
                             UniformOutputId { u32(4) },
                             UniformValueShape::Float(u32(2)),
                             u32(24) },
        GlobalUniformField { G_PARALLAXPOSITION,
                             {},
                             "vec2"_str,
                             GlobalUniformProducer::Frame,
                             UniformOutputId { u32(5) },
                             UniformValueShape::Float(u32(2)),
                             u32(32) },
        GlobalUniformField { G_AUDIO_SPEC_16_L,
                             {},
                             "float[16]"_str,
                             GlobalUniformProducer::Audio,
                             UniformOutputId { u32(0) },
                             UniformValueShape::Float(u32(16)),
                             u32(48) },
        GlobalUniformField { G_AUDIO_SPEC_16_R,
                             {},
                             "float[16]"_str,
                             GlobalUniformProducer::Audio,
                             UniformOutputId { u32(1) },
                             UniformValueShape::Float(u32(16)),
                             u32(304) },
        GlobalUniformField { G_AUDIO_SPEC_32_L,
                             {},
                             "float[32]"_str,
                             GlobalUniformProducer::Audio,
                             UniformOutputId { u32(2) },
                             UniformValueShape::Float(u32(32)),
                             u32(560) },
        GlobalUniformField { G_AUDIO_SPEC_32_R,
                             {},
                             "float[32]"_str,
                             GlobalUniformProducer::Audio,
                             UniformOutputId { u32(3) },
                             UniformValueShape::Float(u32(32)),
                             u32(1072) },
        GlobalUniformField { G_AUDIO_SPEC_64_L,
                             {},
                             "float[64]"_str,
                             GlobalUniformProducer::Audio,
                             UniformOutputId { u32(4) },
                             UniformValueShape::Float(u32(64)),
                             u32(1584) },
        GlobalUniformField { G_AUDIO_SPEC_64_R,
                             {},
                             "float[64]"_str,
                             GlobalUniformProducer::Audio,
                             UniformOutputId { u32(5) },
                             UniformValueShape::Float(u32(64)),
                             u32(2608) },
        GlobalUniformField { G_LP,
                             {},
                             "vec3[4]"_str,
                             GlobalUniformProducer::Light,
                             UniformOutputId { u32(0) },
                             UniformValueShape::Float(u32(12)),
                             u32(3632) },
        GlobalUniformField { G_LCP,
                             {},
                             "vec4[3]"_str,
                             GlobalUniformProducer::Light,
                             UniformOutputId { u32(1) },
                             UniformValueShape::Float(u32(12)),
                             u32(3696) },
        GlobalUniformField { G_LCR,
                             {},
                             "vec4[4]"_str,
                             GlobalUniformProducer::Light,
                             UniformOutputId { u32(2) },
                             UniformValueShape::Float(u32(16)),
                             u32(3744) },
        GlobalUniformField { G_LIGHTDIRECTIONTYPE,
                             {},
                             "vec4[4]"_str,
                             GlobalUniformProducer::Light,
                             UniformOutputId { u32(3) },
                             UniformValueShape::Float(u32(16)),
                             u32(3808) },
        GlobalUniformField { G_LIGHTCONEEXPONENT,
                             {},
                             "vec4[4]"_str,
                             GlobalUniformProducer::Light,
                             UniformOutputId { u32(4) },
                             UniformValueShape::Float(u32(16)),
                             u32(3872) },
        GlobalUniformField { G_LIGHTCASTSHADOW,
                             {},
                             "float[4]"_str,
                             GlobalUniformProducer::Light,
                             UniformOutputId { u32(5) },
                             UniformValueShape::Float(u32(4)),
                             u32(3936) },
        GlobalUniformField { G_VIEWPORTVIEWPROJECTIONMATRICES,
                             {},
                             "mat4[6]"_str,
                             GlobalUniformProducer::Shadow,
                             UniformOutputId { u32(0) },
                             UniformValueShape::MatrixArray(u32(4), u32(4), usize(6), usize(6)),
                             u32(4000) },
        GlobalUniformField { G_SHADOWATLASTRANSFORMS,
                             {},
                             "vec4[3]"_str,
                             GlobalUniformProducer::Shadow,
                             UniformOutputId { u32(1) },
                             UniformValueShape::Float(u32(12)),
                             u32(4384) },
    };
    return fields.as_slice();
}

inline auto FindGlobalUniform(ref<str> name) -> Option<ref<GlobalUniformField>> {
    for (const auto& field : GlobalUniformFields()) {
        if (field.name == name || (! field.alias.is_empty() && field.alias == name)) {
            return Some(ref<GlobalUniformField>::from_raw_parts(rstd::addressof(field)));
        }
    }
    return None();
}

} // namespace owe
