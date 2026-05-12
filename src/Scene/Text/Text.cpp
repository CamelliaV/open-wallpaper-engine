module;

#include <rstd/macro.hpp>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>

#include <ft2build.h>
#include FT_FREETYPE_H
module wescene.text;
import wescene.spec_texs;
import wescene.core;
import wescene.types;
import rstd.log;
import rstd.cppstd;
import wescene.scene;
import wescene.shader_compile;

namespace owe::text
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
            rstd_error("FT_Init_FreeType failed");
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
        rstd_error("FT_New_Memory_Face failed");
        return nullptr;
    }
    if (FT_Set_Pixel_Sizes(face->m_impl->face, 0, pixel_size) != 0) {
        rstd_error("FT_Set_Pixel_Sizes failed (px={})", pixel_size);
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

std::shared_ptr<owe::Image> BuildAtlasImage(const FontFace& face,
                                                   const std::string& key) {
    auto fm  = face.Metrics();
    auto pix = face.AtlasPixels();
    if (fm.atlas_w == 0 || fm.atlas_h == 0 || pix.empty()) return nullptr;

    auto img = std::make_shared<owe::Image>();
    img->key = key;

    img->header.width    = static_cast<owe::i32>(fm.atlas_w);
    img->header.height   = static_cast<owe::i32>(fm.atlas_h);
    img->header.mapWidth  = img->header.width;
    img->header.mapHeight = img->header.height;
    img->header.mipmap_larger = false;
    img->header.mipmap_pow2   = false;
    img->header.type     = owe::ImageType::UNKNOWN;
    img->header.format   = owe::TextureFormat::R8;
    img->header.count    = 1;
    img->header.isSprite = false;
    img->header.sample   = { owe::TextureWrap::CLAMP_TO_EDGE,
                             owe::TextureWrap::CLAMP_TO_EDGE,
                             owe::TextureFilter::LINEAR,
                             owe::TextureFilter::LINEAR };

    img->slots.resize(1);
    auto& slot = img->slots[0];
    slot.width  = img->header.width;
    slot.height = img->header.height;
    slot.mipmaps.resize(1);
    auto& mip   = slot.mipmaps[0];
    mip.width   = img->header.width;
    mip.height  = img->header.height;
    mip.size    = static_cast<owe::isize>(pix.size());

    auto* buf = new std::uint8_t[pix.size()];
    std::memcpy(buf, pix.data(), pix.size());
    mip.data  = owe::ImageDataPtr(buf, [](std::uint8_t* p) { delete[] p; });

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

std::shared_ptr<owe::SceneShader> CompileTextShader() {
    using namespace owe::vulkan;

    std::array<ShaderCompUnit, 2> units {
        ShaderCompUnit { owe::ShaderType::VERTEX,   kTextShaderHlsl, "main_vs" },
        ShaderCompUnit { owe::ShaderType::FRAGMENT, kTextShaderHlsl, "main_ps" },
    };
    ShaderCompOpt opt {};
    opt.target   = VulkanTarget::Vulkan_1_1;
    opt.optimize = false;

    std::vector<Uni_ShaderSpv> spvs;
    if (! CompileAndLinkShaderUnits(units, opt, spvs)) {
        rstd_error("text shader compile failed");
        return nullptr;
    }

    auto shader  = std::make_shared<owe::SceneShader>();
    shader->id   = 0;
    shader->name = "text";
    shader->codes.reserve(spvs.size());
    for (auto& spv : spvs) {
        shader->codes.emplace_back(std::move(spv->spirv));
    }
    return shader;
}

} // namespace

std::shared_ptr<owe::SceneShader> GetTextSceneShader() {
    static std::once_flag                            once;
    static std::shared_ptr<owe::SceneShader>   shader;
    std::call_once(once, [] { shader = CompileTextShader(); });
    return shader;
}

// -- TextLayouter ---------------------------------------------------------

namespace
{

struct TextLineRunGI {
    std::vector<const GlyphInfo*> glyphs;
    float                         width { 0.0f };
};

bool ContainsSubstring(std::string_view s, std::string_view what) noexcept {
    return s.find(what) != std::string::npos;
}

} // namespace

struct TextLayouter::Impl {
    std::unique_ptr<FontCache>            cache;
    FontFace*                             face { nullptr };
    std::shared_ptr<owe::SceneMesh> mesh;
    TextLayoutStyle                       style;
    std::size_t                           peak_quads { 0 };
    FontMetrics                           metrics;

    float        last_text_w { 0.0f };
    float        last_text_h { 0.0f };
    bool         missing_glyph_logged { false };

    // Scratch buffers reused across SetText calls — avoids reallocs for
    // every script tick. Sized at construction to peak capacity.
    std::vector<float>         positions;
    std::vector<float>         texcoords;
    std::vector<float>         colors;
    std::vector<std::uint32_t> indices;

    Impl(std::unique_ptr<FontCache> c, FontFace* f,
         std::shared_ptr<owe::SceneMesh> m, TextLayoutStyle s, std::size_t pq)
        : cache(std::move(c)), face(f), mesh(std::move(m)), style(std::move(s)),
          peak_quads(pq), metrics(face->Metrics()) {
        positions.assign(pq * 4 * 3, 0.0f);
        texcoords.assign(pq * 4 * 2, 0.0f);
        colors   .assign(pq * 4 * 4, 0.0f);
        indices  .assign(pq * 6,     0u);
    }
};

TextLayouter::TextLayouter(std::unique_ptr<FontCache>            cache,
                           FontFace*                             face,
                           std::shared_ptr<owe::SceneMesh> mesh,
                           TextLayoutStyle                       style,
                           std::size_t                           peak_quads)
    : m_impl(std::make_unique<Impl>(std::move(cache), face, std::move(mesh),
                                    std::move(style), peak_quads)) {}

TextLayouter::~TextLayouter() = default;

float TextLayouter::TextWidth() const noexcept  { return m_impl->last_text_w; }
float TextLayouter::TextHeight() const noexcept { return m_impl->last_text_h; }

void TextLayouter::SetText(std::string_view utf8) {
    auto& im = *m_impl;

    auto codepoints = DecodeUtf8(utf8);

    // Phase 1: split into lines and look up pre-rasterised glyph metrics.
    std::vector<TextLineRunGI> lines;
    lines.emplace_back();
    std::size_t total_glyph_quads = 0;
    for (std::uint32_t cp : codepoints) {
        if (cp == '\n') {
            lines.emplace_back();
            continue;
        }
        const auto* gi = im.face->Glyph(cp);
        if (gi == nullptr) {
            if (! im.missing_glyph_logged) {
                rstd_info("text: codepoint U+{:04X} not in pre-rasterised set, skipping",
                         cp);
                im.missing_glyph_logged = true;
            }
            continue;
        }
        lines.back().glyphs.push_back(gi);
        lines.back().width += gi->advance_x;
        ++total_glyph_quads;
    }

    bool has_bg = im.style.opaquebackground;
    std::size_t total_quads = total_glyph_quads + (has_bg ? 1u : 0u);
    if (total_quads > im.peak_quads) {
        rstd_info("text: {} quads exceed peak capacity {}, truncating",
                 total_quads, im.peak_quads);
        total_quads = im.peak_quads;
        if (has_bg && total_glyph_quads + 1 > im.peak_quads)
            total_glyph_quads = im.peak_quads - 1;
        else if (! has_bg)
            total_glyph_quads = total_quads;
    }

    auto&  fm    = im.metrics;
    float  text_w = 0.0f;
    for (auto& l : lines)
        if (l.width > text_w) text_w = l.width;
    float text_h = fm.ascender - fm.descender +
                   static_cast<float>(lines.size() - 1) * fm.line_height;
    im.last_text_w = text_w;
    im.last_text_h = text_h;

    // Zero the unused tail so stale data from the previous (longer) text
    // doesn't show up. Cheaper than tracking exact quad count downstream.
    std::fill(im.positions.begin(), im.positions.end(), 0.0f);
    std::fill(im.texcoords.begin(), im.texcoords.end(), 0.0f);
    std::fill(im.colors.begin(),    im.colors.end(),    0.0f);
    std::fill(im.indices.begin(),   im.indices.end(),   0u);

    auto write_quad = [&](std::size_t q_idx, float left, float right, float bottom,
                          float top, float u_l, float u_r, float v_t, float v_b,
                          const std::array<float, 4>& rgba) {
        std::size_t v_off = q_idx * 4;
        const float pos[4][3] = {
            { left,  top,    0.0f },
            { right, top,    0.0f },
            { right, bottom, 0.0f },
            { left,  bottom, 0.0f },
        };
        const float uv[4][2] = {
            { u_l, v_t },
            { u_r, v_t },
            { u_r, v_b },
            { u_l, v_b },
        };
        for (std::size_t k = 0; k < 4; ++k) {
            std::memcpy(&im.positions[(v_off + k) * 3], pos[k], sizeof(pos[k]));
            std::memcpy(&im.texcoords[(v_off + k) * 2], uv[k], sizeof(uv[k]));
            std::memcpy(&im.colors   [(v_off + k) * 4], rgba.data(), sizeof(float) * 4);
        }
        std::size_t i_off = q_idx * 6;
        const std::uint32_t base = static_cast<std::uint32_t>(v_off);
        im.indices[i_off + 0] = base + 0;
        im.indices[i_off + 1] = base + 1;
        im.indices[i_off + 2] = base + 2;
        im.indices[i_off + 3] = base + 0;
        im.indices[i_off + 4] = base + 2;
        im.indices[i_off + 5] = base + 3;
    };

    float text_top    = +text_h * 0.5f;
    float text_bottom = -text_h * 0.5f;
    float text_left   = -text_w * 0.5f;
    float text_right  = +text_w * 0.5f;
    (void)text_left;
    (void)text_right;
    (void)text_bottom;
    float pad         = im.style.padding;

    std::size_t q = 0;

    if (has_bg) {
        float u_l = 1.0f / static_cast<float>(fm.atlas_w);
        float u_r = 3.0f / static_cast<float>(fm.atlas_w);
        float v_t = 1.0f / static_cast<float>(fm.atlas_h);
        float v_b = 3.0f / static_cast<float>(fm.atlas_h);
        std::array<float, 4> rgba {
            im.style.background_color[0] * im.style.background_brightness,
            im.style.background_color[1] * im.style.background_brightness,
            im.style.background_color[2] * im.style.background_brightness,
            1.0f,
        };
        write_quad(q++,
                   -text_w * 0.5f - pad, +text_w * 0.5f + pad,
                   -text_h * 0.5f - pad, +text_h * 0.5f + pad,
                   u_l, u_r, v_t, v_b, rgba);
    }

    std::array<float, 4> text_rgba {
        im.style.color[0] * im.style.brightness,
        im.style.color[1] * im.style.brightness,
        im.style.color[2] * im.style.brightness,
        im.style.alpha,
    };

    std::size_t emitted_glyphs = 0;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li];
        float       line_origin_x;
        if (ContainsSubstring(im.style.halign, "left")) {
            line_origin_x = -text_w * 0.5f;
        } else if (ContainsSubstring(im.style.halign, "right")) {
            line_origin_x = +text_w * 0.5f - line.width;
        } else {
            line_origin_x = -line.width * 0.5f;
        }
        float baseline_y =
            text_top - fm.ascender - static_cast<float>(li) * fm.line_height;

        float pen_x = line_origin_x;
        for (auto* gi : line.glyphs) {
            if (q >= im.peak_quads) break;
            if (gi->pixel_w == 0 || gi->pixel_h == 0) {
                pen_x += gi->advance_x;
                ++emitted_glyphs;
                continue;
            }
            float left   = pen_x + gi->bearing_x;
            float right  = left + static_cast<float>(gi->pixel_w);
            float top    = baseline_y + gi->bearing_y;
            float bottom = top - static_cast<float>(gi->pixel_h);
            float u_l    = static_cast<float>(gi->atlas_x) / static_cast<float>(fm.atlas_w);
            float u_r    = static_cast<float>(gi->atlas_x + gi->pixel_w) /
                        static_cast<float>(fm.atlas_w);
            float v_t = static_cast<float>(gi->atlas_y) / static_cast<float>(fm.atlas_h);
            float v_b = static_cast<float>(gi->atlas_y + gi->pixel_h) /
                        static_cast<float>(fm.atlas_h);
            write_quad(q++, left, right, bottom, top, u_l, u_r, v_t, v_b, text_rgba);
            pen_x += gi->advance_x;
            ++emitted_glyphs;
            if (emitted_glyphs >= total_glyph_quads) break;
        }
        if (emitted_glyphs >= total_glyph_quads) break;
    }

    // Push into the mesh. Vertex array's stride is interleaved with padding
    // already laid out by SceneVertexArray; SetVertex scatters by name.
    auto& v = im.mesh->GetVertexArray(0);
    v.SetVertex(WE_IN_POSITION, im.positions);
    v.SetVertex(WE_IN_TEXCOORD, im.texcoords);
    v.SetVertex(WE_IN_COLOR,    im.colors);

    auto& idx = im.mesh->GetIndexArray(0);
    idx.Assign(0, im.indices);
    // Render only the indices we actually populated (rest are zeroed out
    // and reference vertex 0, which is harmless but wastes draw calls).
    idx.SetRenderDataCount(q * 6);

    im.mesh->SetDirty();
}

} // namespace owe::text
