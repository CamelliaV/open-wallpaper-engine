module;

export module wescene.fs;
export import wescene.io;
export import wescene.vfs;
import rstd;
import rstd.cppstd;

export namespace owe::fs
{

using BinaryReader = owe::io::BinaryReader;
using BinaryWriter = owe::io::BinaryWriter;

inline Path ToPath(std::string_view path) { return Path(rstd::ref<rstd::str>(path)); }

inline std::string ToStdString(Path path) {
    return std::string(reinterpret_cast<const char*>(path.data()), path.len());
}

inline auto OpenBinary(VFS& vfs, std::string_view path) -> rstd::io::Result<BinaryReader> {
    auto range = vfs.open_read(ToPath(path));
    if (range.is_err()) return rstd::Err(rstd::move(range).unwrap_err_unchecked());
    return rstd::Ok(BinaryReader(rstd::move(range).unwrap_unchecked()));
}

inline auto OpenBinaryWriter(VFS& vfs, std::string_view path, WriteOptions options)
    -> rstd::io::Result<BinaryWriter> {
    auto handle = vfs.open_write(ToPath(path), options);
    if (handle.is_err()) return rstd::Err(rstd::move(handle).unwrap_err_unchecked());
    return rstd::Ok(BinaryWriter(rstd::move(handle).unwrap_unchecked()));
}

inline auto OpenPhysicalBinary(std::string_view path) -> rstd::io::Result<BinaryReader> {
    auto file = rstd::fs::File::open(ToPath(path));
    if (file.is_err()) return rstd::Err(rstd::move(file).unwrap_err_unchecked());
    auto opened   = rstd::move(file).unwrap_unchecked();
    auto metadata = opened.metadata();
    if (metadata.is_err()) return rstd::Err(rstd::move(metadata).unwrap_err_unchecked());
    auto source = rstd::io::SharedReadAt::make(rstd::move(opened));
    auto range  = rstd::io::ReadRange::make(
        rstd::move(source), 0, rstd::move(metadata).unwrap_unchecked().len());
    if (range.is_err()) return rstd::Err(rstd::move(range).unwrap_err_unchecked());
    return rstd::Ok(BinaryReader(rstd::move(range).unwrap_unchecked()));
}

inline auto ReadFileContent(VFS& vfs, std::string_view path) -> rstd::io::Result<std::string> {
    auto reader = OpenBinary(vfs, path);
    if (reader.is_err()) return rstd::Err(rstd::move(reader).unwrap_err_unchecked());
    return reader->read_all_string();
}

} // namespace owe::fs
