module;

#include <rstd/macro.hpp>

module wescene.pkg_fs;
import wescene.core;
import rstd;
import rstd.log;
import rstd.cppstd;

import wescene.fs;

using ::alloc::collections::HashMap;
using ::alloc::string::String;
using namespace owe;
using namespace owe::fs;
using namespace rstd::prelude;

namespace
{

auto FsError(rstd::io::error::ErrorKind::Entity kind) -> rstd::io::error::Error {
    return rstd::io::error::Error::from_kind(rstd::io::error::ErrorKind { kind });
}

std::optional<std::string> ReadSizedString(BinaryReader& file, usize max_len) {
    auto signed_len = file.ReadInt32();
    if (signed_len < 0) return std::nullopt;

    auto len = usize(signed_len);
    if (len > max_len) return std::nullopt;
    std::string result(len, '\0');
    if (file.Read(result.data(), len) != len) return std::nullopt;
    return result;
}

bool IsPkgVersionStamp(std::string_view stamp) {
    constexpr std::string_view prefix = "PKGV";
    return stamp.size() > prefix.size() && stamp.substr(0, prefix.size()) == prefix;
}

auto LookupKey(Path path) -> rstd::io::Result<String> {
    auto output     = String::make("/");
    auto components = path.components();
    while (true) {
        auto component = components.next();
        if (component.is_none()) break;
        if (component->is_parent_dir()) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidInput));
        }
        if (! component->is_normal()) continue;
        auto value = component->as_os_str().to_str();
        if (value.is_none()) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidFilename));
        }
        if (output.len() > 1) output.push_back('/');
        for (auto byte : *value) {
            auto lowered = byte >= 'A' && byte <= 'Z' ? u8(byte - 'A' + 'a') : byte;
            output.push_back(lowered);
        }
    }
    return rstd::Ok(rstd::move(output));
}

} // namespace

auto WPPkgFs::CreatePkgFs(std::string_view pkgpath) -> rstd::io::Result<PkgMount> {
    auto file = rstd::fs::File::open(ToPath(pkgpath));
    if (file.is_err()) return rstd::Err(rstd::move(file).unwrap_err_unchecked());
    auto opened   = rstd::move(file).unwrap_unchecked();
    auto metadata = opened.metadata();
    if (metadata.is_err()) return rstd::Err(rstd::move(metadata).unwrap_err_unchecked());
    auto source = rstd::io::SharedReadAt::make(rstd::move(opened));
    auto range =
        ReadRange::make(rstd::move(source), 0, rstd::move(metadata).unwrap_unchecked().len());
    if (range.is_err()) return rstd::Err(rstd::move(range).unwrap_err_unchecked());
    auto pkg_source = rstd::move(range).unwrap_unchecked();
    auto pkg        = BinaryReader(pkg_source.clone());

    auto version = ReadSizedString(pkg, 64);
    if (! version || ! IsPkgVersionStamp(*version)) {
        return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
    }
    rstd_info("pkg version: {}", *version);

    struct PendingFile {
        String path;
        u64    offset;
        u64    length;
    };
    auto files = ::alloc::vec::Vec<PendingFile>::make();

    auto entry_count = pkg.ReadInt32();
    if (entry_count < 0) {
        return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
    }
    files.reserve(usize(entry_count));
    for (i32 i = 0; i < entry_count; ++i) {
        auto path = ReadSizedString(pkg, 4096);
        if (! path) return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        auto key = LookupKey(ToPath(*path));
        if (key.is_err()) return rstd::Err(rstd::move(key).unwrap_err_unchecked());

        auto offset = pkg.ReadInt32();
        auto length = pkg.ReadInt32();
        if (offset < 0 || length < 0) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        }
        files.push(PendingFile { .path   = rstd::move(key).unwrap_unchecked(),
                                 .offset = u64(offset),
                                 .length = u64(length) });
    }

    auto header_size = pkg.position();
    auto entries     = HashMap<String, PkgFile>::with_capacity(files.len());
    for (auto& file : files) {
        if (file.offset > u64(-1) - header_size) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        }
        auto absolute = header_size + file.offset;
        if (absolute > pkg_source.len() || file.length > pkg_source.len() - absolute) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        }
        entries.insert(rstd::move(file.path),
                       PkgFile { .offset = absolute, .length = file.length });
    }

    auto version_string = String::make(rstd::ref<rstd::str>(*version));
    auto mount_version  = version_string.clone();
    auto mount          = MountHandle::make(
        WPPkgFs(rstd::move(pkg_source), rstd::move(version_string), rstd::move(entries)));
    return rstd::Ok(PkgMount(rstd::move(mount), rstd::move(mount_version)));
}

auto WPPkgFs::open_read(Path path) const -> rstd::io::Result<ReadRange> {
    auto key = LookupKey(path);
    if (key.is_err()) return rstd::Err(rstd::move(key).unwrap_err_unchecked());
    auto file = m_files.get(*key);
    if (file.is_none()) return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
    return m_source.subrange((*file)->offset, (*file)->length);
}

auto WPPkgFs::open_write(Path path, WriteOptions) const -> rstd::io::Result<WriteSeekHandle> {
    auto key = LookupKey(path);
    if (key.is_err()) return rstd::Err(rstd::move(key).unwrap_err_unchecked());
    if (! m_files.contains_key(*key)) {
        return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
    }
    return rstd::Err(FsError(rstd::io::error::ErrorKind::ReadOnlyFilesystem));
}

auto WPPkgFs::metadata(Path path) const -> rstd::io::Result<FileMetadata> {
    auto key = LookupKey(path);
    if (key.is_err()) return rstd::Err(rstd::move(key).unwrap_err_unchecked());
    auto file = m_files.get(*key);
    if (file.is_none()) return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
    return rstd::Ok(FileMetadata {
        .len = (*file)->length, .is_file = true, .is_directory = false, .readonly = true });
}
