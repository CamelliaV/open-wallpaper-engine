module;

#include "Core/Literals.hpp"

export module wescene.pkg_fs;
import cppstd;

export import wescene.fs;

export namespace wallpaper::fs
{

class WPPkgFs : public Fs {
public:
    virtual ~WPPkgFs() = default;
    static std::unique_ptr<WPPkgFs> CreatePkgFs(std::string_view pkgpath);

private:
    WPPkgFs() = default;

public:
    bool                            Contains(std::string_view path) const override;
    std::shared_ptr<IBinaryStream>  Open(std::string_view path) override;
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view path) override;

private:
    struct PkgFile {
        std::string path;

        idx offset { 0 };
        idx length { 0 };
    };
    std::string                              m_pkgPath;
    std::unordered_map<std::string, PkgFile> m_files;
};

} // namespace wallpaper::fs
