#include "OsrRenderHandler.hpp"

#include <cstring>

namespace weweb {

void OsrRenderHandler::SetViewSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    std::lock_guard lk(mu_);
    view_w_ = width;
    view_h_ = height;
}

void OsrRenderHandler::GetViewRect(CefRefPtr<CefBrowser> /*browser*/,
                                   CefRect& rect) {
    std::lock_guard lk(mu_);
    rect.x = 0;
    rect.y = 0;
    rect.width  = view_w_;
    rect.height = view_h_;
}

bool OsrRenderHandler::GetScreenInfo(CefRefPtr<CefBrowser> /*browser*/,
                                     CefScreenInfo& info) {
    std::lock_guard lk(mu_);
    // Match the view; CEF uses this for window.screen.* and devicePixelRatio
    // bookkeeping. Single-monitor logical-pixel space is fine for v1.
    info.device_scale_factor = 1.0f;
    info.depth = 32;
    info.depth_per_component = 8;
    info.is_monochrome = false;
    info.rect.x = 0;
    info.rect.y = 0;
    info.rect.width  = view_w_;
    info.rect.height = view_h_;
    info.available_rect = info.rect;
    return true;
}

void OsrRenderHandler::OnPaint(CefRefPtr<CefBrowser> /*browser*/,
                               PaintElementType type,
                               const RectList& /*dirtyRects*/,
                               const void* buffer,
                               int width,
                               int height) {
    if (type != PET_VIEW) return;             // ignore popup widgets for v1
    if (!buffer || width <= 0 || height <= 0) return;

    const std::size_t bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;

    std::lock_guard lk(mu_);
    if (bgra_.size() != bytes) bgra_.resize(bytes);
    std::memcpy(bgra_.data(), buffer, bytes);
    width_  = width;
    height_ = height;
    fresh_  = true;
}

OsrRenderHandler::FrameLock OsrRenderHandler::LockLatestFrame() {
    mu_.lock();
    FrameLock lk;
    lk.pixels = bgra_.empty() ? nullptr : bgra_.data();
    lk.width  = width_;
    lk.height = height_;
    lk.fresh  = fresh_;
    fresh_ = false;
    return lk;
}

void OsrRenderHandler::UnlockLatestFrame() {
    mu_.unlock();
}

}  // namespace weweb
