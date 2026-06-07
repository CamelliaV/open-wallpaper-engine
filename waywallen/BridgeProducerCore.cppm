// BridgeProducerCore — bridge-protocol state shared by every waywallen
// renderer subprocess that pushes Vulkan-backed DMA-BUF slots through a
// `ww_pool_t`.
//
// Owns:
//   - the bridge `ww_pool_t*` and control socket fd (caller-owned;
//     core only borrows them).
//   - the pending `ww_pool_directive_t` stash + slot bookkeeping
//     (slot count / next slot / negotiated geometry / fourcc /
//     resolved VkFormat).
//   - on-first-negotiated and on-ready-changed callbacks.
//
// What it does NOT do:
//   - any Vulkan command recording (the producer host owns the
//     VkDevice and submits the blit).
//   - any inheritance from `owe::ExSwapchain` — that adapter is
//     `BridgeExSwapchain`, which is a thin shim around this core for
//     the wescene host. Other producers (e.g. weweb) consume this core
//     directly without pulling wescene-vulkan-runtime.
//
// Threading model is identical to BridgeExSwapchain's:
//   - `queueDirective` is the only multi-threaded entry point.
//   - `drainPendingDirective` / `acquireSlot` / `submitSlot` are
//     producer-thread-only (the thread that submits the blit).

module;

#include <waywallen-bridge/bridge.h>
#include <waywallen-bridge/pool.h>

export module waywallen.bridge_producer_core;

import rstd.cppstd;
import vulkan;

export namespace ww_wescene
{

// Snapshot fired by `setOnReadyChanged` whenever applyDirective resolves
// a new slot pool. Mirrors `owe::ExSwapchainReadyEvent` shape but
// kept Vulkan-native and free of any wescene types so non-wescene
// producers can consume it.
struct BridgeReadyEvent {
    bool     ready;
    uint32_t width;
    uint32_t height;
    VkFormat format;
};

class BridgeProducerCore {
public:
    static constexpr uint32_t kMaxSlots = 8; // matches bridge cap

    // `pool` and `sock` are caller-owned; the same (pool, sock) pair
    // must outlive this core.
    BridgeProducerCore(ww_pool_t* pool, int sock);
    ~BridgeProducerCore();

    BridgeProducerCore(const BridgeProducerCore&)            = delete;
    BridgeProducerCore& operator=(const BridgeProducerCore&) = delete;

    // Stash a directive received from the daemon. Safe to call from any
    // thread — only the producer-thread `drainPendingDirective()` ever
    // reads the stash. If a directive was already pending, it is
    // dropped (the newer directive supersedes it; the daemon's older
    // negotiation is implicitly abandoned).
    void queueDirective(const ww_pool_directive_t& directive);

    // True iff a queued directive is waiting to be applied.
    bool hasPendingDirective() const { return m_pending_valid.load(std::memory_order_acquire); }

    // One-shot callback fired from the producer thread the first time a
    // directive is successfully applied. Setting after the first
    // negotiate is a no-op.
    void setOnFirstNegotiated(std::function<void()> cb) {
        std::lock_guard<std::mutex> lk(m_cb_mu);
        m_on_first_negotiated = std::move(cb);
    }

    // Fired from the producer thread inside drainPendingDirective after
    // every successful apply. Coexists with setOnFirstNegotiated; both
    // fire in undefined order.
    void setOnReadyChanged(std::function<void(const BridgeReadyEvent&)> cb) {
        std::lock_guard<std::mutex> lk(m_cb_mu);
        m_on_ready_changed = std::move(cb);
    }

    // Producer-thread-only. Drain any pending directive so `format()` /
    // `ready()` reflect the new state before the caller decides whether
    // to acquire a slot.
    void drainPendingDirective();

    // Producer-thread-only. Returns the next slot's VkImage handle and
    // (optionally) its dimensions. Returns false when the pool has no
    // slots applied yet.
    bool acquireSlot(VkImage* out_image, uint32_t* out_width = nullptr,
                     uint32_t* out_height = nullptr);

    // Producer-thread-only. Forwards `producer_sync_fd` to
    // `ww_bridge_pool_submit_slot`. Bridge takes ownership of the fd
    // and closes it. Pass -1 only on shutdown.
    void submitSlot(int producer_sync_fd);

    // Geometry / readiness accessors — readable from any thread, but
    // they can only update on the producer thread, so the caller is
    // responsible for any happens-before they need (typical pattern:
    // call from inside the producer thread right after
    // drainPendingDirective).
    bool     ready() const { return m_slot_count > 0 && m_export_format != VK_FORMAT_UNDEFINED; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    VkFormat format() const { return m_export_format; }
    uint32_t fourcc() const { return m_fourcc; }

private:
    // Producer-thread-only. Runs the bridge's apply_directive,
    // publishes new geometry/format on success. Returns:
    //   0   success — m_slot_count == directive.count
    //  <0   dry-run failure (bridge already emitted bind_failed); caller
    //       leaves m_slot_count = 0 and waits for the next directive
    //  >0   hard system error (logged); m_slot_count = 0
    int applyDirective(const ww_pool_directive_t& directive);

    // Caller-owned bridge handles.
    ww_pool_t* m_pool { nullptr };
    int        m_sock { -1 };

    VkFormat m_export_format { VK_FORMAT_UNDEFINED };

    std::atomic<bool>   m_pending_valid { false };
    std::mutex          m_pending_mu;
    ww_pool_directive_t m_pending_directive {};

    std::mutex                                   m_cb_mu;
    std::function<void()>                        m_on_first_negotiated;
    bool                                         m_first_negotiated_done { false };
    std::function<void(const BridgeReadyEvent&)> m_on_ready_changed;

    uint32_t m_slot_count { 0 };
    uint32_t m_next_slot { 0 };
    uint32_t m_pending_slot { 0 };
    bool     m_have_pending { false };
    uint32_t m_width { 0 };
    uint32_t m_height { 0 };
    uint32_t m_fourcc { 0 };
};

} // namespace ww_wescene
