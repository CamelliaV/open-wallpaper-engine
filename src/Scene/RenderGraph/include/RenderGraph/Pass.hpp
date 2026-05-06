#pragma once

namespace wallpaper
{
namespace rg
{

class Pass {
public:
    Pass()          = default;
    virtual ~Pass() = default;

    Pass(const Pass&)            = delete;
    Pass& operator=(const Pass&) = delete;
};

class VirtualPass : public Pass {
public:
    struct Desc {};
    VirtualPass(const Desc&) noexcept {};
    virtual ~VirtualPass() noexcept {};
};
} // namespace rg
} // namespace wallpaper
