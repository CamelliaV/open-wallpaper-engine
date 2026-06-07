module;

#include <rstd/macro.hpp>

module wescene.pkg_fs;
import wescene.core;
import rstd.log;
import rstd.cppstd;

import wescene.fs;

using namespace owe;
using namespace owe::fs;

namespace
{
std::string ReadSizedString(IBinaryStream& f) {
    idx ilen = f.ReadInt32();
    rstd_assert(ilen >= 0);

    usize       len = (usize)ilen;
    std::string result;
    result.resize(len);
    f.Read(result.data(), len);
    return result;
}

// WE pkgs were authored on Windows where NTFS is case-insensitive; some
// shaders reference `effects/foo` while the pkg stores `Effects/foo`. Lower
// every path going through the map so lookups match regardless of case.
std::string LowerPath(std::string_view p) {
    std::string s(p);
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}
} // namespace

std::unique_ptr<WPPkgFs> WPPkgFs::CreatePkgFs(std::string_view pkgpath) {
    auto ppkg = fs::CreateCBinaryStream(pkgpath);
    if (! ppkg) return nullptr;

    auto&       pkg = *ppkg;
    std::string ver = ReadSizedString(pkg);
    rstd_info("pkg version: {}", ver);

    std::vector<PkgFile> pkgfiles;
    i32                  entryCount = pkg.ReadInt32();
    for (i32 i = 0; i < entryCount; i++) {
        std::string path   = "/" + ReadSizedString(pkg);
        idx         offset = pkg.ReadInt32();
        idx         length = pkg.ReadInt32();
        pkgfiles.push_back({ path, offset, length });
    }
    auto pkgfs           = std::unique_ptr<WPPkgFs>(new WPPkgFs());
    pkgfs->m_pkgPath     = pkgpath;
    pkgfs->m_pkg_version = std::move(ver);
    idx headerSize       = pkg.Tell();
    for (auto& el : pkgfiles) {
        el.offset += headerSize;
        pkgfs->m_files.insert({ LowerPath(el.path), el });
    }
    return pkgfs;
}

bool WPPkgFs::Contains(std::string_view path) const { return m_files.count(LowerPath(path)) > 0; }

std::shared_ptr<IBinaryStream> WPPkgFs::Open(std::string_view path) {
    auto pkg = fs::CreateCBinaryStream(m_pkgPath);
    if (! pkg) return nullptr;
    auto it = m_files.find(LowerPath(path));
    if (it != m_files.end()) {
        return std::make_shared<LimitedBinaryStream>(pkg, it->second.offset, it->second.length);
    }
    return nullptr;
}

std::shared_ptr<IBinaryStreamW> WPPkgFs::OpenW(std::string_view) { return nullptr; }
