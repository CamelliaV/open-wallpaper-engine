module;

export module wescene.pkg_fs;
import wescene.core;
import rstd.cppstd;

export import wescene.fs;

export namespace owe::fs
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

    // Pkg-format version stamp from the binary header (e.g. "PKGV0023").
    // Empty if the pkg was malformed.
    std::string_view pkg_version_stamp() const noexcept { return m_pkg_version; }

private:
    struct PkgFile {
        std::string path;

        idx offset { 0 };
        idx length { 0 };
    };
    std::string                              m_pkgPath;
    std::string                              m_pkg_version;
    std::unordered_map<std::string, PkgFile> m_files;
};

} // namespace owe::fs
