module;

#include <rstd/macro.hpp>

module wescene.scene;
import wescene.spec_names;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace owe;

namespace
{

void ChangeMeshToUnitQuad(SceneMesh& target) {
    SceneMesh mesh;
    // clang-format off
    const rstd::array<float, 12> pos = {
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
    };
    const rstd::array<float, 8> tex_coord = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
    };
    // clang-format on

    SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord }), usize(4));
    vertex.SetVertex(rstd::cppstd::as_string_view(WE_IN_POSITION), pos.as_slice());
    vertex.SetVertex(rstd::cppstd::as_string_view(WE_IN_TEXCOORD), tex_coord.as_slice());
    mesh.AddVertexArray(std::move(vertex));
    target.ChangeMeshDataFrom(mesh);
}

} // namespace

SceneNodeLayer::SceneNodeLayer(SceneNode* node, float w, float h, std::string_view pingpong_a,
                               std::string_view pingpong_b)
    : m_worldNode(node),
      m_sourceNode(node),
      m_width(w),
      m_height(h),
      m_pingpong_a(pingpong_a),
      m_pingpong_b(pingpong_b),
      m_source_camera(node != nullptr ? node->Camera() : std::string()),
      m_final_mesh(Box<SceneMesh>::make()) {};

void SceneNodeLayer::SetSourceDraw(SceneNode& node) {
    m_sourceNode    = &node;
    m_source_camera = node.Camera();
}

void SceneNodeLayer::ConfigureSourceDraw(bool intermediate) {
    if (m_sourceNode == nullptr) return;
    if (intermediate) {
        m_sourceNode->SetCamera(m_source_camera);
        return;
    }
    if (! m_final_camera.empty()) {
        m_sourceNode->SetCamera(m_final_camera);
    } else {
        m_sourceNode->SetCamera(m_sourceNode->Perspective() ? "global_perspective" : "");
    }
}

void SceneNodeLayer::ResolveEffect(const SceneMesh& default_mesh, std::string_view effect_cam) {
    if (m_resolved) return;
    m_resolved_effects.clear();
    std::string_view ppong_a = m_pingpong_a, ppong_b = m_pingpong_b;
    auto             swap_pp = [&ppong_a, &ppong_b]() {
        std::swap(ppong_a, ppong_b);
    };
    auto default_node = SceneNode();

    SceneImageEffectNode* last_output { nullptr };
    auto                  resolve_effect = [&](SceneImageEffect& eff) {
        SceneImageEffectNode* effect_output { nullptr };
        for (auto& cmd : eff.commands) {
            auto state_it = m_command_resolve_state
                                .try_emplace(&cmd,
                                             EffectCommandResolveState {
                                                 .src = cmd.src,
                                                 .dst = cmd.dst,
                                             })
                                .first;
            cmd.src       = state_it->second.src;
            cmd.dst       = state_it->second.dst;
            if (cmd.src == m_pingpong_a) cmd.src = ppong_a;
            if (cmd.dst == m_pingpong_a) cmd.dst = ppong_a;
        }
        for (auto it = eff.nodes.begin(); it != eff.nodes.end(); it++) {
            rstd_assert(it->sceneNode->HasMaterial());
            auto& material            = *(it->sceneNode->Mesh()->Material());
            auto [state_it, inserted] = m_node_resolve_state.try_emplace(
                &(*it), EffectNodeResolveState { .output = it->output });
            auto& state = state_it->second;
            if (inserted) {
                for (std::size_t i = 0; i < material.textures.size(); ++i) {
                    if (material.textures[i] == m_pingpong_a)
                        state.pingpong_input_slots.push_back(i);
                }
            }
            it->output = state.output;
            for (std::size_t slot : state.pingpong_input_slots) {
                if (slot < material.textures.size()) material.textures[slot] = ppong_a;
            }

            auto output = rstd::cppstd::as_str(it->output).unwrap();
            if (it->output == m_pingpong_b || output == SpecTex_Default) {
                it->output  = ppong_b;
                last_output = &(*it);
            }
            effect_output = &(*it);

            {
                material.blenmode = BlendMode::Normal;
                it->sceneNode->SetCamera(effect_cam.data());
                it->sceneNode->CopyTrans(default_node);
                it->sceneNode->Mesh()->ChangeMeshDataFrom(default_mesh);
            }
        }
        m_resolved_effects.push_back(&eff);
        swap_pp();
        return effect_output;
    };
    for (auto& eff : m_effects) {
        if (eff && eff->runtime_visible) resolve_effect(*eff);
    }
    if (m_final_resolve_effect) resolve_effect(*m_final_resolve_effect);
    SceneImageEffectNode* published_output { nullptr };
    if (m_published_effect) published_output = resolve_effect(*m_published_effect);
    SceneImageEffectNode* visible_output { nullptr };
    if (m_visible_output_enabled && m_visible_resolve_effect)
        visible_output = resolve_effect(*m_visible_resolve_effect);

    auto* final_output = visible_output != nullptr
                             ? visible_output
                             : (published_output == nullptr ? last_output : nullptr);
    if (final_output != nullptr) {
        final_output->output = m_final_target;
        auto& mesh           = *(final_output->sceneNode->Mesh());
        auto& material       = *mesh.Material();
        material.blenmode    = m_final_blend;
        material.depth_test  = m_final_depth_test;
        material.depth_write = m_final_depth_write;
        material.cull_mode   = m_final_cull_mode;
        if (m_final_local) {
            final_output->sceneNode->SetCamera(std::string(effect_cam));
            final_output->sceneNode->SetParentAnchor(nullptr);
            final_output->sceneNode->CopyTrans(default_node);
            mesh.ChangeMeshDataFrom(default_mesh);
        } else if (fullscreen) {
            final_output->sceneNode->SetCamera(std::string(effect_cam));
            final_output->sceneNode->SetParentAnchor(nullptr);
            final_output->sceneNode->CopyTrans(default_node);
            mesh.ChangeMeshDataFrom(default_mesh);
        } else {
            const bool perspective = m_worldNode != nullptr && m_worldNode->Perspective();
            final_output->sceneNode->SetCamera(m_final_camera.empty()
                                                   ? (perspective ? "global_perspective" : "")
                                                   : m_final_camera);
            final_output->sceneNode->SetPerspective(perspective);
            // Anchor to the layer's primary SceneNode so the composite quad
            // inherits the layer's world transform (including any container
            // parent chain) via ModelTrans. Identity local — no CopyTrans dance.
            final_output->sceneNode->SetParentAnchor(m_worldNode);
            if (final_output->uses_unit_final_quad) {
                final_output->sceneNode->SetTranslate({ -m_width * 0.5f, -m_height * 0.5f, 0.0f });
                final_output->sceneNode->SetScale({ m_width, m_height, 1.0f });
                ChangeMeshToUnitQuad(mesh);
            } else {
                mesh.ChangeMeshDataFrom(*m_final_mesh.as_ptr());
            }
            final_output->final_quad_shader_values.iter().for_each([&](auto entry) {
                auto [name, value] = entry;
                material.SetShaderValue(rstd::cppstd::to_string(name->as_str()), value->base);
                if (value->curve.is_some() && ! (**value->curve).Empty()) {
                    (void)material.customShader.valueAnimations.insert(name->clone(),
                                                                       value->Clone());
                } else {
                    (void)material.customShader.valueAnimations.remove(name->as_str());
                }
            });
        }
        final_output->sceneNode->SetAlphaSource(m_worldNode);
    }
    m_resolved = true;
}
