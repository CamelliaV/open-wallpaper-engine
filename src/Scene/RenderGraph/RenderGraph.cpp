module;

#include <rstd/macro.hpp>

module wescene.rgraph;
import rstd;
import cppstd;

using namespace rstd::prelude;
using namespace owe::rg;

namespace
{
auto ToTexType(TextureKind kind) -> TexNode::TexType {
    return kind == TextureKind::Temp ? TexNode::TexType::Temp : TexNode::TexType::Imported;
}

auto ToTextureKind(TexNode::TexType type) -> TextureKind {
    switch (type) {
    case TexNode::TexType::Imported: return TextureKind::Imported;
    case TexNode::TexType::Temp: return TextureKind::Temp;
    }
    return TextureKind::Imported;
}

auto ToTextureDesc(const TexNode& node) -> TextureDesc {
    return TextureDesc {
        .name = node.name.clone(),
        .key  = node.key.clone(),
        .kind = ToTextureKind(node.type),
    };
}

template<typename T>
    requires rstd::Impled<T, Node>
auto NodeGraphviz(const T& node) -> String {
    return rstd::as<Node>(node).ToGraphviz();
}

auto Sorted(rstd::slice<NodeHandle> handles) -> rstd::vec::Vec<NodeHandle> {
    auto sorted = rstd::vec::Vec<NodeHandle>::with_capacity(handles.len());
    sorted.extend_from_slice(handles);
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}
} // namespace

auto RenderGraph::getPassNode(NodeHandle handle) -> rstd::Option<PassNode&> {
    auto node = m_pass_nodes.get_mut(handle);
    if (node.is_none()) return rstd::None();
    return rstd::Some<PassNode&>(**node);
}

auto RenderGraph::getPassNode(NodeHandle handle) const -> rstd::Option<const PassNode&> {
    auto node = m_pass_nodes.get(handle);
    if (node.is_none()) return rstd::None();
    return rstd::Some<const PassNode&>(**node);
}

auto RenderGraph::getTexNode(NodeHandle handle) -> rstd::Option<TexNode&> {
    auto node = m_tex_nodes.get_mut(handle);
    if (node.is_none()) return rstd::None();
    return rstd::Some<TexNode&>(**node);
}

auto RenderGraph::getTexNode(NodeHandle handle) const -> rstd::Option<const TexNode&> {
    auto node = m_tex_nodes.get(handle);
    if (node.is_none()) return rstd::None();
    return rstd::Some<const TexNode&>(**node);
}

auto RenderGraph::getPass(PassHandle handle) -> rstd::Option<Pass&> {
    auto pass = m_passes.get_mut(handle);
    if (pass.is_none() || ! **pass) return rstd::None();
    return rstd::Some<Pass&>(***pass);
}

auto RenderGraph::getPass(PassHandle handle) const -> rstd::Option<const Pass&> {
    auto pass = m_passes.get(handle);
    if (pass.is_none() || ! **pass) return rstd::None();
    return rstd::Some<const Pass&>(***pass);
}

auto RenderGraph::passState(NodeHandle handle) const -> rstd::Option<PassNodeState> {
    auto node = getPassNode(handle);
    if (node.is_none()) return rstd::None();
    return rstd::Some(PassNodeState {
        .handle = handle,
        .pass   = node->pass,
        .name   = node->name.clone(),
        .type   = node->type,
    });
}

void RenderGraph::ToGraphviz(rstd::ref<rstd::str> path) const {
    auto output = String::make("digraph framegraph {\nnode [shape=box]\n");

    for (usize index = 0; index < m_dg.NodeNum(); ++index) {
        NodeHandle handle { .index = index };
        if (auto pass = getPassNode(handle); pass.is_some()) {
            auto declaration = NodeGraphviz(*pass);
            output.push_str(declaration.as_str());
            output.push_back('\n');
        } else if (auto texture = getTexNode(handle); texture.is_some()) {
            auto declaration = NodeGraphviz(*texture);
            output.push_str(declaration.as_str());
            output.push_back('\n');
        }
    }

    for (usize index = 0; index < m_dg.NodeNum(); ++index) {
        NodeHandle from { .index = index };
        for (auto to : Sorted(m_dg.GetNodeOut(from))) {
            rstd::ref<rstd::str> access       = "order";
            auto                 texture_from = getTexNode(from);
            auto                 pass_to      = getPassNode(to);
            if (texture_from && pass_to) {
                if (texture_from->next) {
                    auto next = getTexNode(*texture_from->next);
                    if (next && next->writer && *next->writer == to) access = "read/version";
                }
                if (access == "order") access = "read";
            } else if (auto pass_from = getPassNode(from); pass_from) {
                auto texture_to = getTexNode(to);
                if (texture_to && texture_to->writer && *texture_to->writer == from) {
                    access = "write";
                }
            }
            auto edge =
                rstd::format("n{}->n{}[label=\"access={}\"]\n", from.index, to.index, access);
            output.push_str(edge.as_str());
        }
    }

    output.push_back('}');
    (void)rstd::fs::write(rstd::ref<rstd::path::Path>(path), rstd::str_::as_bytes(output.as_str()));
}

auto RenderGraph::textureState(TextureNodeRef ref) const -> rstd::Option<TextureNodeState> {
    auto node = getTexNode(ref.handle);
    if (node.is_none()) return rstd::None();
    return rstd::Some(TextureNodeState {
        .ref     = ref,
        .desc    = ToTextureDesc(*node),
        .version = node->version,
    });
}

auto RenderGraph::readTexture(NodeHandle pass_node, TextureNodeRef texture) -> bool {
    if (getPassNode(pass_node).is_none() || textureState(texture).is_none()) return false;

    RenderGraphBuilder builder(*this, pass_node);
    builder.read(texture);
    return true;
}

auto RenderGraph::topologicalOrder() const -> rstd::vec::Vec<NodeHandle> {
    auto in_degree = rstd::vec::Vec<usize>::make();
    in_degree.resize(m_dg.NodeNum(), 0);
    for (usize index = 0; index < m_dg.NodeNum(); ++index) {
        NodeHandle handle { .index = index };
        in_degree[index] = m_dg.GetNodeIn(handle).len();
    }

    auto ready = rstd::vec::Vec<NodeHandle>::with_capacity(m_dg.NodeNum());
    for (usize index = 0; index < m_dg.NodeNum(); ++index) {
        if (in_degree[index] == 0) ready.push(NodeHandle { .index = index });
    }

    auto                 pass_nodes = rstd::vec::Vec<NodeHandle>::make();
    rstd::Option<String> active_target;
    usize                visited = 0;

    auto choose_ready = [&]() -> usize {
        std::sort(ready.begin(), ready.end());

        for (usize index = 0; index < ready.len(); ++index) {
            if (! isRenderPassNode(ready[index])) return index;
        }

        if (active_target) {
            for (usize index = 0; index < ready.len(); ++index) {
                auto target = passWriteTarget(ready[index]);
                if (target && *target == *active_target) return index;
            }
        }

        using CountMap     = rstd::collections::HashMap<String, usize>;
        auto target_counts = CountMap::make();
        for (auto handle : ready) {
            auto target = passWriteTarget(handle);
            if (! target) continue;
            auto count = target_counts.get_mut(*target);
            if (count)
                ++**count;
            else
                (void)target_counts.insert(target->clone(), 1);
        }

        usize best_index = 0;
        usize best_count = 0;
        for (usize index = 0; index < ready.len(); ++index) {
            usize count  = 0;
            auto  target = passWriteTarget(ready[index]);
            if (target) {
                auto stored = target_counts.get(*target);
                if (stored) count = **stored;
            }
            if (count > best_count) {
                best_index = index;
                best_count = count;
            }
        }
        return best_index;
    };

    while (! ready.is_empty()) {
        auto handle = ready.remove(choose_ready());
        ++visited;

        if (isRenderPassNode(handle)) {
            pass_nodes.push(NodeHandle { handle });
            active_target = passWriteTarget(handle);
        }

        for (auto next : Sorted(m_dg.GetNodeOut(handle))) {
            rstd_assert(in_degree[next.index] > 0);
            --in_degree[next.index];
            if (in_degree[next.index] == 0) ready.push(NodeHandle { next });
        }
    }

    if (visited == m_dg.NodeNum()) return pass_nodes;

    pass_nodes.clear();
    for (auto handle : m_dg.TopologicalOrder()) {
        if (isRenderPassNode(handle)) pass_nodes.push(NodeHandle { handle });
    }
    return pass_nodes;
}

auto RenderGraph::isPassNode(NodeHandle handle) const -> bool {
    return m_pass_nodes.contains_key(handle);
}

auto RenderGraph::isVirtualPassNode(NodeHandle handle) const -> bool {
    auto node = getPassNode(handle);
    return node && node->type == PassNode::Type::Virtual;
}

auto RenderGraph::isRenderPassNode(NodeHandle handle) const -> bool {
    return isPassNode(handle) && ! isVirtualPassNode(handle);
}

auto RenderGraph::passWriteTarget(NodeHandle handle) const -> rstd::Option<String> {
    if (! isPassNode(handle)) return rstd::None();

    for (auto out : Sorted(m_dg.GetNodeOut(handle))) {
        auto texture = getTexNode(out);
        if (texture && texture->writer && *texture->writer == handle) {
            return rstd::Some(texture->key.clone());
        }
    }
    return rstd::None();
}

RenderGraphBuilder::RenderGraphBuilder(RenderGraph& graph, NodeHandle pass_node)
    : m_rg(graph), m_passnode_wip(pass_node) {}

void RenderGraphBuilder::markSelfWrite(TextureNodeRef ref) {
    auto state = m_rg.textureState(ref);
    rstd_assert(state.is_some());
    if (! state || state->version > 0) return;
    m_rg.addPass<VirtualPass>(
        "virtual pass", PassNode::Type::Virtual, [ref](RenderGraphBuilder& builder, auto&) {
            builder.write(ref);
        });
}

void RenderGraphBuilder::markVirtualWrite(TextureNodeRef ref) {
    auto state = m_rg.textureState(ref);
    rstd_assert(state.is_some());
    if (! state || state->version > 0 || m_rg.textureHasWriter(ref)) return;
    m_rg.addPass<VirtualPass>(
        "virtual pass", PassNode::Type::Virtual, [ref](RenderGraphBuilder& builder, auto&) {
            builder.write(ref);
        });
}

auto RenderGraphBuilder::createTexture(const TextureDesc& desc, bool write) -> TextureNodeRef {
    return createTextureNode(desc, write);
}

void RenderGraphBuilder::read(TextureNodeRef ref) {
    auto state = m_rg.textureState(ref);
    rstd_assert(state.is_some());
    if (state) readTextureNode(ref);
}

void RenderGraphBuilder::write(TextureNodeRef ref) {
    auto state = m_rg.textureState(ref);
    rstd_assert(state.is_some());
    if (state) writeTextureNode(ref);
}

auto RenderGraphBuilder::textureState(TextureNodeRef ref) const -> rstd::Option<TextureNodeState> {
    return m_rg.textureState(ref);
}

auto RenderGraphBuilder::createTextureNode(const TextureDesc& desc, bool write) -> TextureNodeRef {
    return m_rg.createTextureNode(desc, write);
}

void RenderGraphBuilder::readTextureNode(TextureNodeRef ref) {
    m_rg.connectTextureRead(ref, m_passnode_wip);
}

void RenderGraphBuilder::writeTextureNode(TextureNodeRef ref) {
    m_rg.connectTextureWrite(ref, m_passnode_wip);
}

auto RenderGraph::createTextureNode(const TextureDesc& desc, bool write) -> TextureNodeRef {
    TextureNodeRef ref;
    auto           current = m_key_texnode.get(desc.key);
    if (current) {
        TextureNodeRef old { .handle = **current };
        ref = write && textureHasWriter(old) ? createNewTextureNode(desc) : old;
    } else {
        ref = createNewTextureNode(desc);
    }
    rstd_assert(ref.valid());
    return ref;
}

auto RenderGraph::createNewTextureNode(const TextureDesc& desc) -> TextureNodeRef {
    auto handle  = m_dg.AddNode();
    auto current = m_key_texnode.get(desc.key);

    TexNode node {
        .handle = handle,
        .type   = ToTexType(desc.kind),
        .key    = desc.key.clone(),
        .name   = desc.name.clone(),
    };
    if (current) {
        auto previous = getTexNode(**current);
        rstd_assert(previous.is_some());
        if (previous) {
            node.version   = previous->version + 1;
            node.previous  = rstd::Some(previous->handle);
            previous->next = rstd::Some(handle);
        }
    }

    (void)m_tex_nodes.insert(handle, std::move(node));
    (void)m_key_texnode.insert(desc.key.clone(), handle);
    return TextureNodeRef { .handle = handle };
}

void RenderGraph::connectTextureRead(TextureNodeRef ref, NodeHandle pass_node) {
    auto texture = getTexNode(ref.handle);
    if (! texture || getPassNode(pass_node).is_none()) return;
    (void)m_dg.Connect(ref.handle, pass_node);

    if (texture->next) {
        auto next = getTexNode(*texture->next);
        if (next && next->writer) (void)m_dg.Connect(pass_node, *next->writer);
    }
}

void RenderGraph::connectTextureWrite(TextureNodeRef ref, NodeHandle pass_node) {
    auto node = getTexNode(ref.handle);
    if (! node || getPassNode(pass_node).is_none()) return;

    if (node->previous) {
        auto previous = *node->previous;
        auto readers  = m_dg.GetNodeOut(previous);
        for (usize index = 0; index < readers.len(); ++index) {
            auto reader = readers[index];
            if (isPassNode(reader)) (void)m_dg.Connect(reader, pass_node);
        }
        if (readers.len() == 0) (void)m_dg.Connect(previous, pass_node);
    }
    (void)m_dg.Connect(pass_node, ref.handle);
    node->writer = rstd::Some(pass_node);
}

auto RenderGraph::textureHasWriter(TextureNodeRef ref) const -> bool {
    auto node = getTexNode(ref.handle);
    return node && node->writer.is_some();
}

auto RenderGraphBuilder::workPassNode() const -> const PassNode& {
    auto node = m_rg.getPassNode(m_passnode_wip);
    rstd_assert(node.is_some());
    return *node;
}

auto RenderGraph::getLastReadTextures(rstd::slice<NodeHandle> nodes) const
    -> rstd::vec::Vec<rstd::vec::Vec<TextureNodeState>> {
    auto result = rstd::vec::Vec<rstd::vec::Vec<TextureNodeState>>::with_capacity(nodes.len());
    for (usize index = 0; index < nodes.len(); ++index) {
        result.push(rstd::vec::Vec<TextureNodeState>::make());
    }

    using SeenMap = rstd::collections::HashMap<NodeHandle, bool, NodeHandleHasher>;
    auto seen     = SeenMap::make();
    for (usize offset = nodes.len(); offset > 0; --offset) {
        auto index  = offset - 1;
        auto inputs = m_dg.GetNodeIn(nodes[index]);
        for (usize input_index = 0; input_index < inputs.len(); ++input_index) {
            auto input = inputs[input_index];
            if (seen.contains_key(input)) continue;
            (void)seen.insert(input, true);
            auto state = textureState(TextureNodeRef { .handle = input });
            if (state) result[index].push(std::move(*state));
        }
    }
    return result;
}
