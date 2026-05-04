#include "Manifest.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>

namespace weweb {

namespace {

std::string LowerAscii(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

std::optional<WebManifest> LoadWebManifest(const std::filesystem::path& workshop_dir) {
    auto pj_path = workshop_dir / "project.json";
    std::ifstream is(pj_path);
    if (!is) {
        std::fprintf(stderr, "weweb: cannot open %s\n", pj_path.c_str());
        return std::nullopt;
    }

    // weweb-host inherits CEF's -fno-exceptions, so use the non-throwing
    // parser entry point.
    auto j = nlohmann::json::parse(is,
                                   /*callback=*/nullptr,
                                   /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    if (j.is_discarded()) {
        std::fprintf(stderr, "weweb: invalid JSON in %s\n", pj_path.c_str());
        return std::nullopt;
    }

    auto type_it = j.find("type");
    if (type_it == j.end() || !type_it->is_string()) {
        std::fprintf(stderr,
                     "weweb: %s is missing a string \"type\" field\n",
                     pj_path.c_str());
        return std::nullopt;
    }
    // WE corpus has both "web" and "Web" for the type field; fold case.
    std::string type = LowerAscii(type_it->get<std::string>());
    if (type != "web") {
        std::fprintf(stderr,
                     "weweb: %s has type=\"%s\", expected \"web\"\n",
                     pj_path.c_str(), type.c_str());
        return std::nullopt;
    }

    WebManifest m;
    m.entry_html = j.value("file",  std::string{"index.html"});
    m.title      = j.value("title", std::string{"Wallpaper"});

    if (auto pv = j.find("preview"); pv != j.end() && pv->is_string()) {
        m.preview = pv->get<std::string>();
    }

    if (auto gen = j.find("general"); gen != j.end() && gen->is_object()) {
        auto props = gen->find("properties");
        if (props != gen->end() && props->is_object()) {
            m.user_props = *props;
        }
    }

    return m;
}

}  // namespace weweb
