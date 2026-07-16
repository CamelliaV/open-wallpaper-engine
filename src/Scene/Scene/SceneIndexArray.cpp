module;

module wescene.scene;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace owe;

SceneIndexArray::SceneIndexArray(usize index_count): m_data(Vec<u32>::with_capacity(index_count)) {
    for (usize i = 0; i < index_count; ++i) m_data.push(0);
}
SceneIndexArray::SceneIndexArray(std::span<const u32> data)
    : m_data(Vec<u32>::with_capacity(data.size())), m_size(data.size()) {
    for (u32 value : data) m_data.push(rstd::move(value));
}

SceneIndexArray::SceneIndexArray(SceneIndexArray&& other) noexcept
    : m_data(rstd::move(other.m_data)),
      m_size(other.m_size),
      m_render_size(other.m_render_size),
      m_id(other.m_id),
      m_generation(other.m_generation) {}

bool SceneIndexArray::IncreaseCheckSet(usize nsize) {
    if (nsize > CapacitySizeof()) return false;
    if (nsize > DataSizeOf()) {
        m_size = nsize / Unit_Byte_Size + (nsize % Unit_Byte_Size == 0 ? 0 : 1);
    }
    return true;
}
