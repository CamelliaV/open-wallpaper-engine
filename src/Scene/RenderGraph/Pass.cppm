export module wescene.rgraph:pass;
import rstd;

using namespace rstd::prelude;

export namespace owe::rg
{

struct PassHandle {
    usize index { numeric_limits<usize>::max() };

    bool valid() const noexcept { return index != numeric_limits<usize>::max(); }

    friend auto operator<=>(const PassHandle&, const PassHandle&) = default;
};

struct PassHandleHasher {
    rstd::hash::RandomState state;

    auto operator()(PassHandle handle) const noexcept -> rstd::u64 { return state(handle.index); }
};

struct Pass {
    Pass()          = default;
    virtual ~Pass() = default;

    Pass(const Pass&)            = delete;
    Pass& operator=(const Pass&) = delete;
};

struct VirtualPass : Pass {
    struct Desc {};

    explicit VirtualPass(Desc&&) noexcept {}
    ~VirtualPass() noexcept override = default;
};

} // namespace owe::rg
