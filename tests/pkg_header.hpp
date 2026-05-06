// Shared scene.pkg header re-parser used by the test/CLI tools.
//
// We deliberately re-parse scene.pkg's header here instead of reaching into
// WPPkgFs internals: the production class throws away the version string
// after logging it and exposes neither file enumeration nor the version.
// Re-parsing is ~15 lines and keeps the production code untouched. Hoisted
// out of dump.cpp / scene_keys.cpp / wptexparse_main.cpp once the count of
// callers reached four (wpscriptdump joining the set).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace owe::testing {

struct PkgEntry {
    std::string  path;
    std::int32_t offset { 0 };
    std::int32_t length { 0 };
};

// Reads the header of a scene.pkg-format file. `version` is filled with the
// stamp (e.g. "PKGV0001") and `entries` lists every file with its absolute
// in-pkg path (leading slash) and (offset, length) into the body.
// Returns false on stream open / shape errors.
bool ReadPkgHeader(const std::string& pkg_path, std::string& version,
                   std::vector<PkgEntry>& entries);

} // namespace owe::testing
