module;

#include <nlohmann/json.hpp>

export module weweb:manifest;

import rstd.cppstd;

export namespace weweb
{

struct WebManifest {
    std::string                title;
    std::string                entry_html;
    nlohmann::json             user_props;
    std::optional<std::string> preview;
};

std::optional<WebManifest> LoadWebManifest(const std::filesystem::path& workshop_dir);

} // namespace weweb
