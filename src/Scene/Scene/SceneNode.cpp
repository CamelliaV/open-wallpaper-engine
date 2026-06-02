module;


module wescene.scene;
import eigen;
import rstd.cppstd;

using namespace owe;
using namespace Eigen;

Matrix4d SceneNode::GetLocalTrans() const {
    Affine3d trans = Affine3d::Identity();
    trans.prescale(m_scale.cast<double>());

    // m_rotation carries WE `angles` in degrees (static parse, JS setter, and
    // script actuators all feed degrees); AngleAxis wants radians.
    constexpr double kDegToRad = rstd::f64_::consts::PI / 180.0;
    trans.prerotate(AngleAxis<double>(m_rotation.x() * kDegToRad, Vector3d::UnitX())); // x
    trans.prerotate(AngleAxis<double>(m_rotation.y() * kDegToRad, Vector3d::UnitY())); // y
    trans.prerotate(AngleAxis<double>(m_rotation.z() * kDegToRad, Vector3d::UnitZ())); // z

    trans.pretranslate(m_translate.cast<double>());

    return trans.matrix();
}

void SceneNode::UpdateTrans() {
    if (! m_dirty) return;
    m_dirty = false;

    if (m_parent) {
        m_parent->UpdateTrans();
    }
    {
        Affine3d trans = Affine3d::Identity();
        if (m_parent) {
            trans *= m_parent->ModelTrans();
        }
        m_trans = (trans * GetLocalTrans()).matrix();
    }
}

void SceneNode::MarkTransDirty() {
    if (! m_dirty) {
        m_dirty = true;
        for (auto& child : m_children) {
            child->MarkTransDirty();
        }
    }
}

SceneNode* SceneNode::FindByName(std::string_view name) {
    if (m_name == name) return this;
    for (auto& child : m_children) {
        if (auto* hit = child->FindByName(name)) return hit;
    }
    return nullptr;
}
