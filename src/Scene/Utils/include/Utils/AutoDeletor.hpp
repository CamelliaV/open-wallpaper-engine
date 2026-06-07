#pragma once
#include <utility>

#define AUTO_DELETER(name, del_func) owe::AutoDeleter del_##name(del_func);

namespace owe
{
template<typename TDeleter>
struct AutoDeleter {
    AutoDeleter(TDeleter&& del): m_del(std::move(del)) {}
    ~AutoDeleter() { m_del(); }
    TDeleter m_del;
};
} // namespace owe