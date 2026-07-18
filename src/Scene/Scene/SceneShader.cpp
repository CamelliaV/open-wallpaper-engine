module;

module wescene.scene;
import rstd.cppstd;

using namespace owe;

void ShaderValue::fromSpan(std::span<const value_type> values) noexcept {
    m_size    = static_cast<usize>(values.size());
    m_dynamic = values.size() > m_value.len();
    if (m_dynamic) {
        m_dynamic_value.resize(m_size);
        std::copy(values.begin(), values.end(), m_dynamic_value.begin());
    } else
        std::copy(values.begin(), values.end(), m_value.begin());
}
