module;

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

module waywallen.bridge_session;

import rstd.cppstd;
import waywallen.bridge;

namespace ww_wescene
{

std::shared_ptr<BridgeSession> BridgeSession::Adopt(ww_pool_t* pool, int control_socket) {
    if (pool == nullptr || control_socket < 0) return {};
    int send_socket = ::fcntl(control_socket, F_DUPFD_CLOEXEC, 0);
    if (send_socket < 0) return {};
    return std::shared_ptr<BridgeSession>(new BridgeSession(pool, send_socket));
}

BridgeSession::~BridgeSession() {
    if (m_pool != nullptr) ww_bridge_pool_destroy(m_pool);
    if (m_send_socket >= 0) ::close(m_send_socket);
}

int BridgeSession::advertiseCaps(uint32_t width, uint32_t height, uint32_t mem_hints) {
    std::scoped_lock lock(m_send_mutex);
    return ww_bridge_pool_advertise_caps(m_pool, m_send_socket, width, height, mem_hints);
}

int BridgeSession::applyDirective(const ww_pool_directive_t& directive) {
    std::scoped_lock lock(m_send_mutex);
    return ww_bridge_pool_apply_directive(m_pool, m_send_socket, &directive);
}

int BridgeSession::acquireSlot(uint32_t slot_index, ww_pool_slot_t& out_slot) {
    return ww_bridge_pool_acquire_slot(m_pool, slot_index, &out_slot);
}

int BridgeSession::acquireSlotForRender(uint32_t slot_index, uint32_t timeout_ms,
                                        ww_pool_slot_acquire_result_t& out_result) {
    return ww_bridge_pool_acquire_slot_for_render(m_pool, slot_index, timeout_ms, &out_result);
}

int BridgeSession::submitSlotForRender(uint32_t slot_index, int producer_sync_fd,
                                       ww_pool_slot_submit_result_t& out_result) {
    std::scoped_lock lock(m_send_mutex);
    return ww_bridge_pool_submit_slot_for_render(
        m_pool, m_send_socket, slot_index, producer_sync_fd, &out_result);
}

int BridgeSession::reportRelease(const ww_evt_in_release_resolved_t& release) {
    if (release.outcome > static_cast<uint32_t>(WW_POOL_RELEASE_FORCED)) return -EPROTO;
    return ww_bridge_pool_report_release(m_pool,
                                         release.buffer_generation,
                                         release.buffer_index,
                                         release.release_point,
                                         static_cast<ww_pool_release_outcome_t>(release.outcome));
}

int BridgeSession::sendBindFailed(uint32_t fourcc, uint64_t modifier, uint32_t reason,
                                  const char* message) {
    std::scoped_lock lock(m_send_mutex);
    return ww_bridge_send_bind_failed(m_send_socket, fourcc, modifier, reason, message);
}

int BridgeSession::sendClearColor(float r, float g, float b, float a) {
    std::scoped_lock lock(m_send_mutex);
    return ww_bridge_send_report_state_clear_color(m_send_socket, r, g, b, a);
}

} // namespace ww_wescene
