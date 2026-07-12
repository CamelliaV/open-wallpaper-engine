export module weweb:manifest;

import rstd.cppstd;
import wescene.json;

export namespace weweb
{

struct WebManifest {
    std::string                title;
    std::string                entry_html;
    owe::Json                  user_props;
    std::optional<std::string> preview;
};

std::optional<WebManifest> LoadWebManifest(const std::filesystem::path& workshop_dir);

} // namespace weweb
