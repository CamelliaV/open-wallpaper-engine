module wescene.rgraph;
import rstd;
import cppstd;

using namespace rstd::prelude;
using namespace owe::rg;

namespace
{
bool SliceContains(rstd::slice<NodeHandle> handles, NodeHandle needle) {
    for (usize index = 0; index < handles.len(); ++index) {
        if (handles[index] == needle) return true;
    }
    return false;
}
} // namespace

auto DependencyGraph::AddNode() -> NodeHandle {
    NodeHandle handle { .index = m_next_index++ };
    (void)m_nodes.insert(handle, NodeLinks {});
    return handle;
}

auto DependencyGraph::Connect(NodeHandle from, NodeHandle to) -> bool {
    auto from_links = m_nodes.get_mut(from);
    auto to_links   = m_nodes.get_mut(to);
    if (from_links.is_none() || to_links.is_none()) return false;

    if (! SliceContains((**from_links).outgoing.as_slice(), to)) {
        (**from_links).outgoing.push(NodeHandle { to });
    }
    if (! SliceContains((**to_links).incoming.as_slice(), from)) {
        (**to_links).incoming.push(NodeHandle { from });
    }
    return true;
}

auto DependencyGraph::Contains(NodeHandle handle) const -> bool {
    return m_nodes.contains_key(handle);
}

auto DependencyGraph::NodeNum() const noexcept -> usize { return m_nodes.len(); }

auto DependencyGraph::EdgeNum() const noexcept -> usize {
    usize count = 0;
    auto  nodes = m_nodes.values();
    for (auto node = nodes.next(); node.is_some(); node = nodes.next()) {
        count += (**node).outgoing.len();
    }
    return count;
}

auto DependencyGraph::GetNodeOut(NodeHandle handle) const -> rstd::slice<NodeHandle> {
    auto links = m_nodes.get(handle);
    return links.is_some() ? (**links).outgoing.as_slice() : rstd::slice<NodeHandle> {};
}

auto DependencyGraph::GetNodeIn(NodeHandle handle) const -> rstd::slice<NodeHandle> {
    auto links = m_nodes.get(handle);
    return links.is_some() ? (**links).incoming.as_slice() : rstd::slice<NodeHandle> {};
}

auto DependencyGraph::TopologicalOrder() const -> rstd::vec::Vec<NodeHandle> {
    auto in_degree = rstd::vec::Vec<usize>::make();
    in_degree.resize(NodeNum(), 0);

    auto ready = rstd::vec::Vec<NodeHandle>::make();
    for (usize index = 0; index < NodeNum(); ++index) {
        NodeHandle handle { .index = index };
        in_degree[index] = GetNodeIn(handle).len();
        if (in_degree[index] == 0) ready.push(NodeHandle { handle });
    }

    auto result = rstd::vec::Vec<NodeHandle>::with_capacity(NodeNum());
    while (! ready.is_empty()) {
        std::sort(ready.begin(), ready.end());
        auto handle = ready.remove(0);
        result.push(NodeHandle { handle });

        auto outgoing = GetNodeOut(handle);
        for (usize index = 0; index < outgoing.len(); ++index) {
            auto next = outgoing[index];
            if (in_degree[next.index] == 0) continue;
            --in_degree[next.index];
            if (in_degree[next.index] == 0) ready.push(NodeHandle { next });
        }
    }
    return result;
}

auto DependencyGraph::HasCycle() const -> bool { return TopologicalOrder().len() != NodeNum(); }
