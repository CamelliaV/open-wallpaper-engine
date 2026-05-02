module;

#include "Core/Literals.hpp"

#include "Type.hpp"
#include "Image.hpp"
#include <lz4.h>

#include "SpriteAnimation.hpp"
#include "Utils/Logging.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstring>

module wescene.parse;
import cppstd;
import wescene.utils;
import wescene.scene;
import wescene.common;

using namespace wallpaper;

enum class WPTexFlagEnum : uint32_t
{
    // true for no bilinear
    noInterpolation = 0,
    // true for no repeat
    clampUVs = 1,
    sprite   = 2,

    compo1 = 20,
    compo2 = 21,
    compo3 = 22
};
using WPTexFlags = BitFlags<WPTexFlagEnum>;

namespace
{
char* Lz4Decompress(const char* src, int size, int decompressed_size) {
    char* dst       = new char[(usize)decompressed_size];
    int   load_size = LZ4_decompress_safe(src, dst, size, decompressed_size);
    if (load_size < decompressed_size) {
        LOG_ERROR("lz4 decompress failed");
        delete[] dst;
        return nullptr;
    }
    return dst;
}

// Magic-bytes sniffer used as a fallback when the .tex header's
// `image_type` slot says UNKNOWN but the body is actually an embedded
// image container. Some PKGV0022+ assets ship this way (the texture's
// declared image_type is -1 even though the LZ4-decompressed payload
// is a self-contained PNG/JPEG). Without this fallback the body bytes
// are memcpy'd into a "raw RGBA8" slot, which decodes to garbage and
// the wallpaper renders as a flat clear-color screen.
ImageType DetectEmbeddedImageType(const unsigned char* data, usize size) {
    if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return ImageType::PNG;
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return ImageType::JPEG;
    if (size >= 6 && (std::memcmp(data, "GIF87a", 6) == 0 ||
                      std::memcmp(data, "GIF89a", 6) == 0))
        return ImageType::GIF;
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return ImageType::BMP;
    if (size >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2a && data[3] == 0x00) ||
                      (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2a)))
        return ImageType::TIFF;
    return ImageType::UNKNOWN;
}

TextureFormat ToTexFormate(int type) {
    /*
        type
        RGBA8888 = 0,
        DXT5 = 4,
        DXT3 = 6,
        DXT1 = 7,
        RG88 = 8,
        R8 = 9,
    */
    switch (type) {
    case 0: return TextureFormat::RGBA8;
    case 4: return TextureFormat::BC3;
    case 6: return TextureFormat::BC2;
    case 7: return TextureFormat::BC1;
    case 8: return TextureFormat::RG8;
    case 9: return TextureFormat::R8;
    default:
        LOG_ERROR("ERROR::ToTexFormate Unkown image type: %d", type);
        return TextureFormat::RGBA8;
    }
}
// Reads the fixed-layout portion of a .tex header (everything up to and
// including the optional image_type slot). Populates `header.extraHeader`
// with the version stamps + flag bits the renderer consumes downstream,
// and returns the parsed sub-versions so the body / sprite branches can
// dispatch off explicit predicates instead of re-fetching the magic ints.
//
// Version validation is permissive: unsupported (texv,texi,texb) tuples
// log an error but the function still returns a populated struct so the
// caller can decide whether to bail or attempt a best-effort read.
WPTexFormatVersion LoadHeader(fs::IBinaryStream& file, ImageHeader& header) {
    WPTexFormatVersion v;
    v.texv = ReadTexVesion(file);
    v.texi = ReadTexVesion(file);
    header.extraHeader["texv"].val = v.texv;
    header.extraHeader["texi"].val = v.texi;

    header.format = ToTexFormate(file.ReadInt32());
    WPTexFlags flags(file.ReadUint32());
    {
        header.isSprite     = flags[WPTexFlagEnum::sprite];
        header.sample.wrapS = header.sample.wrapT =
            flags[WPTexFlagEnum::clampUVs] ? TextureWrap::CLAMP_TO_EDGE : TextureWrap::REPEAT;
        header.sample.minFilter = header.sample.magFilter =
            flags[WPTexFlagEnum::noInterpolation] ? TextureFilter::NEAREST : TextureFilter::LINEAR;
        header.extraHeader["compo1"].val = flags[WPTexFlagEnum::compo1];
        header.extraHeader["compo2"].val = flags[WPTexFlagEnum::compo2];
        header.extraHeader["compo3"].val = flags[WPTexFlagEnum::compo3];
    }

    /*
        picture:
        width, height --> pow of 2 (tex size)
        mapw, maph    --> pic size
        mips
        mipw,miph     --> pow of 2

        sprites:
        width, height --> piece of sprite sheet
        mapw, maph    --> same
        1 mip
        mipw,mimp     --> tex size
    */

    header.width  = file.ReadInt32();
    header.height = file.ReadInt32();
    // in sprite this mean one pic
    header.mapWidth  = file.ReadInt32();
    header.mapHeight = file.ReadInt32();

    file.ReadInt32(); // unknown

    v.texb = ReadTexVesion(file);
    header.extraHeader["texb"].val = v.texb;

    header.count = file.ReadInt32();

    if (v.body_has_image_type()) header.type = static_cast<ImageType>(file.ReadInt32());
    if (v.body_has_reserved_slot()) file.ReadInt32(); // reserved (always 0 in corpus)

    if (v.texv != 5 || v.texi != 1 || v.texb < 1 || v.texb > 4) {
        LOG_ERROR("WPTexImageParser: unsupported version texv=%d texi=%d texb=%d",
                  v.texv, v.texi, v.texb);
    }
    return v;
}

void SetHeaderPow2(ImageHeader& header, i32 mip_0_w, i32 mip_0_h) {
    header.mipmap_pow2   = algorism::IsPowOfTwo((u32)mip_0_w) || algorism::IsPowOfTwo((u32)mip_0_h);
    header.mipmap_larger = mip_0_w * mip_0_h > header.mapWidth * header.mapHeight;
}

} // namespace

std::shared_ptr<Image> WPTexImageParser::Parse(const std::string& name) {
    std::string            path    = "/assets/materials/" + name + ".tex";
    std::shared_ptr<Image> img_ptr = std::make_shared<Image>();
    auto&                  img     = *img_ptr;
    img.key                        = name;
    // std::ifstream file = fs::GetFileFstream(vfs, path);
    auto pfile = m_vfs->Open(path);
    if (! pfile) return nullptr;
    auto& file     = *pfile;
    auto  startpos = file.Tell();
    auto  ver      = LoadHeader(file, img.header);

    // image
    i32 _image_count = img.header.count;
    if (_image_count < 0) return nullptr;
    usize image_count = (usize)_image_count;

    img.slots.resize(image_count);
    for (usize i_image = 0; i_image < image_count; i_image++) {
        auto& img_slot = img.slots[i_image];
        auto& mipmaps  = img_slot.mipmaps;

        usize mipmap_count = (usize)std::max<i32>(file.ReadInt32(), 0);
        mipmaps.resize(mipmap_count);
        // load image
        for (usize i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
            auto& mipmap  = mipmaps.at(i_mipmap);
            mipmap.width  = file.ReadInt32();
            mipmap.height = file.ReadInt32();
            if (i_mipmap == 0) {
                img_slot.width  = mipmap.width;
                img_slot.height = mipmap.height;
                SetHeaderPow2(img.header, mipmap.width, mipmap.height);
            }

            bool    LZ4_compressed    = false;
            int32_t decompressed_size = 0;
            // check compress
            if (ver.body_has_lz4_prelude()) {
                LZ4_compressed    = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }

            i32 src_size = file.ReadInt32();
            if (src_size <= 0 || mipmap.width <= 0 || mipmap.height <= 0 || decompressed_size < 0)
                return nullptr;

            char* result;
            result = new char[(usize)src_size];
            file.Read(result, (usize)src_size);

            // is LZ4 compress
            if (LZ4_compressed) {
                char* decompressed_char = Lz4Decompress(result, src_size, decompressed_size);
                src_size                = decompressed_size;
                if (decompressed_char != nullptr) {
                    delete[] result;
                    result = decompressed_char;
                } else {
                    LOG_ERROR("lz4 decompress failed");
                    delete[] result;
                    return nullptr;
                }
            }
            // is image container — declared image_type takes precedence; if
            // it's UNKNOWN, sniff the magic bytes so PKGV0022+ assets that
            // ship containerised PNG/JPEG with image_type=-1 still decode.
            ImageType embedded = img.header.type;
            if (ver.body_has_image_type() && embedded == ImageType::UNKNOWN) {
                embedded = DetectEmbeddedImageType((const unsigned char*)result,
                                                   (usize)src_size);
            }
            if (ver.body_has_image_type() && embedded != ImageType::UNKNOWN) {
                int32_t w, h, n;
                auto*   data =
                    stbi_load_from_memory((const unsigned char*)result, src_size, &w, &h, &n, 4);
                if (data == nullptr) {
                    LOG_ERROR("stbi failed to decode embedded image (type=%d)", (int)embedded);
                    delete[] result;
                    return nullptr;
                }
                img.header.type   = embedded;
                img.header.format = TextureFormat::RGBA8;
                mipmap.data = ImageDataPtr((uint8_t*)data, [](uint8_t* data) {
                    stbi_image_free((unsigned char*)data);
                });
                src_size    = w * h * 4;
            } else {
                mipmap.data = ImageDataPtr(new uint8_t[(usize)src_size], [](uint8_t* data) {
                    delete[] data;
                });
                std::copy(result, result + src_size, mipmap.data.get());
            }
            mipmap.size = src_size * (i32)sizeof(uint8_t);
            delete[] result;
        }
    }
    return img_ptr;
}

ImageHeader WPTexImageParser::ParseHeader(const std::string& name) {
    ImageHeader header;
    std::string path  = "/assets/materials/" + name + ".tex";
    auto        pfile = m_vfs->Open(path);
    if (! pfile) return header;
    auto& file = *pfile;

    auto ver = LoadHeader(file, header);
    if (header.count < 0) return header;

    usize image_count = (usize)header.count;

    // load sprite info
    if (header.isSprite) {
        // bypass image data, store width and height
        std::vector<std::vector<float>> imageDatas(image_count);
        for (usize i_image = 0; i_image < image_count; i_image++) {
            int mipmap_count = file.ReadInt32();
            for (int32_t i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
                int32_t width  = file.ReadInt32();
                int32_t height = file.ReadInt32();
                if (i_mipmap == 0) {
                    imageDatas.at(i_image) = { (float)width, (float)height };
                    header.mipmap_pow2     = algorism::IsPowOfTwo((u32)(width * height));
                }
                if (ver.body_has_lz4_prelude()) {
                    int32_t LZ4_compressed    = file.ReadInt32();
                    int32_t decompressed_size = file.ReadInt32();
                    (void)LZ4_compressed;
                    (void)decompressed_size;
                }
                long src_size = file.ReadInt32();
                file.SeekCur(src_size);
            }
        }
        // sprite pos
        ver.texs                       = ReadTexVesion(file);
        header.extraHeader["texs"].val = ver.texs;
        int32_t framecount             = file.ReadInt32();
        if (ver.texs < 1 || ver.texs > 3) {
            LOG_ERROR("WPTexImageParser: unsupported texs version %d", ver.texs);
        }
        if (ver.sprite_has_atlas_size()) {
            i32 width  = file.ReadInt32();
            i32 height = file.ReadInt32();
            (void)width;
            (void)height;
        }

        for (int32_t i = 0; i < framecount; i++) {
            SpriteFrame sf;
            sf.imageId = file.ReadInt32();
            if (sf.imageId < 0) {
                LOG_ERROR("get neg imageid");
            }
            float spriteWidth  = imageDatas.at((usize)sf.imageId)[0];
            float spriteHeight = imageDatas.at((usize)sf.imageId)[1];

            sf.frametime = file.ReadFloat();
            if (ver.sprite_frame_coords_int()) {
                sf.x        = (float)file.ReadInt32() / spriteWidth;
                sf.y        = (float)file.ReadInt32() / spriteHeight;
                sf.xAxis[0] = (float)file.ReadInt32();
                sf.xAxis[1] = (float)file.ReadInt32();
                sf.yAxis[0] = (float)file.ReadInt32();
                sf.yAxis[1] = (float)file.ReadInt32();
            } else {
                sf.x        = file.ReadFloat() / spriteWidth;
                sf.y        = file.ReadFloat() / spriteHeight;
                sf.xAxis[0] = file.ReadFloat();
                sf.xAxis[1] = file.ReadFloat();
                sf.yAxis[0] = file.ReadFloat();
                sf.yAxis[1] = file.ReadFloat();
            }
            sf.width  = (float)std::sqrt(std::pow(sf.xAxis[0], 2) + std::pow(sf.xAxis[1], 2));
            sf.height = (float)std::sqrt(std::pow(sf.yAxis[0], 2) + std::pow(sf.yAxis[1], 2));
            sf.xAxis[0] /= spriteWidth;
            sf.xAxis[1] /= spriteWidth;
            sf.yAxis[0] /= spriteHeight;
            sf.yAxis[1] /= spriteHeight;
            sf.rate = sf.height / sf.width;
            header.spriteAnim.AppendFrame(sf);
        }
    } else {
        i32 mipmap_count = file.ReadInt32();
        (void)mipmap_count;
        i32 width  = file.ReadInt32();
        i32 height = file.ReadInt32();
        SetHeaderPow2(header, width, height);
    }
    return header;
}
