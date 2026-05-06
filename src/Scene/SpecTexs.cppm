module;

#include "Utils/String.h"  // STRTONUM macro (uses __SHORT_FILE__)

export module wescene.spec_texs;
import cppstd;

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

inline constexpr std::string_view WE_SPEC_PREFIX { "_rt_" };
inline constexpr std::string_view WE_IMAGE_LAYER_COMPOSITE_PREFIX { "_rt_imageLayerComposite_" };
inline constexpr std::string_view WE_HALF_COMPO_BUFFER_PREFIX { "_rt_HalfCompoBuffer" };
inline constexpr std::string_view WE_QUARTER_COMPO_BUFFER_PREFIX { "_rt_QuarterCompoBuffer" };
inline constexpr std::string_view WE_FULL_COMPO_BUFFER_PREFIX { "_rt_FullCompoBuffer" };
inline constexpr std::string_view WE_MIP_MAPPED_FRAME_BUFFER { "_rt_MipMappedFrameBuffer" };

inline constexpr std::string_view WE_EFFECT_PPONG_PREFIX { "_rt_effect_pingpong_" };
inline constexpr std::string_view WE_EFFECT_PPONG_PREFIX_A { "_rt_effect_pingpong_a_" };
inline constexpr std::string_view WE_EFFECT_PPONG_PREFIX_B { "_rt_effect_pingpong_b_" };

inline constexpr std::string_view WE_IN_POSITION { "a_Position" };
inline constexpr std::string_view WE_IN_TEXCOORD { "a_TexCoord" };
inline constexpr std::string_view WE_IN_BLENDINDICES { "a_BlendIndices" };
inline constexpr std::string_view WE_IN_BLENDWEIGHTS { "a_BlendWeights" };

// particle

inline constexpr std::string_view WE_IN_POSITIONVEC4 { "a_PositionVec4" };
inline constexpr std::string_view WE_IN_COLOR { "a_Color" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC4 { "a_TexCoordVec4" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC4C1 { "a_TexCoordVec4C1" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC4C2 { "a_TexCoordVec4C2" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC4C3 { "a_TexCoordVec4C3" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC3C2 { "a_TexCoordVec3C2" };
inline constexpr std::string_view WE_IN_TEXCOORDC2 { "a_TexCoordC2" };
inline constexpr std::string_view WE_IN_TEXCOORDC3 { "a_TexCoordC3" };
inline constexpr std::string_view WE_IN_TEXCOORDC4 { "a_TexCoordC4" };
inline constexpr std::string_view WE_CB_THICK_FORMAT { "THICKFORMAT" };
inline constexpr std::string_view WE_PRENDER_ROPE { "PRENDER_ROPE" };

inline constexpr std::string_view G_M { "g_ModelMatrix" };
inline constexpr std::string_view G_VP { "g_ViewProjectionMatrix" };
inline constexpr std::string_view G_MVP { "g_ModelViewProjectionMatrix" };
inline constexpr std::string_view G_AM { "g_AltModelMatrix" };
inline constexpr std::string_view G_MI { "g_ModelMatrixInverse" };
inline constexpr std::string_view G_MVPI { "g_ModelViewProjectionMatrixInverse" };
inline constexpr std::string_view G_ETVP { "g_EffectTextureProjectionMatrix" };
inline constexpr std::string_view G_ETVPI { "g_EffectTextureProjectionMatrixInverse" };
inline constexpr std::string_view G_LP { "g_LightsPosition" };
inline constexpr std::string_view G_LCP { "g_LightsColorPremultiplied" };

inline constexpr std::string_view G_TIME { "g_Time" };
inline constexpr std::string_view G_DAYTIME { "g_DayTime" };
inline constexpr std::string_view G_POINTERPOSITION { "g_PointerPosition" };
inline constexpr std::string_view G_TEXELSIZE { "g_TexelSize" };
inline constexpr std::string_view G_TEXELSIZEHALF { "g_TexelSizeHalf" };
inline constexpr std::string_view G_BONES { "g_Bones" };
inline constexpr std::string_view G_SCREEN { "g_Screen" };
inline constexpr std::string_view G_PARALLAXPOSITION { "g_ParallaxPosition" };

inline constexpr std::string_view SpecTex_Default { "_rt_default" };
inline constexpr std::string_view SpecTex_Link { "_rt_link_" };

inline bool IsSpecTex(const std::string_view name) {
    return name.starts_with(WE_SPEC_PREFIX);
}
inline bool IsSpecLinkTex(const std::string_view name) {
    return name.starts_with(SpecTex_Link);
}
inline std::uint32_t ParseLinkTex(const std::string_view name) {
    std::string sid { name };
    sid = sid.substr(9);
    std::uint32_t result { 0 };
    STRTONUM(sid, result);
    return result;
}
inline std::string GenLinkTex(std::ptrdiff_t id) {
    return std::string(SpecTex_Link) + std::to_string(id);
}

} // namespace owe

#undef BASE_GLTEX_NAMES
