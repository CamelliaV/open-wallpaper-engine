module;

#include <rstd/macro.hpp>
#include "Utils/String.h"

module wescene.parse;
import nlohmann.json;
import rstd.cppstd;
import rstd.log;

// WE shader annotation collector. Walks the source line by line and pulls
// `// [COMBO]` / `uniform NAME; // {json}` annotations into WPShaderInfo.
// Collection is unconditional — #if/#endif dead-branch stripping is glslang's
// job downstream, and gating here creates chicken-and-egg cycles (texture
// combo flag inside `#if MASK == 1` depends on its own annotation).

namespace owe {

namespace {

std::string StripBlockComments(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    std::size_t i = 0;
    while (i < src.size()) {
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            out.push_back(' ');
            out.push_back(' ');
            i += 2;
            while (i < src.size() &&
                   !(i + 1 < src.size() && src[i] == '*' && src[i + 1] == '/')) {
                out.push_back(src[i] == '\n' ? '\n' : ' ');
                ++i;
            }
            if (i + 1 < src.size()) {
                out.push_back(' ');
                out.push_back(' ');
                i += 2;
            }
        } else {
            out.push_back(src[i]);
            ++i;
        }
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    std::size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool ParseUniformTokens(std::string_view line, std::string& type_out,
                        std::string& name_out) {
    auto stripped = Trim(line);
    if (stripped.size() < 9 || stripped.substr(0, 7) != "uniform") return false;
    if (!(stripped[7] == ' ' || stripped[7] == '\t')) return false;
    auto rest = Trim(stripped.substr(7));
    std::size_t i = 0;
    while (i < rest.size() && rest[i] != ' ' && rest[i] != '\t') ++i;
    if (i == 0 || i == rest.size()) return false;
    type_out = std::string(rest.substr(0, i));
    auto rem = Trim(rest.substr(i));
    std::size_t j = 0;
    while (j < rem.size() && (std::isalnum((unsigned char)rem[j]) || rem[j] == '_')) ++j;
    if (j == 0) return false;
    name_out = std::string(rem.substr(0, j));
    return true;
}

std::size_t FindJsonBlobStart(std::string_view line) {
    auto comment = line.find("//");
    if (comment == std::string_view::npos) return std::string_view::npos;
    auto rest = line.substr(comment + 2);
    auto brace = rest.find('{');
    if (brace == std::string_view::npos) return std::string_view::npos;
    return comment + 2 + brace;
}

void HandleComboLine(WPShaderInfo* info, std::string_view line) {
    auto brace = line.find('{');
    if (brace == std::string_view::npos) return;
    nlohmann::json j;
    if (! ParseJson(std::string(line.substr(brace)), j)) return;
    if (! j.contains("combo")) return;
    wpscene::WPCombo combo;
    combo.FromJson(j);
    if (combo.combo.empty()) return;
    info->combos[combo.combo] = std::to_string(combo.default_);
    info->combo_defs.push_back(std::move(combo));
}

void HandleUniformLine(WPShaderInfo*                       info,
                      std::span<const WPShaderTexInfo>     texinfos,
                      std::string_view                     line) {
    auto semi = line.find(';');
    if (semi == std::string_view::npos) return;
    std::string type_tok, name;
    if (! ParseUniformTokens(line.substr(0, semi), type_tok, name)) return;

    auto blob_start = FindJsonBlobStart(line.substr(semi + 1));
    if (blob_start == std::string_view::npos) return;
    blob_start += semi + 1;
    nlohmann::json sv_json;
    if (! ParseJson(std::string(line.substr(blob_start)), sv_json)) return;

    std::string material_key;
    GetJsonValue(sv_json, "material", material_key, false);
    if (! material_key.empty()) info->alias[material_key] = name;

    const bool is_tex   = name.compare(0, 9, "g_Texture") == 0;
    const idx  texcount = std::ssize(texinfos);

    if (is_tex) {
        wpscene::WPUniformTex wput;
        wput.FromJson(sv_json);
        i32 index { 0 };
        STRTONUM(name.substr(9), index);
        if (! wput.default_.empty()) {
            info->defTexs.push_back({ index, wput.default_ });
        }
        if (! wput.combo.empty()) {
            info->combos[wput.combo] = (index >= texcount) ? "0" : "1";
        }
        if (index < texcount && texinfos[(usize)index].enabled) {
            auto& compos = texinfos[(usize)index].composEnabled;
            usize num = std::min(std::size(compos), std::size(wput.components));
            for (usize i = 0; i < num; i++) {
                if (compos[i]) info->combos[wput.components[i].combo] = "1";
            }
        }
    } else {
        wpscene::WPUniformVar var;
        var.FromJson(sv_json, name);
        if (sv_json.contains("default")) {
            const auto& value = sv_json.at("default");
            ShaderValue sv;
            if (value.is_string()) {
                std::vector<float> v;
                GetJsonValue(value, v);
                sv = std::span<const float>(v);
            } else if (value.is_number()) {
                sv.setSize(1);
                GetJsonValue(value, sv[0]);
            }
            info->svs[name] = sv;
        }
        if (sv_json.contains("combo")) {
            std::string cname;
            GetJsonValue(sv_json, "combo", cname);
            if (! cname.empty()) info->combos[cname] = "1";
        }
        info->scalar_uniforms.push_back(std::move(var));
    }
}

} // namespace

void ParseWPShader(const std::string& src_in, WPShaderInfo* info,
                   const std::vector<WPShaderTexInfo>& texinfos_vec) {
    const std::string src = StripBlockComments(src_in);
    std::span<const WPShaderTexInfo> texinfos(texinfos_vec.data(), texinfos_vec.size());
    std::size_t pos = 0;
    while (pos <= src.size()) {
        auto nl = src.find('\n', pos);
        std::string_view line(src.data() + pos,
                              (nl == std::string::npos ? src.size() : nl) - pos);
        auto trimmed = Trim(line);
        if (! trimmed.empty()) {
            if (trimmed.find("// [COMBO]") != std::string_view::npos) {
                HandleComboLine(info, line);
            } else if (trimmed.size() >= 8 && trimmed.substr(0, 7) == "uniform" &&
                       (trimmed[7] == ' ' || trimmed[7] == '\t')) {
                HandleUniformLine(info, texinfos, line);
            }
            // Helpers / forward decls above `void main()` are the annotated
            // region; the function body never carries new annotations.
            if (trimmed.find("void main(") != std::string_view::npos) break;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

} // namespace owe
