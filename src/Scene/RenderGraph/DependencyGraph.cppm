export module wescene.rgraph:dependency_graph;
import rstd;

using namespace rstd::prelude;

export namespace owe::rg
{

struct NodeHandle {
    usize index { numeric_limits<usize>::max() };

    bool valid() const noexcept { return index != numeric_limits<usize>::max(); }

    friend auto operator<=>(const NodeHandle&, const NodeHandle&) = default;
};

struct NodeHandleHasher {
    rstd::hash::RandomState state;

    auto operator()(NodeHandle handle) const noexcept -> rstd::u64 { return state(handle.index); }
};

struct Node {
    using Trait                  = Node;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = Node;

        auto Handle() const noexcept -> NodeHandle { return rstd::trait_call<0>(this); }
        auto ToGraphviz() const -> String { return rstd::trait_call<1>(this); }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Handle, &T::ToGraphviz>;
};

struct NodeLinks {
    rstd::vec::Vec<NodeHandle> incoming;
    rstd::vec::Vec<NodeHandle> outgoing;
};

struct DependencyGraph {
    auto AddNode() -> NodeHandle;
    auto Connect(NodeHandle from, NodeHandle to) -> bool;

    auto Contains(NodeHandle handle) const -> bool;
    auto NodeNum() const noexcept -> usize;
    auto EdgeNum() const noexcept -> usize;
    auto GetNodeOut(NodeHandle handle) const -> rstd::slice<NodeHandle>;
    auto GetNodeIn(NodeHandle handle) const -> rstd::slice<NodeHandle>;

    auto HasCycle() const -> bool;
    auto TopologicalOrder() const -> rstd::vec::Vec<NodeHandle>;

private:
    using LinkMap = rstd::collections::HashMap<NodeHandle, NodeLinks, NodeHandleHasher>;

    usize   m_next_index { 0 };
    LinkMap m_nodes;
};

} // namespace owe::rg
