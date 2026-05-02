module;

#include <memory>
#include <string>

#include "Image.hpp"

export module wescene.parse:wp_tex_image_parser;
import wescene.scene;
import wescene.fs;

export namespace wallpaper

{

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
