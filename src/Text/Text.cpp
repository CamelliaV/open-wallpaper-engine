module;

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "Image.hpp"
#include "Type.hpp"
#include "Utils/Logging.h"

module wescene.text;
import cppstd;
import wescene.scene;
import wescene.shader_compile;

namespace wallpaper::text
{

namespace
{

constexpr std::uint32_t kAtlasInitialDim { 1024 };
constexpr std::uint32_t kAtlasMaxDim { 2048 };

// 1px white cell at (0,0) so a single-channel atlas can also serve solid-fill
// quads (e.g. the opaquebackground rectangle). Not exposed via Glyph().
constexpr std::uint32_t kWhiteCellSize { 4 };

class FtLibrary {
public:
    static FtLibrary& Get() {
        static FtLibrary inst;
        return inst;
    }
    FT_Library handle() const noexcept { return m_lib; }

private:
    FtLibrary() {
        if (FT_Init_FreeType(&m_lib) != 0) {
            LOG_ERROR("FT_Init_FreeType failed");
            m_lib = nullptr;
        }
    }
    ~FtLibrary() {
        if (m_lib != nullptr) FT_Done_FreeType(m_lib);
    }
    FtLibrary(const FtLibrary&)            = delete;
    FtLibrary& operator=(const FtLibrary&) = delete;

    FT_Library m_lib { nullptr };
};

std::uint64_t HashBlob(std::span<const std::byte> blob) {
    // FNV-1a 64. Good enough for keying — collisions only matter if the
    // caller hands us two genuinely different fonts whose hashes collide,
    // which is fine since the underlying FT_Face open is the source of truth
    // (we cache per-pixel-size below the blob).
    std::uint64_t h = 1469598103934665603ull;
    for (std::byte b : blob) {
        h ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
        h *= 1099511628211ull;
    }
    return h;
}

bool IsFontExt(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
}

std::shared_ptr<std::vector<std::byte>> ReadAll(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (! f) return nullptr;
    auto size = f.tellg();
    if (size <= 0) return nullptr;
    f.seekg(0, std::ios::beg);
    auto buf = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
    if (! f.read(reinterpret_cast<char*>(buf->data()), size)) return nullptr;
    return buf;
}

} // namespace

// -- FontFace -------------------------------------------------------------

struct FontFace::Impl {
    std::shared_ptr<std::vector<std::byte>> blob;
    FT_Face                                 face { nullptr };
    std::uint32_t                           pixel_size { 0 };

    std::uint32_t                          atlas_w { 0 };
    std::uint32_t                          atlas_h { 0 };
    std::vector<std::uint8_t>              atlas;
    bool                                   dirty { false };

    // Shelf packer state: pen advances along the current shelf, falls to a
    // new shelf when the next glyph won't fit horizontally.
    std::uint32_t pen_x { 0 };
    std::uint32_t pen_y { 0 };
    std::uint32_t shelf_h { 0 };

    std::unordered_map<std::uint32_t, GlyphInfo> glyphs;

    ~Impl() {
        if (face != nullptr) FT_Done_Face(face);
    }

    void SeedWhiteCell() {
        if (atlas_w == 0) return;
        for (std::uint32_t y = 0; y < kWhiteCellSize; ++y) {
            for (std::uint32_t x = 0; x < kWhiteCellSize; ++x) {
                atlas[y * atlas_w + x] = 0xFF;
            }
        }
        pen_x   = kWhiteCellSize + 1;
        pen_y   = 0;
        shelf_h = kWhiteCellSize;
        dirty   = true;
    }

    bool EnsureAtlas() {
        if (atlas_w != 0) return true;
        atlas_w = kAtlasInitialDim;
        atlas_h = kAtlasInitialDim;
        atlas.assign(static_cast<std::size_t>(atlas_w) * atlas_h, 0);
        SeedWhiteCell();
        return true;
    }

    // Atlas is intentionally non-growing: UVs are baked into vertex buffers
    // at parse time, so changing atlas_w/atlas_h after the first text object
    // in a shared face would invalidate every previously-emitted glyph quad.
    // On overflow we fall back to a tofu glyph (see Glyph()).
    bool GrowAtlas() { return false; }

    bool ReserveSlot(std::uint32_t w, std::uint32_t h, std::uint32_t& out_x,
                     std::uint32_t& out_y) {
        while (true) {
            if (pen_x + w > atlas_w) {
                pen_y += shelf_h + 1;
                pen_x   = 0;
                shelf_h = 0;
            }
            if (pen_y + h > atlas_h) {
                if (! GrowAtlas()) return false;
                continue;
            }
            out_x = pen_x;
            out_y = pen_y;
            pen_x += w + 1;
            if (h > shelf_h) shelf_h = h;
            return true;
        }
    }

    void Blit(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
              const std::uint8_t* src, std::uint32_t pitch) {
        for (std::uint32_t row = 0; row < h; ++row) {
            std::memcpy(&atlas[(y + row) * atlas_w + x], src + row * pitch, w);
        }
    }
};

FontFace::FontFace(): m_impl(std::make_unique<Impl>()) {}
FontFace::~FontFace()                              = default;
FontFace::FontFace(FontFace&&) noexcept            = default;
FontFace& FontFace::operator=(FontFace&&) noexcept = default;

FontMetrics FontFace::Metrics() const {
    FontMetrics m {};
    if (m_impl->face != nullptr && m_impl->face->size != nullptr) {
        const auto& sm  = m_impl->face->size->metrics;
        m.ascender      = static_cast<float>(sm.ascender) / 64.0f;
        m.descender     = static_cast<float>(sm.descender) / 64.0f;
        m.line_height   = static_cast<float>(sm.height) / 64.0f;
        m.pixel_size    = m_impl->pixel_size;
    }
    m.atlas_w = m_impl->atlas_w;
    m.atlas_h = m_impl->atlas_h;
    return m;
}

std::span<const std::uint8_t> FontFace::AtlasPixels() const {
    return std::span<const std::uint8_t>(m_impl->atlas);
}

bool FontFace::AtlasDirty() const noexcept { return m_impl->dirty; }
void FontFace::ClearDirty() noexcept { m_impl->dirty = false; }

const GlyphInfo* FontFace::Glyph(std::uint32_t codepoint) {
    auto& impl = *m_impl;
    if (auto it = impl.glyphs.find(codepoint); it != impl.glyphs.end()) {
        return &it->second;
    }
    if (impl.face == nullptr) return nullptr;
    impl.EnsureAtlas();

    FT_UInt glyph_index = FT_Get_Char_Index(impl.face, codepoint);
    // glyph_index 0 is .notdef and is still a valid renderable glyph for tofu.
    if (FT_Load_Glyph(impl.face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
        return nullptr;
    }
    FT_GlyphSlot g = impl.face->glyph;
    GlyphInfo    gi {};
    gi.pixel_w   = g->bitmap.width;
    gi.pixel_h   = g->bitmap.rows;
    gi.bearing_x = static_cast<float>(g->bitmap_left);
    gi.bearing_y = static_cast<float>(g->bitmap_top);
    gi.advance_x = static_cast<float>(g->advance.x) / 64.0f;

    if (gi.pixel_w == 0 || gi.pixel_h == 0) {
        // E.g. space — no bitmap. Still a valid cached entry.
        gi.atlas_x = 0;
        gi.atlas_y = 0;
        auto [it, _] = impl.glyphs.emplace(codepoint, gi);
        return &it->second;
    }

    if (! impl.ReserveSlot(gi.pixel_w, gi.pixel_h, gi.atlas_x, gi.atlas_y)) {
        // Atlas full. Return tofu-equivalent: the .notdef rect mapped to white
        // cell (1×1). Better than nullptr — caller still gets a positioned box.
        gi.atlas_x = 0;
        gi.atlas_y = 0;
        gi.pixel_w = kWhiteCellSize;
        gi.pixel_h = kWhiteCellSize;
        auto [it, _] = impl.glyphs.emplace(codepoint, gi);
        return &it->second;
    }

    impl.Blit(gi.atlas_x,
              gi.atlas_y,
              gi.pixel_w,
              gi.pixel_h,
              g->bitmap.buffer,
              static_cast<std::uint32_t>(g->bitmap.pitch));
    impl.dirty = true;

    auto [it, _] = impl.glyphs.emplace(codepoint, gi);
    return &it->second;
}

// -- FontCache ------------------------------------------------------------

struct FontCache::Impl {
    struct Key {
        std::uint64_t blob_hash;
        std::uint32_t pixel_size;
        bool          operator==(const Key& o) const noexcept {
            return blob_hash == o.blob_hash && pixel_size == o.pixel_size;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return std::hash<std::uint64_t> {}(k.blob_hash) ^
                   (std::hash<std::uint32_t> {}(k.pixel_size) << 1);
        }
    };
    std::unordered_map<Key, std::unique_ptr<FontFace>, KeyHash> faces;
    // Keep blobs alive for the cache lifetime so FT_Face's pointer into them
    // stays valid.
    std::vector<std::shared_ptr<std::vector<std::byte>>> blobs;
};

FontCache::FontCache(): m_impl(std::make_unique<Impl>()) {}
FontCache::~FontCache() = default;

FontFace* FontCache::GetFace(std::span<const std::byte> blob, std::uint32_t pixel_size) {
    if (blob.empty() || pixel_size == 0) return nullptr;

    Impl::Key key { HashBlob(blob), pixel_size };
    if (auto it = m_impl->faces.find(key); it != m_impl->faces.end()) {
        return it->second.get();
    }

    FT_Library lib = FtLibrary::Get().handle();
    if (lib == nullptr) return nullptr;

    auto face = std::make_unique<FontFace>();
    if (FT_New_Memory_Face(lib,
                           reinterpret_cast<const FT_Byte*>(blob.data()),
                           static_cast<FT_Long>(blob.size()),
                           0,
                           &face->m_impl->face) != 0) {
        LOG_ERROR("FT_New_Memory_Face failed");
        return nullptr;
    }
    if (FT_Set_Pixel_Sizes(face->m_impl->face, 0, pixel_size) != 0) {
        LOG_ERROR("FT_Set_Pixel_Sizes failed (px=%u)", pixel_size);
        return nullptr;
    }
    face->m_impl->pixel_size = pixel_size;

    auto* raw           = face.get();
    m_impl->faces[key]  = std::move(face);
    return raw;
}

FontCache::ResolvedBlob FontCache::ResolveSystemFont(std::string_view name, bool fallback_to_any) {
    namespace fs = std::filesystem;

    auto try_load = [](const fs::path& p) -> ResolvedBlob {
        if (! fs::exists(p) || ! fs::is_regular_file(p)) return { nullptr, {} };
        auto bytes = ReadAll(p);
        if (! bytes) return { nullptr, {} };
        return { std::move(bytes), p.string() };
    };

    if (! name.empty()) {
        // Direct path?
        if (auto rb = try_load(fs::path(name)); rb.bytes) return rb;
        // Bare filename: search common roots.
        std::vector<fs::path> roots { "/usr/share/fonts", "/usr/local/share/fonts" };
        if (auto* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr) {
            roots.emplace_back(fs::path(xdg) / "fonts");
        }
        if (auto* home = std::getenv("HOME"); home != nullptr) {
            roots.emplace_back(fs::path(home) / ".local/share/fonts");
            roots.emplace_back(fs::path(home) / ".fonts");
        }
        std::string base = fs::path(name).filename().string();
        std::size_t scanned = 0;
        constexpr std::size_t kCap = 8192;
        for (const auto& root : roots) {
            if (! fs::exists(root)) continue;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(
                     root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (++scanned > kCap) break;
                if (! it->is_regular_file(ec)) continue;
                if (it->path().filename() == base) {
                    if (auto rb = try_load(it->path()); rb.bytes) return rb;
                }
            }
            if (scanned > kCap) break;
        }
    }

    if (! fallback_to_any) return { nullptr, {} };

    // Last-resort fallback: any .ttf/.otf in the system roots.
    std::vector<fs::path> roots { "/usr/share/fonts", "/usr/local/share/fonts" };
    std::size_t           scanned = 0;
    constexpr std::size_t kCap    = 4096;
    for (const auto& root : roots) {
        if (! fs::exists(root)) continue;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (++scanned > kCap) break;
            if (! it->is_regular_file(ec)) continue;
            if (IsFontExt(it->path())) {
                if (auto rb = try_load(it->path()); rb.bytes) return rb;
            }
        }
        if (scanned > kCap) break;
    }
    return { nullptr, {} };
}

// -- Atlas snapshot -------------------------------------------------------

std::shared_ptr<wallpaper::Image> BuildAtlasImage(const FontFace& face,
                                                   const std::string& key) {
    auto fm  = face.Metrics();
    auto pix = face.AtlasPixels();
    if (fm.atlas_w == 0 || fm.atlas_h == 0 || pix.empty()) return nullptr;

    auto img = std::make_shared<wallpaper::Image>();
    img->key = key;

    img->header.width    = static_cast<wallpaper::i32>(fm.atlas_w);
    img->header.height   = static_cast<wallpaper::i32>(fm.atlas_h);
    img->header.mapWidth  = img->header.width;
    img->header.mapHeight = img->header.height;
    img->header.mipmap_larger = false;
    img->header.mipmap_pow2   = false;
    img->header.type     = wallpaper::ImageType::UNKNOWN;
    img->header.format   = wallpaper::TextureFormat::R8;
    img->header.count    = 1;
    img->header.isSprite = false;
    img->header.sample   = { wallpaper::TextureWrap::CLAMP_TO_EDGE,
                             wallpaper::TextureWrap::CLAMP_TO_EDGE,
                             wallpaper::TextureFilter::LINEAR,
                             wallpaper::TextureFilter::LINEAR };

    img->slots.resize(1);
    auto& slot = img->slots[0];
    slot.width  = img->header.width;
    slot.height = img->header.height;
    slot.mipmaps.resize(1);
    auto& mip   = slot.mipmaps[0];
    mip.width   = img->header.width;
    mip.height  = img->header.height;
    mip.size    = static_cast<wallpaper::isize>(pix.size());

    auto* buf = new std::uint8_t[pix.size()];
    std::memcpy(buf, pix.data(), pix.size());
    mip.data  = wallpaper::ImageDataPtr(buf, [](std::uint8_t* p) { delete[] p; });

    return img;
}

// -- Text shader ----------------------------------------------------------

namespace
{

constexpr const char* kTextShaderHlsl = R"hlsl(
[[vk::binding(0, 0)]] cbuffer ww_Uniforms {
    column_major float4x4 g_ModelViewProjectionMatrix;
};

struct VSInput {
    float3 a_Position : a_Position;
    float2 a_TexCoord : a_TexCoord;
    float4 a_Color    : a_Color;
};
struct PSInput {
    float4 sv_pos : SV_Position;
    float2 v_uv   : TEXCOORD0;
    float4 v_col  : COLOR0;
};

PSInput main_vs(VSInput i) {
    PSInput o;
    o.sv_pos = mul(g_ModelViewProjectionMatrix, float4(i.a_Position, 1.0));
    o.v_uv   = i.a_TexCoord;
    o.v_col  = i.a_Color;
    return o;
}

[[vk::combinedImageSampler]][[vk::binding(1, 0)]]
Texture2D<float4> g_Texture0;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]]
SamplerState g_Texture0_sampler;

float4 main_ps(PSInput i) : SV_Target {
    float a = g_Texture0.Sample(g_Texture0_sampler, i.v_uv).r;
    return float4(i.v_col.rgb, i.v_col.a * a);
}
)hlsl";

std::shared_ptr<wallpaper::SceneShader> CompileTextShader() {
    using namespace wallpaper::vulkan;

    std::array<ShaderCompUnit, 2> units {
        ShaderCompUnit { wallpaper::ShaderType::VERTEX,   kTextShaderHlsl, "main_vs" },
        ShaderCompUnit { wallpaper::ShaderType::FRAGMENT, kTextShaderHlsl, "main_ps" },
    };
    ShaderCompOpt opt {};
    opt.target   = VulkanTarget::Vulkan_1_1;
    opt.optimize = false;

    std::vector<Uni_ShaderSpv> spvs;
    if (! CompileAndLinkShaderUnits(units, opt, spvs)) {
        LOG_ERROR("text shader compile failed");
        return nullptr;
    }

    auto shader  = std::make_shared<wallpaper::SceneShader>();
    shader->id   = 0;
    shader->name = "text";
    shader->codes.reserve(spvs.size());
    for (auto& spv : spvs) {
        shader->codes.emplace_back(std::move(spv->spirv));
    }
    return shader;
}

} // namespace

std::shared_ptr<wallpaper::SceneShader> GetTextSceneShader() {
    static std::once_flag                            once;
    static std::shared_ptr<wallpaper::SceneShader>   shader;
    std::call_once(once, [] { shader = CompileTextShader(); });
    return shader;
}

} // namespace wallpaper::text
