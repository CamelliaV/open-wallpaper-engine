export module wescene.rgraph:tex_node;
import rstd;
import :dependency_graph;

using namespace rstd::prelude;

export namespace owe::rg
{

struct TexNode {
    enum class TexType
    {
        Imported,
        Temp
    };

    NodeHandle               handle;
    TexType                  type { TexType::Imported };
    String                   key;
    String                   name { String::make("unknown tex") };
    usize                    version { 0 };
    rstd::Option<NodeHandle> previous;
    rstd::Option<NodeHandle> next;
    rstd::Option<NodeHandle> writer;

    auto Handle() const noexcept -> NodeHandle { return handle; }
    auto ToGraphviz() const -> String;
};

} // namespace owe::rg
