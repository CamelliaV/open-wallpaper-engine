export module waywallen.bridge_session;

import rstd.cppstd;
export import waywallen.bridge;

export namespace ww_wescene
{

class BridgeSession final {
public:
    static std::shared_ptr<BridgeSession> Adopt(ww_pool_t* pool, int control_socket);

    ~BridgeSession();

    BridgeSession(const BridgeSession&)            = delete;
    BridgeSession& operator=(const BridgeSession&) = delete;

    bool valid() const noexcept { return m_pool != nullptr && m_send_socket >= 0; }

    int advertiseCaps(uint32_t width, uint32_t height, uint32_t mem_hints);
    int applyDirective(const ww_pool_directive_t& directive);
    int acquireSlot(uint32_t slot_index, ww_pool_slot_t& out_slot);
    int acquireSlotForRender(uint32_t slot_index, uint32_t timeout_ms,
                             ww_pool_slot_acquire_result_t& out_result);
    int submitSlotForRender(uint32_t slot_index, int producer_sync_fd,
                            ww_pool_slot_submit_result_t& out_result);
    int reportRelease(const ww_evt_in_release_resolved_t& release);
    int sendBindFailed(uint32_t fourcc, uint64_t modifier, uint32_t reason, const char* message);
    int sendClearColor(float r, float g, float b, float a);

private:
    BridgeSession(ww_pool_t* pool, int send_socket): m_pool(pool), m_send_socket(send_socket) {}

    ww_pool_t* m_pool { nullptr };
    int        m_send_socket { -1 };
    std::mutex m_send_mutex;
};

} // namespace ww_wescene
