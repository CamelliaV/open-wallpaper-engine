#include <gtest/gtest.h>

import rstd;
import rstd.cppstd;
import wescene.fs;
import wescene.pkg_fs;

using namespace rstd::prelude;

namespace
{

auto FsError(rstd::io::error::ErrorKind::Entity kind) -> rstd::io::error::Error {
    return rstd::io::error::Error::from_kind(rstd::io::error::ErrorKind { kind });
}

struct MemorySource {
    std::string data;

    auto read_at(u8* buffer, usize len, u64 offset) const -> rstd::io::Result<usize> {
        if (offset >= data.size()) return rstd::Ok(usize(0));
        auto count = rstd::min(len, data.size() - usize(offset));
        rstd::mem::memcpy(buffer, data.data() + offset, count);
        return rstd::Ok(count);
    }
};

} // namespace

template<>
struct rstd::Impl<rstd::io::ReadAt, MemorySource> : rstd::ImplBase<MemorySource> {
    auto read_at(u8* buffer, usize len, u64 offset) const -> rstd::io::Result<usize> {
        return this->self().read_at(buffer, len, offset);
    }
};

namespace
{

class TestMount {
public:
    explicit TestMount(std::unordered_map<std::string, std::string> files,
                       std::string                                  invalid_path = {})
        : m_files(std::move(files)), m_invalid_path(std::move(invalid_path)) {}

    auto open_read(owe::fs::Path path) const -> rstd::io::Result<owe::fs::ReadRange> {
        auto key = owe::fs::ToStdString(path);
        if (key == m_invalid_path) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        }
        auto file = m_files.find(key);
        if (file == m_files.end()) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
        }
        auto source = rstd::io::SharedReadAt::make(MemorySource { file->second });
        return owe::fs::ReadRange::make(std::move(source), 0, file->second.size());
    }

    auto open_write(owe::fs::Path path, owe::fs::WriteOptions) const
        -> rstd::io::Result<owe::fs::WriteSeekHandle> {
        auto key = owe::fs::ToStdString(path);
        if (m_files.contains(key)) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::ReadOnlyFilesystem));
        }
        return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
    }

    auto metadata(owe::fs::Path path) const -> rstd::io::Result<owe::fs::FileMetadata> {
        auto key  = owe::fs::ToStdString(path);
        auto file = m_files.find(key);
        if (file == m_files.end()) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
        }
        return rstd::Ok(owe::fs::FileMetadata {
            .len = file->second.size(), .is_file = true, .is_directory = false, .readonly = true });
    }

private:
    std::unordered_map<std::string, std::string> m_files;
    std::string                                  m_invalid_path;
};

auto MakeMount(std::unordered_map<std::string, std::string> files, std::string invalid_path = {})
    -> owe::fs::MountHandle;

auto ReadText(owe::fs::ReadRange range) -> std::string {
    owe::fs::BinaryReader reader(std::move(range));
    return reader.ReadAllStr();
}

void WriteI32(std::ofstream& output, std::int32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WriteSized(std::ofstream& output, std::string_view value) {
    WriteI32(output, static_cast<std::int32_t>(value.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void WritePkg(const std::filesystem::path& path, std::int32_t body_length) {
    std::ofstream output(path, std::ios::binary);
    WriteSized(output, "PKGV0001");
    WriteI32(output, 1);
    WriteSized(output, "Materials/Foo.bin");
    WriteI32(output, 0);
    WriteI32(output, body_length);
    output.write("hello", 5);
}

class TempDirectory {
public:
    TempDirectory() {
        auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("owe-vfs-test-" + std::to_string(suffix));
        std::filesystem::create_directories(path);
    }

    ~TempDirectory() { std::filesystem::remove_all(path); }

    std::filesystem::path path;
};

} // namespace

template<>
struct rstd::Impl<owe::fs::MountFs, TestMount> : rstd::ImplBase<TestMount> {
    auto open_read(owe::fs::Path path) const -> rstd::io::Result<owe::fs::ReadRange> {
        return this->self().open_read(path);
    }

    auto open_write(owe::fs::Path path, owe::fs::WriteOptions options) const
        -> rstd::io::Result<owe::fs::WriteSeekHandle> {
        return this->self().open_write(path, options);
    }

    auto metadata(owe::fs::Path path) const -> rstd::io::Result<owe::fs::FileMetadata> {
        return this->self().metadata(path);
    }
};

namespace
{

auto MakeMount(std::unordered_map<std::string, std::string> files, std::string invalid_path)
    -> owe::fs::MountHandle {
    return owe::fs::MountHandle::make(TestMount(std::move(files), std::move(invalid_path)));
}

TEST(Vfs, OverlayAndUnmountKeepOpenedRangeAlive) {
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets", MakeMount({ { "shared", "lower" } })).is_ok());
    auto upper = vfs.mount("/assets", MakeMount({ { "shared", "upper" } }));
    ASSERT_TRUE(upper.is_ok());

    auto opened = vfs.open_read("/assets/shared");
    ASSERT_TRUE(opened.is_ok());
    auto retained = std::move(opened).unwrap_unchecked();

    EXPECT_TRUE(vfs.unmount(*upper));
    auto visible = vfs.open_read("/assets/shared");
    ASSERT_TRUE(visible.is_ok());
    EXPECT_EQ(ReadText(std::move(visible).unwrap_unchecked()), "lower");
    EXPECT_EQ(ReadText(std::move(retained)), "upper");
}

TEST(Vfs, BackendErrorsAreNotOverlayMisses) {
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets", MakeMount({ { "broken", "lower" } })).is_ok());
    ASSERT_TRUE(vfs.mount("/assets", MakeMount({}, "broken")).is_ok());

    auto opened = vfs.open_read("/assets/broken");
    ASSERT_TRUE(opened.is_err());
    EXPECT_EQ(std::move(opened).unwrap_err_unchecked().kind().code,
              rstd::io::error::ErrorKind::InvalidData);
}

TEST(Vfs, PathsUseComponentBoundariesAndRejectTraversal) {
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/asset", MakeMount({ { "file", "value" } })).is_ok());

    EXPECT_TRUE(vfs.open_read("/assets/file").is_err());
    auto invalid = vfs.open_read("/asset/../file");
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(std::move(invalid).unwrap_err_unchecked().kind().code,
              rstd::io::error::ErrorKind::InvalidInput);
    EXPECT_TRUE(vfs.mount("asset", MakeMount({})).is_err());
}

TEST(Vfs, WriteRoutingPreservesReadonlyOverlay) {
    TempDirectory temp;
    {
        std::ofstream file(temp.path / "locked");
        file << "physical";
    }

    auto physical = owe::fs::make_physical_fs(owe::fs::ToPath(temp.path.string()));
    ASSERT_TRUE(physical.is_ok());

    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets", std::move(physical).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets", MakeMount({ { "locked", "readonly" } })).is_ok());

    auto locked = vfs.open_write("/assets/locked", owe::fs::WriteOptions { .truncate = true });
    ASSERT_TRUE(locked.is_err());
    EXPECT_EQ(std::move(locked).unwrap_err_unchecked().kind().code,
              rstd::io::error::ErrorKind::ReadOnlyFilesystem);

    auto created =
        vfs.open_write("/assets/new", owe::fs::WriteOptions { .create = true, .truncate = true });
    ASSERT_TRUE(created.is_ok());
    {
        owe::fs::BinaryWriter writer(std::move(created).unwrap_unchecked());
        EXPECT_EQ(writer.Write("new", 3), usize(3));
    }

    std::ifstream file(temp.path / "new");
    std::string   content;
    file >> content;
    EXPECT_EQ(content, "new");
}

TEST(PkgFs, ReusesHeaderAndRejectsInvalidEntryRanges) {
    TempDirectory temp;
    auto          valid_path = temp.path / "valid.pkg";
    WritePkg(valid_path, 5);

    auto pkg = owe::fs::WPPkgFs::open(owe::fs::ToPath(valid_path.string()));
    ASSERT_TRUE(pkg.is_ok());
    auto stamp = pkg->pkg_version_stamp();
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(stamp.data()), stamp.len()), "PKGV0001");

    auto source = pkg->open_read(owe::fs::ToPath("/materials/foo.BIN"));
    ASSERT_TRUE(source.is_ok());
    EXPECT_EQ(ReadText(std::move(source).unwrap_unchecked()), "hello");

    auto invalid_path = temp.path / "invalid.pkg";
    WritePkg(invalid_path, 100);
    auto invalid = owe::fs::WPPkgFs::open(owe::fs::ToPath(invalid_path.string()));
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(std::move(invalid).unwrap_err_unchecked().kind().code,
              rstd::io::error::ErrorKind::InvalidData);
}

} // namespace
