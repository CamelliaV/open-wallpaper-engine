module;

#include <cstdint>

#include "Image.hpp"

export module wescene.text;
import cppstd;
import wescene.scene;

// Stage A surface: UTF-8 decoding + a FreeType-backed glyph cache that
// rasterises codepoints on demand into a single CPU-side R8 atlas. The
// scene-graph emission for text layers is Stage B and lives behind
// ParseTextObj in WPSceneParser; this module does not depend on Scene.

export namespace wallpaper::text
{

inline std::vector<std::uint32_t> DecodeUtf8(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint8_t  b0   = static_cast<std::uint8_t>(s[i]);
        std::uint32_t cp   = 0;
        std::size_t   need = 0;
        if (b0 < 0x80) {
            cp   = b0;
            need = 0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp   = b0 & 0x1Fu;
            need = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp   = b0 & 0x0Fu;
            need = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp   = b0 & 0x07u;
            need = 3;
        } else {
            out.push_back(0xFFFDu);
            ++i;
            continue;
        }
        if (i + need >= s.size()) {
            out.push_back(0xFFFDu);
            break;
        }
        bool ok = true;
        for (std::size_t j = 1; j <= need; ++j) {
            std::uint8_t bj = static_cast<std::uint8_t>(s[i + j]);
            if ((bj & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (bj & 0x3Fu);
        }
        if (! ok) {
            out.push_back(0xFFFDu);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += 1 + need;
    }
    return out;
}

struct GlyphInfo {
    // Position inside the atlas, pixels.
    std::uint32_t atlas_x { 0 };
    std::uint32_t atlas_y { 0 };
    std::uint32_t pixel_w { 0 };
    std::uint32_t pixel_h { 0 };
    // FreeType bearings + advance, fractional pixels.
    float         bearing_x { 0.0f };
    float         bearing_y { 0.0f };
    float         advance_x { 0.0f };
};

struct FontMetrics {
    float         ascender { 0.0f };
    float         descender { 0.0f };
    float         line_height { 0.0f };
    std::uint32_t pixel_size { 0 };
    std::uint32_t atlas_w { 0 };
    std::uint32_t atlas_h { 0 };
};

class FontFace {
public:
    FontFace();
    ~FontFace();
    FontFace(FontFace&&) noexcept;
    FontFace& operator=(FontFace&&) noexcept;
    FontFace(const FontFace&)            = delete;
    FontFace& operator=(const FontFace&) = delete;

    // Returns a stable pointer into the internal cache. Lazily rasterises
    // missing glyphs into the atlas; returns nullptr only on hard FreeType
    // failure (rare — even .notdef is normally available as glyph index 0).
    const GlyphInfo* Glyph(std::uint32_t codepoint);
    FontMetrics      Metrics() const;
    // R8 atlas, row-major, atlas_w * atlas_h bytes. Stable across Glyph()
    // calls until the dirty flag is cleared and the next miss happens.
    std::span<const std::uint8_t> AtlasPixels() const;
    bool                          AtlasDirty() const noexcept;
    void                          ClearDirty() noexcept;

private:
    friend class FontCache;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class FontCache {
public:
    FontCache();
    ~FontCache();
    FontCache(const FontCache&)            = delete;
    FontCache& operator=(const FontCache&) = delete;

    // Acquires (or reuses) a face for the given font blob at the given pixel
    // size. The blob is hashed for keying, then kept alive internally.
    // Returns nullptr if FreeType cannot open the blob.
    FontFace* GetFace(std::span<const std::byte> blob, std::uint32_t pixel_size);

    struct ResolvedBlob {
        std::shared_ptr<std::vector<std::byte>> bytes;
        std::string                             source;  // path or "in-pkg:..."
    };

    // Resolves a font reference. Tries:
    //   1. exact path on the host filesystem
    //   2. recursive search of /usr/share/fonts and $XDG_DATA_HOME/fonts
    //      (capped at ~2k entries)
    //   3. first available .ttf/.otf in /usr/share/fonts as last-resort
    //      fallback (when fallback_to_any == true)
    // Returns {nullptr, ""} if nothing matches.
    static ResolvedBlob ResolveSystemFont(std::string_view name,
                                          bool             fallback_to_any = true);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Snapshot the face's atlas pixels into a renderer-consumable Image (R8,
// single slot, single mipmap, LINEAR/CLAMP_TO_EDGE sampler). The returned
// Image owns its pixel buffer; the FontFace can subsequently mutate or be
// destroyed without affecting the snapshot.
std::shared_ptr<wallpaper::Image> BuildAtlasImage(const FontFace& face,
                                                   const std::string& key);

// Lazily compiles the embedded text HLSL shader (one-time, process-wide
// cached) and returns a ready-to-bind SceneShader. The shader expects:
//   - vertex inputs: a_Position (float3), a_TexCoord (float2),
//                    a_Color (float4)
//   - uniform block ww_Uniforms with member g_ModelViewProjectionMatrix
//   - combined image sampler g_Texture0 (R8 atlas; .r = coverage)
// Returns nullptr if the SPIR-V compile fails.
std::shared_ptr<wallpaper::SceneShader> GetTextSceneShader();

} // namespace wallpaper::text
