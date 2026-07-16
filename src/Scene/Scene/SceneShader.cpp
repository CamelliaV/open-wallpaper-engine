module;

module wescene.scene;
import rstd.cppstd;

using namespace owe;

void ShaderValue::fromSpan(std::span<const value_type> s) noexcept {
    m_size    = static_cast<usize>(s.size());
    m_dynamic = s.size() > m_value.len();
    if (m_dynamic) {
        m_dvalue.resize(m_size);
        std::copy(s.begin(), s.end(), m_dvalue.begin());
    } else
        std::copy(s.begin(), s.end(), m_value.begin());
}
