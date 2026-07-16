module wescene.rgraph;
import rstd;

using namespace rstd::prelude;
using namespace owe::rg;

namespace
{
auto DotEscape(rstd::ref<rstd::str> value) -> String {
    auto out = String::make();
    for (usize index = 0; index < value.size(); ++index) {
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

auto PassTypeName(PassNode::Type type) -> rstd::ref<rstd::str> {
    switch (type) {
    case PassNode::Type::CustomShader: return "CustomShader";
    case PassNode::Type::Copy: return "Copy";
    case PassNode::Type::Virtual: return "Virtual";
    }
    return "Unknown";
}
} // namespace

auto PassNode::ToGraphviz() const -> String {
    auto graph_id = rstd::format("n{}", handle.index);
    auto escaped  = DotEscape(name.as_str());
    return rstd::format("{}[label=\"ref={}\\npass-ref=p{}\\npass: {}\\ntype={}\"]",
                        graph_id.as_str(),
                        graph_id.as_str(),
                        pass.index,
                        escaped.as_str(),
                        PassTypeName(type));
}
