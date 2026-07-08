module;

#include <cmath>

#include <rstd/macro.hpp>
module wescene.scene;
import eigen;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.utils;

using namespace owe;
using namespace Eigen;

namespace
{
Matrix4d NodeCameraFrame(SceneNode& node) {
    node.UpdateTrans();

    Matrix4d frame = node.ModelTrans();
    if (! frame.allFinite()) return Matrix4d::Identity();

    constexpr double kAxisEps = 1e-10;
    Vector3d         x        = frame.block<3, 1>(0, 0);
    Vector3d         y        = frame.block<3, 1>(0, 1);
    Vector3d         z        = frame.block<3, 1>(0, 2);

    if (! z.allFinite() || z.squaredNorm() <= kAxisEps) {
        z = x.cross(y);
        if (! z.allFinite() || z.squaredNorm() <= kAxisEps) z = Vector3d::UnitZ();
        frame.block<3, 1>(0, 2) = z.normalized();
    }

    if (! frame.allFinite() || std::abs(frame.determinant()) <= kAxisEps)
        return Matrix4d::Identity();
    return frame;
}
} // namespace

Vector3d SceneCamera::GetPosition() const {
    if (m_lookat) return m_eye;
    if (m_node) {
        return Affine3d(m_node->GetLocalTrans()) * Vector3d::Zero();
    }
    return Vector3d::Zero();
}

Vector3d SceneCamera::GetDirection() const {
    if (m_lookat) return (m_center - m_eye).normalized();
    if (m_node) {
        return (m_node->GetLocalTrans() * Vector4d(0.0f, 0.0f, -1.0f, 0.0f)).head<3>();
    }
    return -Vector3d::UnitZ();
}

Matrix4d SceneCamera::GetViewMatrix() {
    CalculateViewProjectionMatrix();
    return m_viewMat;
}

Matrix4d SceneCamera::GetViewProjectionMatrix() {
    CalculateViewProjectionMatrix();
    return m_viewProjectionMat;
}

void SceneCamera::CalculateViewProjectionMatrix() {
    if (m_lookat) {
        m_viewMat = LookAt(m_eye, m_center, m_up);
    } else if (m_node) {
        // view = inv(node.ModelTrans()) so the layer-local frame maps to
        // view origin regardless of where the node sits in the world (parent
        // chain + local translate / scale / rotate). With LookAt-only the
        // node's local scale would leak into clip space and a 9× scaled
        // layer would only see 1/9 of its quad inside the ortho viewport.
        m_viewMat = NodeCameraFrame(*m_node).inverse();
    } else
        m_viewMat = Matrix4d::Identity();

    if (m_perspective) {
        m_viewProjectionMat =
            Perspective(Radians(m_fov), m_aspect, m_nearClip, m_farClip) * m_viewMat;
    } else {
        double left         = -m_width / 2.0f;
        double right        = m_width / 2.0f;
        double bottom       = -m_height / 2.0f;
        double up           = m_height / 2.0f;
        m_viewProjectionMat = Ortho(left, right, bottom, up, m_nearClip, m_farClip) * m_viewMat;
    }
}

void SceneCamera::Update() { CalculateViewProjectionMatrix(); }

void SceneCamera::AttatchNode(SceneNode* node) {
    if (! node) {
        rstd_error("Attach a null node to camera");
        return;
    }
    m_node   = node;
    m_lookat = false; // node-based view takes over from any explicit LookAt
}
