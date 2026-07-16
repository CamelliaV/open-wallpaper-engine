export module wescene.rgraph:pass_node;
import rstd;

import :dependency_graph;
import :pass;

using namespace rstd::prelude;

export namespace owe::rg
{

struct PassNode {
    enum class Type
    {
        CustomShader,
        Copy,
        Virtual
    };

    NodeHandle handle;
    PassHandle pass;
    Type       type { Type::CustomShader };
    String     name { String::make("unknown pass") };

    auto Handle() const noexcept -> NodeHandle { return handle; }
    auto ToGraphviz() const -> String;
};

} // namespace owe::rg
