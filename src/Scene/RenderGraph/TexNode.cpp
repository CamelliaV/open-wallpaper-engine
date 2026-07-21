module wescene.rgraph;
import rstd;

using namespace rstd::prelude;
using namespace owe::rg;

namespace
{
auto DotEscape(rstd::ref<rstd::str> value) -> String {
    auto out = String::make();
    for (std::size_t index = 0; index < value.size().to_primitive(); ++index) {
        switch (static_cast<char>(value.data()[index])) {
        case '\\': out.push_str("\\\\"); break;
        case '"': out.push_str("\\\""); break;
        case '\n': out.push_str("\\n"); break;
        case '\r': break;
        default: out.push_back(value.data()[index]); break;
        }
    }
    return out;
}

auto TextureTypeName(TexNode::TexType type) -> rstd::ref<rstd::str> {
    switch (type) {
    case TexNode::TexType::Imported: return "Imported";
    case TexNode::TexType::Temp: return "Temp";
    }
    return "Unknown";
}
} // namespace

auto TexNode::ToGraphviz() const -> String {
    auto graph_id     = rstd::format("n{}", handle.index);
    auto escaped_key  = DotEscape(key.as_str());
    auto escaped_name = DotEscape(name.as_str());
    auto label = rstd::format("{}[label=\"ref={}\\nresource: {}\\nkey={}\\nkind={}\\nversion={}",
                              graph_id.as_str(),
                              graph_id.as_str(),
                              escaped_name.as_str(),
                              escaped_key.as_str(),
                              TextureTypeName(type),
                              version);
    if (writer) {
        auto field = rstd::format("\\nwriter=n{}", writer->index);
        label.push_str(field.as_str());
    }
    if (previous) {
        auto field = rstd::format("\\nprev=n{}", previous->index);
        label.push_str(field.as_str());
    }
    if (next) {
        auto field = rstd::format("\\nnext=n{}", next->index);
        label.push_str(field.as_str());
    }
    label.push_str("\" shape=ellipse]");
    return label;
}
