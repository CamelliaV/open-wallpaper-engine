module;
#include <rstd/macro.hpp>

export module wescene.vfs;
import rstd;

using ::alloc::string::String;
using ::alloc::sync::Arc;
using ::alloc::vec::Vec;
using namespace rstd::prelude;

export namespace owe::fs
{

using Path            = rstd::ref<rstd::path::Path>;
using MountHandle     = Arc<rstd::dyn<struct MountFs>>;
using ReadRange       = rstd::io::ReadRange;
using WriteSeekHandle = rstd::io::WriteSeekHandle;

struct FileMetadata {
    u64  len { 0 };
    bool is_file { false };
    bool is_directory { false };
    bool readonly { false };
};

struct WriteOptions {
    bool create { false };
    bool create_new { false };
    bool truncate { false };
    bool append { false };
};

struct MountId {
    u64 value { 0 };

    friend bool operator==(MountId lhs, MountId rhs) noexcept { return lhs.value == rhs.value; }
};

struct MountFs {
    using Trait                  = MountFs;
    static constexpr bool direct = false;

    template<typename Self, typename Delegate = void>
    struct Api {
        using Trait = MountFs;

        auto open_read(Path path) const -> rstd::io::Result<ReadRange> {
            return rstd::trait_call<0>(this, path);
        }

        auto open_write(Path path, WriteOptions options) const
            -> rstd::io::Result<WriteSeekHandle> {
            return rstd::trait_call<1>(this, path, options);
        }

        auto metadata(Path path) const -> rstd::io::Result<FileMetadata> {
            return rstd::trait_call<2>(this, path);
        }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::open_read, &T::open_write, &T::metadata>;
};

namespace detail
{

auto error(rstd::io::error::ErrorKind::Entity kind) -> rstd::io::error::Error {
    return rstd::io::error::Error::from_kind(rstd::io::error::ErrorKind { kind });
}

auto normalize(Path path, bool rooted) -> rstd::io::Result<rstd::path::PathBuf> {
    auto components = path.components();
    auto output     = rstd::path::PathBuf::make();

    if (rooted) {
        auto first = components.next();
        if (first.is_none() || ! first->is_root_dir()) {
            return rstd::Err(error(rstd::io::error::ErrorKind::InvalidInput));
        }
        output = rstd::path::PathBuf::from("/");
    }

    while (true) {
        auto component = components.next();
        if (component.is_none()) break;
        if (component->is_parent_dir() || component->is_root_dir()) {
            return rstd::Err(error(rstd::io::error::ErrorKind::InvalidInput));
        }
        if (! component->is_normal()) continue;
        output.push(Path(component->as_os_str()));
    }
    return rstd::Ok(rstd::move(output));
}

auto normalize_global(Path path) -> rstd::io::Result<rstd::path::PathBuf> {
    return normalize(path, true);
}

auto normalize_relative(Path path) -> rstd::io::Result<rstd::path::PathBuf> {
    return normalize(path, false);
}

bool is_mount_point(Path path) {
    auto components = path.components();
    auto root       = components.next();
    return root.is_some() && root->is_root_dir() && components.next().is_some();
}

bool is_not_found(const rstd::io::error::Error& error) {
    return error.kind().code == rstd::io::error::ErrorKind::NotFound;
}

bool is_readonly(const rstd::io::error::Error& error) {
    auto kind = error.kind().code;
    return kind == rstd::io::error::ErrorKind::ReadOnlyFilesystem ||
           kind == rstd::io::error::ErrorKind::Unsupported;
}

} // namespace detail

class PhysicalFs {
public:
    PhysicalFs(const PhysicalFs&)                        = delete;
    auto operator=(const PhysicalFs&) -> PhysicalFs&     = delete;
    PhysicalFs(PhysicalFs&&) noexcept                    = default;
    auto operator=(PhysicalFs&&) noexcept -> PhysicalFs& = default;

    static auto make(Path root, bool create) -> rstd::io::Result<PhysicalFs> {
        if (create) {
            auto created = rstd::fs::create_dir_all(root);
            if (created.is_err()) {
                return rstd::Err(rstd::move(created).unwrap_err_unchecked());
            }
        }

        auto metadata = rstd::fs::metadata(root);
        if (metadata.is_err()) return rstd::Err(rstd::move(metadata).unwrap_err_unchecked());
        if (! rstd::move(metadata).unwrap_unchecked().is_dir()) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::NotADirectory));
        }

        auto canonical = rstd::fs::canonicalize(root);
        if (canonical.is_err()) return rstd::Err(rstd::move(canonical).unwrap_err_unchecked());
        return rstd::Ok(PhysicalFs(rstd::move(canonical).unwrap_unchecked()));
    }

    auto open_read(Path path) const -> rstd::io::Result<ReadRange> {
        auto resolved = resolve_existing(path);
        if (resolved.is_err()) return rstd::Err(rstd::move(resolved).unwrap_err_unchecked());

        auto file = rstd::fs::File::open(resolved->as_path());
        if (file.is_err()) return rstd::Err(rstd::move(file).unwrap_err_unchecked());

        auto opened   = rstd::move(file).unwrap_unchecked();
        auto metadata = opened.metadata();
        if (metadata.is_err()) return rstd::Err(rstd::move(metadata).unwrap_err_unchecked());
        auto info = rstd::move(metadata).unwrap_unchecked();
        if (! info.is_file()) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::IsADirectory));
        }

        auto source = rstd::io::SharedReadAt::make(rstd::move(opened));
        return ReadRange::make(rstd::move(source), 0, info.len());
    }

    auto open_write(Path path, WriteOptions options) const -> rstd::io::Result<WriteSeekHandle> {
        auto resolved = resolve_write(path, options.create || options.create_new);
        if (resolved.is_err()) return rstd::Err(rstd::move(resolved).unwrap_err_unchecked());

        auto open = rstd::fs::File::options();
        open.write(! options.append)
            .append(options.append)
            .create(options.create)
            .create_new(options.create_new)
            .truncate(options.truncate);
        auto file = open.open(resolved->as_path());
        if (file.is_err()) return rstd::Err(rstd::move(file).unwrap_err_unchecked());
        return rstd::Ok(WriteSeekHandle::make(rstd::move(file).unwrap_unchecked()));
    }

    auto metadata(Path path) const -> rstd::io::Result<FileMetadata> {
        auto resolved = resolve_existing(path);
        if (resolved.is_err()) return rstd::Err(rstd::move(resolved).unwrap_err_unchecked());
        auto metadata = rstd::fs::metadata(resolved->as_path());
        if (metadata.is_err()) return rstd::Err(rstd::move(metadata).unwrap_err_unchecked());
        auto value = rstd::move(metadata).unwrap_unchecked();
        return rstd::Ok(FileMetadata { .len          = value.len(),
                                       .is_file      = value.is_file(),
                                       .is_directory = value.is_dir(),
                                       .readonly     = value.permissions().readonly() });
    }

private:
    explicit PhysicalFs(rstd::path::PathBuf root): m_root(rstd::move(root)) {}

    auto local_path(Path path) const -> rstd::io::Result<rstd::path::PathBuf> {
        auto local = detail::normalize_relative(path);
        if (local.is_err()) return rstd::Err(rstd::move(local).unwrap_err_unchecked());
        return rstd::Ok(m_root.join(local->as_path()));
    }

    auto ensure_in_root(rstd::path::PathBuf path) const -> rstd::io::Result<rstd::path::PathBuf> {
        if (! path.as_path().starts_with(m_root.as_path())) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::PermissionDenied));
        }
        return rstd::Ok(rstd::move(path));
    }

    auto resolve_existing(Path path) const -> rstd::io::Result<rstd::path::PathBuf> {
        auto full = local_path(path);
        if (full.is_err()) return rstd::Err(rstd::move(full).unwrap_err_unchecked());
        auto canonical = rstd::fs::canonicalize(full->as_path());
        if (canonical.is_err()) return rstd::Err(rstd::move(canonical).unwrap_err_unchecked());
        return ensure_in_root(rstd::move(canonical).unwrap_unchecked());
    }

    auto resolve_write(Path path, bool create) const -> rstd::io::Result<rstd::path::PathBuf> {
        auto full = local_path(path);
        if (full.is_err()) return rstd::Err(rstd::move(full).unwrap_err_unchecked());

        auto metadata = rstd::fs::metadata(full->as_path());
        if (metadata.is_ok()) {
            auto canonical = rstd::fs::canonicalize(full->as_path());
            if (canonical.is_err()) {
                return rstd::Err(rstd::move(canonical).unwrap_err_unchecked());
            }
            return ensure_in_root(rstd::move(canonical).unwrap_unchecked());
        }
        auto metadata_error = rstd::move(metadata).unwrap_err_unchecked();
        if (! detail::is_not_found(metadata_error) || ! create) {
            return rstd::Err(rstd::move(metadata_error));
        }

        auto parent = full->as_path().parent();
        if (parent.is_none()) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::InvalidInput));
        }

        auto existing = rstd::path::PathBuf::from(*parent);
        while (true) {
            auto exists = rstd::fs::metadata(existing.as_path());
            if (exists.is_ok()) break;
            auto error = rstd::move(exists).unwrap_err_unchecked();
            if (! detail::is_not_found(error) || ! existing.pop()) {
                return rstd::Err(rstd::move(error));
            }
        }
        auto canonical = rstd::fs::canonicalize(existing.as_path());
        if (canonical.is_err()) return rstd::Err(rstd::move(canonical).unwrap_err_unchecked());
        auto checked = ensure_in_root(rstd::move(canonical).unwrap_unchecked());
        if (checked.is_err()) return rstd::Err(rstd::move(checked).unwrap_err_unchecked());

        auto created = rstd::fs::create_dir_all(*parent);
        if (created.is_err()) return rstd::Err(rstd::move(created).unwrap_err_unchecked());
        auto parent_canonical = rstd::fs::canonicalize(*parent);
        if (parent_canonical.is_err()) {
            return rstd::Err(rstd::move(parent_canonical).unwrap_err_unchecked());
        }
        auto parent_checked = ensure_in_root(rstd::move(parent_canonical).unwrap_unchecked());
        if (parent_checked.is_err()) {
            return rstd::Err(rstd::move(parent_checked).unwrap_err_unchecked());
        }
        return rstd::Ok(rstd::move(full).unwrap_unchecked());
    }

    rstd::path::PathBuf m_root;
};

class VFS {
public:
    VFS(): m_state(State { .mounts = Vec<MountedFs>::make(), .next_id = 1 }) {}

    VFS(const VFS&)                    = delete;
    auto operator=(const VFS&) -> VFS& = delete;

    auto mount(Path mount_point, MountHandle fs, rstd::ref<rstd::str> name = {})
        -> rstd::io::Result<MountId> {
        auto normalized = detail::normalize_global(mount_point);
        if (normalized.is_err()) {
            return rstd::Err(rstd::move(normalized).unwrap_err_unchecked());
        }
        if (! detail::is_mount_point(normalized->as_path())) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::InvalidInput));
        }

        auto state = m_state.lock().unwrap_unchecked();
        if (state->next_id == u64(-1)) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::Other));
        }
        auto id = MountId { state->next_id++ };
        state->mounts.push(MountedFs { .id          = id,
                                       .name        = String::make(name),
                                       .mount_point = rstd::move(normalized).unwrap_unchecked(),
                                       .fs          = rstd::move(fs) });
        return rstd::Ok(id);
    }

    bool unmount(MountId id) {
        auto state = m_state.lock().unwrap_unchecked();
        for (usize i = state->mounts.len(); i > 0; --i) {
            if (state->mounts[i - 1].id == id) {
                state->mounts.remove(i - 1);
                return true;
            }
        }
        return false;
    }

    bool is_mounted(rstd::ref<rstd::str> name) const {
        auto state = m_state.lock().unwrap_unchecked();
        for (usize i = 0; i < state->mounts.len(); ++i) {
            if (state->mounts[i].name == name) return true;
        }
        return false;
    }

    auto open_read(Path path) const -> rstd::io::Result<ReadRange> {
        auto normalized = detail::normalize_global(path);
        if (normalized.is_err()) {
            return rstd::Err(rstd::move(normalized).unwrap_err_unchecked());
        }
        auto mounts = snapshot();
        for (usize i = mounts.len(); i > 0; --i) {
            auto& mount = mounts[i - 1];
            auto  local = normalized->as_path().strip_prefix(mount.mount_point.as_path());
            if (local.is_none()) continue;
            auto opened = mount.fs->open_read(*local);
            if (opened.is_ok()) return opened;
            auto error = rstd::move(opened).unwrap_err_unchecked();
            if (! detail::is_not_found(error)) return rstd::Err(rstd::move(error));
        }
        return rstd::Err(detail::error(rstd::io::error::ErrorKind::NotFound));
    }

    auto open_write(Path path, WriteOptions options) const -> rstd::io::Result<WriteSeekHandle> {
        if (options.create_new) {
            auto existing = metadata(path);
            if (existing.is_ok()) {
                return rstd::Err(detail::error(rstd::io::error::ErrorKind::AlreadyExists));
            }
            auto error = rstd::move(existing).unwrap_err_unchecked();
            if (! detail::is_not_found(error)) return rstd::Err(rstd::move(error));
        }

        auto normalized = detail::normalize_global(path);
        if (normalized.is_err()) {
            return rstd::Err(rstd::move(normalized).unwrap_err_unchecked());
        }
        auto mounts = snapshot();

        if (! options.create_new) {
            auto existing_options       = options;
            existing_options.create     = false;
            existing_options.create_new = false;
            for (usize i = mounts.len(); i > 0; --i) {
                auto& mount = mounts[i - 1];
                auto  local = normalized->as_path().strip_prefix(mount.mount_point.as_path());
                if (local.is_none()) continue;
                auto opened = mount.fs->open_write(*local, existing_options);
                if (opened.is_ok()) return opened;
                auto error = rstd::move(opened).unwrap_err_unchecked();
                if (! detail::is_not_found(error)) return rstd::Err(rstd::move(error));
            }
        }

        if (! options.create && ! options.create_new) {
            return rstd::Err(detail::error(rstd::io::error::ErrorKind::NotFound));
        }
        for (usize i = mounts.len(); i > 0; --i) {
            auto& mount = mounts[i - 1];
            auto  local = normalized->as_path().strip_prefix(mount.mount_point.as_path());
            if (local.is_none()) continue;
            auto opened = mount.fs->open_write(*local, options);
            if (opened.is_ok()) return opened;
            auto error = rstd::move(opened).unwrap_err_unchecked();
            if (! detail::is_readonly(error) && ! detail::is_not_found(error)) {
                return rstd::Err(rstd::move(error));
            }
        }
        return rstd::Err(detail::error(rstd::io::error::ErrorKind::ReadOnlyFilesystem));
    }

    auto metadata(Path path) const -> rstd::io::Result<FileMetadata> {
        auto normalized = detail::normalize_global(path);
        if (normalized.is_err()) {
            return rstd::Err(rstd::move(normalized).unwrap_err_unchecked());
        }
        auto mounts = snapshot();
        for (usize i = mounts.len(); i > 0; --i) {
            auto& mount = mounts[i - 1];
            auto  local = normalized->as_path().strip_prefix(mount.mount_point.as_path());
            if (local.is_none()) continue;
            auto result = mount.fs->metadata(*local);
            if (result.is_ok()) return result;
            auto error = rstd::move(result).unwrap_err_unchecked();
            if (! detail::is_not_found(error)) return rstd::Err(rstd::move(error));
        }
        return rstd::Err(detail::error(rstd::io::error::ErrorKind::NotFound));
    }

private:
    struct MountedFs {
        MountId             id;
        String              name;
        rstd::path::PathBuf mount_point;
        MountHandle         fs;

        auto clone() const -> MountedFs {
            return MountedFs {
                .id = id, .name = name.clone(), .mount_point = mount_point.clone(), .fs = fs.clone()
            };
        }
    };

    struct State {
        Vec<MountedFs> mounts;
        u64            next_id;
    };

    auto snapshot() const -> Vec<MountedFs> {
        auto state  = m_state.lock().unwrap_unchecked();
        auto result = Vec<MountedFs>::with_capacity(state->mounts.len());
        for (usize i = 0; i < state->mounts.len(); ++i) {
            result.push(state->mounts[i].clone());
        }
        return result;
    }

    rstd::sync::Mutex<State> m_state;
};

} // namespace owe::fs

namespace rstd
{

template<>
struct Impl<owe::fs::MountFs, owe::fs::PhysicalFs> : ImplBase<owe::fs::PhysicalFs> {
    auto open_read(owe::fs::Path path) const -> io::Result<io::ReadRange> {
        return this->self().open_read(path);
    }

    auto open_write(owe::fs::Path path, owe::fs::WriteOptions options) const
        -> io::Result<io::WriteSeekHandle> {
        return this->self().open_write(path, options);
    }

    auto metadata(owe::fs::Path path) const -> io::Result<owe::fs::FileMetadata> {
        return this->self().metadata(path);
    }
};

} // namespace rstd

export namespace owe::fs
{

auto make_physical_fs(Path root, bool create = false) -> rstd::io::Result<MountHandle> {
    auto fs = PhysicalFs::make(root, create);
    if (fs.is_err()) return rstd::Err(rstd::move(fs).unwrap_err_unchecked());
    return rstd::Ok(MountHandle::make(rstd::move(fs).unwrap_unchecked()));
}

} // namespace owe::fs
