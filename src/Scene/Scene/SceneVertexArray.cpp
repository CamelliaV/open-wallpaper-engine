module;

#include <rstd/macro.hpp>

module wescene.scene;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace owe;

u8 SceneVertexArray::TypeCount(VertexType t) {
    switch (t) {
    case VertexType::FLOAT1:
    case VertexType::UINT1: return 1;
    case VertexType::FLOAT2:
    case VertexType::UINT2: return 2;
    case VertexType::FLOAT3:
    case VertexType::UINT3: return 3;
    case VertexType::FLOAT4:
    case VertexType::UINT4: return 4;
    }
    return 1;
}

u8 SceneVertexArray::RealAttributeSize(const SceneVertexArray::SceneVertexAttribute& attr) {
    return attr.padding ? 4 : TypeCount(attr.type);
}

SceneVertexArray::SceneVertexArray(const std::vector<SceneVertexAttribute>& attrs,
                                   const usize                              count)
    : m_attributes(attrs) {
    for (const auto& el : m_attributes) {
        usize size = SceneVertexArray::RealAttributeSize(el);
        m_oneSize += size;
    }
    auto capacity = m_oneSize * count;
    m_data        = Vec<float>::with_capacity(capacity);
    for (usize i = 0; i < capacity; ++i) m_data.push(0.0f);
}

SceneVertexArray::SceneVertexArray(SceneVertexArray&& other) noexcept
    : m_attributes(rstd::move(other.m_attributes)),
      m_options(rstd::move(other.m_options)),
      m_data(rstd::move(other.m_data)),
      m_oneSize(other.m_oneSize),
      m_size(other.m_size),
      m_id(other.m_id),
      m_generation(other.m_generation) {}

SceneVertexArray& SceneVertexArray::operator=(SceneVertexArray&& other) noexcept {
    if (this == &other) return *this;
    m_attributes = rstd::move(other.m_attributes);
    m_options    = rstd::move(other.m_options);
    m_data       = rstd::move(other.m_data);
    m_oneSize    = other.m_oneSize;
    m_size       = other.m_size;
    m_id         = other.m_id;
    m_generation = other.m_generation;
    return *this;
}

bool SceneVertexArray::AddVertex(const float* data) {
    if (m_size + m_oneSize > m_data.len()) return false;
    usize  pos   = 0;
    usize  mpos  = 0;
    float* mData = m_data.begin() + m_size;
    for (const auto& el : m_attributes) {
        auto typeSize = SceneVertexArray::TypeCount(el.type);
        std::copy(data + pos, data + pos + typeSize, mData + mpos);
        pos += typeSize;
        mpos += SceneVertexArray::RealAttributeSize(el);
    }
    m_size += m_oneSize;
    BumpDataGeneration();
    return true;
}

bool SceneVertexArray::SetVertex(std::string_view name, std::span<const float> data) noexcept {
    u32 offset = 0;
    for (const auto& el : m_attributes) {
        if (el.name == name) {
            usize typeSize = SceneVertexArray::TypeCount(el.type);
            usize count    = data.size() / typeSize;
            if (! TrySetSize(count * m_oneSize)) return false;

            for (usize i = 0; i < data.size(); i += typeSize) {
                auto  start = data.begin() + (isize)i;
                usize num   = i / typeSize;
                std::copy(
                    start, start + (isize)typeSize, m_data.begin() + offset + num * m_oneSize);
            }
            BumpDataGeneration();
            return true;
        } else
            offset += RealAttributeSize(el);
    }
    return false;
}

bool SceneVertexArray::SetVertexs(usize index, std::span<const float> data) noexcept {
    usize start = index * m_oneSize;
    if (TrySetSize(start + data.size())) {
        std::copy(data.begin(), data.end(), m_data.begin() + start);
        BumpDataGeneration();
        return true;
    }
    return false;
}

void SceneVertexArray::ResetSize() noexcept {
    if (m_size == 0) return;
    m_size = 0;
    BumpDataGeneration();
}

bool SceneVertexArray::TrySetSize(usize new_size) noexcept {
    rstd_assert(new_size <= m_data.len());
    if (new_size > m_data.len()) {
        return false;
    }
    if (new_size > m_size) m_size = new_size;
    return true;
}

Map<std::string, SceneVertexArray::SceneVertexAttributeOffset>
SceneVertexArray::GetAttrOffsetMap() const {
    Map<std::string, SceneVertexArray::SceneVertexAttributeOffset> result;
    usize                                                          offset { 0 };
    for (const auto& attr : m_attributes) {
        result[attr.name] = (SceneVertexAttributeOffset { .attr = attr, .offset = offset });
        offset += SceneVertexArray::RealAttributeSize(attr) * sizeof(float);
    }
    return result;
}

bool SceneVertexArray::GetOption(std::string_view name) const {
    return exists(m_options, name) && m_options.at(std::string(name));
}
void SceneVertexArray::SetOption(std::string_view name, bool value) {
    m_options[std::string(name)] = value;
}
