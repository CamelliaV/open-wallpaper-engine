#include "pkg_header.hpp"

import wescene.fs;

namespace wallpaper::testing {

namespace {

std::string ReadSizedString(wallpaper::fs::IBinaryStream& f) {
    std::int32_t len = f.ReadInt32();
    if (len < 0) return {};
    std::string out;
    out.resize(static_cast<std::size_t>(len));
    f.Read(out.data(), static_cast<std::size_t>(len));
    return out;
}

} // namespace

bool ReadPkgHeader(const std::string& pkg_path, std::string& version,
                   std::vector<PkgEntry>& entries) {
    auto stream = wallpaper::fs::CreateCBinaryStream(pkg_path);
    if (! stream) return false;
    version            = ReadSizedString(*stream);
    std::int32_t count = stream->ReadInt32();
    if (count < 0) return false;
    entries.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        PkgEntry e;
        e.path   = "/" + ReadSizedString(*stream);
        e.offset = stream->ReadInt32();
        e.length = stream->ReadInt32();
        entries.push_back(std::move(e));
    }
    return true;
}

} // namespace wallpaper::testing
