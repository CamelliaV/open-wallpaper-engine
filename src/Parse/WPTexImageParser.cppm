module;

#include "Image.hpp"

export module wescene.parse:wp_tex_image_parser;
import cppstd;
import wescene.scene;
import wescene.fs;

export namespace wallpaper

{

// Sub-version stamps embedded in a `.tex` file. All four are read from
// independent "TEXX0000" stamps interleaved with the header / sprite
// payload (see `LoadHeader` and the sprite branch of `ParseHeader`).
//
// Observed corpus distribution (732 pkgs, 14763 textures across PKGV0001..23):
//   texv: always 5
//   texi: always 1
//   texb: 1 (PKGV0001 only, 4 occurrences) | 2 (early; 168) | 3 (historic dominant; 10052) | 4 (PKGV0022+ dominant; 4537)
//   texs: 0 (= absent / non-sprite) | 2 (early sprites; 15) | 3 (historic+current sprites; 401)
//          texs == 1 is documented in legacy code but never observed.
//
// The predicate methods below collapse texb / texs version drift into a
// single source of truth so the parser body and the sprite branch share
// the same dispatch rules.
struct WPTexFormatVersion {
    std::int32_t texv { 0 };
    std::int32_t texi { 0 };
    std::int32_t texb { 0 };
    std::int32_t texs { 0 };

    // texb >= 2 — body has per-mip { LZ4_compressed, decompressed_size } prelude.
    constexpr bool body_has_lz4_prelude() const noexcept { return texb >= 2; }
    // texb >= 3 — header carries an int32 image_type slot before the mip body
    // (UNKNOWN/-1 for raw pixel data, or a FreeImage-style enum for png/jpg
    // containers). Pre-fix this was gated on `texb == 3`, which silently
    // dropped the slot for texb=4 and misaligned the entire body parse on
    // PKGV0022+ assets.
    constexpr bool body_has_image_type() const noexcept { return texb >= 3; }
    // texb >= 4 — header has an extra reserved int32 (always 0 in the
    // observed corpus) immediately after image_type and before the mip
    // section. Empirically verified across 5126/5129 texb=4 samples.
    constexpr bool body_has_reserved_slot() const noexcept { return texb >= 4; }
    // texs == 1 — sprite frame coordinates are int pixels (legacy; never
    // observed in our corpus). Otherwise floats.
    constexpr bool sprite_frame_coords_int() const noexcept { return texs == 1; }
    // texs >= 3 — sprite section carries an extra trailing { width, height }
    // pair after framecount (atlas dimensions).
    constexpr bool sprite_has_atlas_size() const noexcept { return texs >= 3; }
    constexpr bool valid() const noexcept { return texv != 0 && texi != 0 && texb != 0; }
};

class WPTexImageParser : public IImageParser {
public:
    WPTexImageParser(fs::VFS* vfs): m_vfs(vfs) {}
    virtual ~WPTexImageParser() = default;

    std::shared_ptr<Image> Parse(const std::string&) override;
    ImageHeader            ParseHeader(const std::string&) override;

private:
    fs::VFS* m_vfs;
};
} // namespace wallpaper
