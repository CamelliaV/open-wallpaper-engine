module;

#include <rstd/macro.hpp>

export module wescene.spec_names;
import rstd;
import rstd.cppstd;
import rstd.log;
import wescene.types;

using namespace rstd::prelude;
using namespace rstd::literals;

#define BASE_GLTEX_NAMES(ext)                                                                      \
    "g_Texture0" #ext, "g_Texture1" #ext, "g_Texture2" #ext, "g_Texture3" #ext, "g_Texture4" #ext, \
        "g_Texture5" #ext, "g_Texture6" #ext, "g_Texture7" #ext, "g_Texture8" #ext,                \
        "g_Texture9" #ext, "g_Texture10" #ext, "g_Texture11" #ext, "g_Texture12" #ext

export namespace owe
{

inline constexpr std::array WE_GLTEX_NAMES { BASE_GLTEX_NAMES() };
inline constexpr std::array WE_GLTEX_RESOLUTION_NAMES { BASE_GLTEX_NAMES(Resolution) };
inline constexpr std::array WE_GLTEX_ROTATION_NAMES { BASE_GLTEX_NAMES(Rotation) };
inline constexpr std::array WE_GLTEX_TRANSLATION_NAMES { BASE_GLTEX_NAMES(Translation) };
inline constexpr std::array WE_GLTEX_MIPMAPINFO_NAMES { BASE_GLTEX_NAMES(MipMapInfo) };
inline constexpr std::array WE_GLTEX_TEXEL_NAMES { BASE_GLTEX_NAMES(Texel) };

inline constexpr ref<str> WE_SPEC_PREFIX = "_rt_"_str;
// --- Names that originate from Wallpaper Engine content
inline constexpr ref<str> WE_IMAGE_LAYER_COMPOSITE_PREFIX = "_rt_imageLayerComposite_"_str;
inline constexpr ref<str> WE_FULL_COMPO_BUFFER_PREFIX     = "_rt_FullCompoBuffer"_str;
inline constexpr ref<str> WE_HALF_COMPO_BUFFER_PREFIX     = "_rt_HalfCompoBuffer"_str;
inline constexpr ref<str> WE_QUARTER_COMPO_BUFFER_PREFIX  = "_rt_QuarterCompoBuffer"_str;
inline constexpr ref<str> WE_EIGHT_COMPO_BUFFER_PREFIX    = "_rt_EightBuffer"_str;
inline constexpr ref<str> WE_FULL_FRAME_BUFFER            = "_rt_FullFrameBuffer"_str;
inline constexpr ref<str> WE_MIP_MAPPED_FRAME_BUFFER      = "_rt_MipMappedFrameBuffer"_str;
// Other WE engine RTs seen in the assets.
inline constexpr ref<str> WE_SHADOW_ATLAS_PREFIX         = "_rt_shadowAtlas"_str;
inline constexpr ref<str> WE_REFLECTION_PREFIX           = "_rt_Reflection"_str;
inline constexpr ref<str> WE_VOLUMETRICS_PREFIX          = "_rt_volumetrics"_str;
inline constexpr ref<str> WE_QUARTER_FORCE_RG_PREFIX     = "_rt_QuarterForceRG"_str;
inline constexpr ref<str> WE_BLOOM_PREFIX                = "_rt_Bloom"_str;
inline constexpr ref<str> WE_QUARTER_FRAME_BUFFER_PREFIX = "_rt_QuarterFrameBuffer"_str;
inline constexpr ref<str> WE_EIGHTH_FRAME_BUFFER_PREFIX  = "_rt_EighthFrameBuffer"_str;

// --- Names coined by owe ---
inline constexpr ref<str> OWE_EFFECT_PPONG_PREFIX   = "_rt_effect_pingpong_"_str;
inline constexpr ref<str> OWE_EFFECT_PPONG_PREFIX_A = "_rt_effect_pingpong_a_"_str;
inline constexpr ref<str> OWE_EFFECT_PPONG_PREFIX_B = "_rt_effect_pingpong_b_"_str;
inline constexpr ref<str> OWE_BLOOM_MIP_PREFIX      = "_rt_bloom_mip"_str;
inline constexpr ref<str> SpecTex_Default           = "_rt_default"_str;
inline constexpr ref<str> SpecTex_Link              = "_rt_link_"_str;

inline constexpr ref<str> WE_IN_POSITION     = "a_Position"_str;
inline constexpr ref<str> WE_IN_NORMAL       = "a_Normal"_str;
inline constexpr ref<str> WE_IN_TEXCOORD     = "a_TexCoord"_str;
inline constexpr ref<str> WE_IN_TANGENT4     = "a_Tangent4"_str;
inline constexpr ref<str> WE_IN_BLENDINDICES = "a_BlendIndices"_str;
inline constexpr ref<str> WE_IN_BLENDWEIGHTS = "a_BlendWeights"_str;
inline constexpr ref<str> WE_IN_CENTER       = "a_Center"_str;
inline constexpr ref<str> WE_IN_COLOR4U      = "a_Color4u"_str;
inline constexpr ref<str> WE_IN_POSITIONC1   = "a_PositionC1"_str;

// particle

inline constexpr ref<str> WE_IN_POSITIONVEC4         = "a_PositionVec4"_str;
inline constexpr ref<str> WE_IN_COLOR                = "a_Color"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC4         = "a_TexCoordVec4"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC4C1       = "a_TexCoordVec4C1"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC4C2       = "a_TexCoordVec4C2"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC4C3       = "a_TexCoordVec4C3"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC3C2       = "a_TexCoordVec3C2"_str;
inline constexpr ref<str> WE_IN_TEXCOORDC2           = "a_TexCoordC2"_str;
inline constexpr ref<str> WE_IN_TEXCOORDC3           = "a_TexCoordC3"_str;
inline constexpr ref<str> WE_IN_TEXCOORDC4           = "a_TexCoordC4"_str;
inline constexpr ref<str> WE_CB_BLENDMODE            = "BLENDMODE"_str;
inline constexpr ref<str> WE_CB_BONECOUNT            = "BONECOUNT"_str;
inline constexpr ref<str> WE_CB_SPRITESHEET          = "SPRITESHEET"_str;
inline constexpr ref<str> WE_CB_SPRITESHEETBLENDNPOT = "SPRITESHEETBLENDNPOT"_str;
inline constexpr ref<str> WE_CB_THICK_FORMAT         = "THICKFORMAT"_str;
inline constexpr ref<str> WE_CB_TRAILRENDERER        = "TRAILRENDERER"_str;
inline constexpr ref<str> WE_CB_GS_ENABLED           = "GS_ENABLED"_str;
inline constexpr ref<str> WE_CB_LIGHTING             = "LIGHTING"_str;
inline constexpr ref<str> WE_CB_REFLECTION           = "REFLECTION"_str;
inline constexpr ref<str> WE_CB_NORMALMAP            = "NORMALMAP"_str;
inline constexpr ref<str> WE_CB_MORPHING             = "MORPHING"_str;
inline constexpr ref<str> WE_CB_SKINNING             = "SKINNING"_str;
inline constexpr ref<str> WE_CB_POINTEMITTER         = "POINTEMITTER"_str;
inline constexpr ref<str> WE_CB_LINEEMITTER          = "LINEEMITTER"_str;
inline constexpr ref<str> WE_PRENDER_ROPE            = "PRENDER_ROPE"_str;
inline constexpr ref<str> WE_PRENDER_ROPE_TRAIL      = "PRENDER_ROPE_TRAIL"_str;

// Compile-time (name, type) pair for declarative attribute layouts.
struct VertexAttrSpec {
    ref<str>   name;
    VertexType type;
    bool       padding { true };
};

namespace VAttr
{
inline constexpr VertexAttrSpec Position { WE_IN_POSITION, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec Normal { WE_IN_NORMAL, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec PositionVec4 { WE_IN_POSITIONVEC4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoord { WE_IN_TEXCOORD, VertexType::FLOAT2 };
inline constexpr VertexAttrSpec Tangent4 { WE_IN_TANGENT4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4 { WE_IN_TEXCOORDVEC4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C1 { WE_IN_TEXCOORDVEC4C1, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C2 { WE_IN_TEXCOORDVEC4C2, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C3 { WE_IN_TEXCOORDVEC4C3, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec3C2 { WE_IN_TEXCOORDVEC3C2, VertexType::FLOAT3, false };
inline constexpr VertexAttrSpec TexCoordC2 { WE_IN_TEXCOORDC2, VertexType::FLOAT2 };
inline constexpr VertexAttrSpec TexCoordC3 { WE_IN_TEXCOORDC3, VertexType::FLOAT2, false };
inline constexpr VertexAttrSpec TexCoordC4 { WE_IN_TEXCOORDC4, VertexType::FLOAT2, false };
inline constexpr VertexAttrSpec Color { WE_IN_COLOR, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec BlendIndices { WE_IN_BLENDINDICES, VertexType::UINT4 };
inline constexpr VertexAttrSpec BlendWeights { WE_IN_BLENDWEIGHTS, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec Center { WE_IN_CENTER, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec Color4u { WE_IN_COLOR4U, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec PositionC1 { WE_IN_POSITIONC1, VertexType::FLOAT3 };
} // namespace VAttr

inline constexpr ref<str> G_M                       = "g_ModelMatrix"_str;
inline constexpr ref<str> G_VP                      = "g_ViewProjectionMatrix"_str;
inline constexpr ref<str> G_MVP                     = "g_ModelViewProjectionMatrix"_str;
inline constexpr ref<str> G_AM                      = "g_AltModelMatrix"_str;
inline constexpr ref<str> G_ALTVIEWPROJECTIONMATRIX = "g_AltViewProjectionMatrix"_str;
inline constexpr ref<str> G_MI                      = "g_ModelMatrixInverse"_str;
inline constexpr ref<str> G_MVPI                    = "g_ModelViewProjectionMatrixInverse"_str;
inline constexpr ref<str> G_EYEPOSITION             = "g_EyePosition"_str;
inline constexpr ref<str> G_EFFECTMODELMATRIX       = "g_EffectModelMatrix"_str;
inline constexpr ref<str> G_EFFECTMODELVIEWPROJECTIONMATRIX =
    "g_EffectModelViewProjectionMatrix"_str;
inline constexpr ref<str> G_EFFECTMODELVIEWPROJECTIONMATRIXINVERSE =
    "g_EffectModelViewProjectionMatrixInverse"_str;
inline constexpr ref<str> G_EFFECTTEXTUREPROJECTIONMATRIX = "g_EffectTextureProjectionMatrix"_str;
inline constexpr ref<str> G_EFFECTTEXTUREPROJECTIONMATRIXINVERSE =
    "g_EffectTextureProjectionMatrixInverse"_str;
inline constexpr ref<str> G_LAYERMODELMATRIX   = "g_LayerModelMatrix"_str;
inline constexpr ref<str> G_EMVP               = "g_EffectModelViewProjectionMatrix"_str;
inline constexpr ref<str> G_ETVP               = "g_EffectTextureProjectionMatrix"_str;
inline constexpr ref<str> G_ETVPI              = "g_EffectTextureProjectionMatrixInverse"_str;
inline constexpr ref<str> G_LP                 = "g_LightsPosition"_str;
inline constexpr ref<str> G_LCP                = "g_LightsColorPremultiplied"_str;
inline constexpr ref<str> G_LCR                = "g_LightsColorRadius"_str;
inline constexpr ref<str> G_LIGHTAMBIENTCOLOR  = "g_LightAmbientColor"_str;
inline constexpr ref<str> G_LIGHTSKYLIGHTCOLOR = "g_LightSkylightColor"_str;
inline constexpr ref<str> G_FOGDISTANCECOLOR   = "g_FogDistanceColor"_str;
inline constexpr ref<str> G_FOGDISTANCEPARAMS  = "g_FogDistanceParams"_str;
inline constexpr ref<str> G_FOGHEIGHTCOLOR     = "g_FogHeightColor"_str;
inline constexpr ref<str> G_FOGHEIGHTPARAMS    = "g_FogHeightParams"_str;

inline constexpr ref<str> G_TIME                           = "g_Time"_str;
inline constexpr ref<str> G_FRAMETIME                      = "g_Frametime"_str;
inline constexpr ref<str> G_DAYTIME                        = "g_Daytime"_str;
inline constexpr ref<str> G_DAYTIME_LEGACY                 = "g_DayTime"_str;
inline constexpr ref<str> G_POINTERPOSITION                = "g_PointerPosition"_str;
inline constexpr ref<str> G_POINTERPOSITIONLAST            = "g_PointerPositionLast"_str;
inline constexpr ref<str> G_TEXELSIZE                      = "g_TexelSize"_str;
inline constexpr ref<str> G_TEXELSIZEHALF                  = "g_TexelSizeHalf"_str;
inline constexpr ref<str> G_TEXTURE0SAMPLERSTATE           = "g_Texture0SamplerState"_str;
inline constexpr ref<str> G_BONES                          = "g_Bones"_str;
inline constexpr ref<str> G_BONESALPHA                     = "g_BonesAlpha"_str;
inline constexpr ref<str> G_SCREEN                         = "g_Screen"_str;
inline constexpr ref<str> G_PARALLAXPOSITION               = "g_ParallaxPosition"_str;
inline constexpr ref<str> G_MORPHWEIGHTS                   = "g_MorphWeights"_str;
inline constexpr ref<str> G_MORPHOFFSETS                   = "g_MorphOffsets"_str;
inline constexpr ref<str> G_VIEWPORTVIEWPROJECTIONMATRICES = "g_ViewportViewProjectionMatrices"_str;
inline constexpr ref<str> G_VIEWUP                         = "g_ViewUp"_str;
inline constexpr ref<str> G_VIEWRIGHT                      = "g_ViewRight"_str;
inline constexpr ref<str> G_VIEWFORWARD                    = "g_ViewForward"_str;
inline constexpr ref<str> G_ORIENTATIONUP                  = "g_OrientationUp"_str;
inline constexpr ref<str> G_ORIENTATIONRIGHT               = "g_OrientationRight"_str;
inline constexpr ref<str> G_ORIENTATIONFORWARD             = "g_OrientationForward"_str;
inline constexpr ref<str> G_NORMALMODELMATRIX              = "g_NormalModelMatrix"_str;
inline constexpr ref<str> G_COLOR4                         = "g_Color4"_str;
inline constexpr ref<str> G_COLOR                          = "g_Color"_str;
inline constexpr ref<str> G_ALPHA                          = "g_Alpha"_str;
inline constexpr ref<str> G_USERALPHA                      = "g_UserAlpha"_str;
inline constexpr ref<str> G_BRIGHTNESS                     = "g_Brightness"_str;
inline constexpr ref<str> G_RENDERVAR0                     = "g_RenderVar0"_str;
inline constexpr ref<str> G_RENDERVAR1                     = "g_RenderVar1"_str;
inline constexpr ref<str> G_RENDERVAR2                     = "g_RenderVar2"_str;
inline constexpr ref<str> G_AUDIOFREQUENCYMIN              = "g_AudioFrequencyMin"_str;
inline constexpr ref<str> G_AUDIOFREQUENCYMAX              = "g_AudioFrequencyMax"_str;

// WE audio-bar shaders read one of three (Left, Right) array pairs depending
// on the chosen Frequency Resolution combo. owe sources from wavsen's 64-bin
// log-spaced spectrum; we downsample to 16/32 by averaging neighboring bins.
inline constexpr ref<str> G_AUDIO_SPEC_16_L = "g_AudioSpectrum16Left"_str;
inline constexpr ref<str> G_AUDIO_SPEC_16_R = "g_AudioSpectrum16Right"_str;
inline constexpr ref<str> G_AUDIO_SPEC_32_L = "g_AudioSpectrum32Left"_str;
inline constexpr ref<str> G_AUDIO_SPEC_32_R = "g_AudioSpectrum32Right"_str;
inline constexpr ref<str> G_AUDIO_SPEC_64_L = "g_AudioSpectrum64Left"_str;
inline constexpr ref<str> G_AUDIO_SPEC_64_R = "g_AudioSpectrum64Right"_str;

inline bool IsSpecTex(ref<str> name) { return rstd::str_::starts_with(name, WE_SPEC_PREFIX); }
inline bool IsSpecLinkTex(ref<str> name) { return rstd::str_::starts_with(name, SpecTex_Link); }
inline u32  ParseLinkTex(ref<str> name) {
    auto suffix = rstd::str_::strip_prefix(name, SpecTex_Link);
    if (suffix.is_none()) {
        rstd_error("invalid linked texture id: {}", name);
        return u32();
    }
    auto result = rstd::from_str<u32>(*suffix);
    if (result.is_err()) {
        rstd_error("invalid linked texture id: {}", name);
        return u32();
    }
    return rstd::move(result).unwrap();
}
inline std::string GenLinkTex(std::ptrdiff_t id) {
    return rstd::cppstd::to_string(SpecTex_Link) + std::to_string(id);
}

inline bool IsImageLayerComposite(ref<str> name) {
    return rstd::str_::starts_with(name, WE_IMAGE_LAYER_COMPOSITE_PREFIX);
}
// Parse <id> from `_rt_imageLayerComposite_<id>[_a|_b]`; nullopt when it isn't a
// composite ref or no id digits follow the prefix.
inline std::optional<std::uint32_t> ParseImageLayerCompositeId(ref<str> name) {
    auto rest = rstd::str_::strip_prefix(name, WE_IMAGE_LAYER_COMPOSITE_PREFIX);
    if (rest.is_none()) return std::nullopt;
    usize         i {};
    std::uint32_t id = 0;
    for (; i < rest->size(); ++i) {
        auto value = (*rest)[i].to_primitive();
        if (value < '0' || value > '9') break;
        id = id * 10u + std::uint32_t(value - '0');
    }
    if (i == usize()) return std::nullopt;
    return id;
}

} // namespace owe

#undef BASE_GLTEX_NAMES
