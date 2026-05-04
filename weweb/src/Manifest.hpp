#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace weweb {

// In-memory representation of a Wallpaper Engine *web* `project.json`.
struct WebManifest {
    std::string title;          // project.json:title (default "Wallpaper")
    std::string entry_html;     // project.json:file  (default "index.html")
    nlohmann::json user_props;  // project.json:general.properties (object or null)
    std::optional<std::string> preview;  // project.json:preview, if any
};

// Read `<workshop_dir>/project.json` and parse it as a type=web manifest.
// Returns std::nullopt and prints a one-line diagnostic to stderr on
// missing/unreadable file, JSON parse failure, or `type != "web"`. Cannot
// throw — weweb-host inherits CEF's -fno-exceptions compile flag.
std::optional<WebManifest> LoadWebManifest(const std::filesystem::path& workshop_dir);

}  // namespace weweb
