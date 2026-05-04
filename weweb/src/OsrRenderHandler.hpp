#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "include/cef_render_handler.h"

namespace weweb {

// CefRenderHandler that buffers CEF's BGRA8 OSR output into a CPU bitmap.
// CEF invokes OnPaint on the UI thread (the same thread that pumps
// CefDoMessageLoopWork in single-threaded mode). The viewer's render
// thread reads the bitmap by calling LockLatestFrame / UnlockLatestFrame
// — protected by a mutex because in practice both run on the main thread
// already, but the handler is small enough that the cost is irrelevant
// and we keep it correct under any threading model.
class OsrRenderHandler : public CefRenderHandler {
public:
    OsrRenderHandler() = default;

    // The viewer sets the logical size before CEF first calls GetViewRect
    // and updates it whenever the GLFW window resizes.
    void SetViewSize(int width, int height);

    // CefRenderHandler.
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser>      browser,
                 PaintElementType           type,
                 const RectList&            dirtyRects,
                 const void*                buffer,
                 int                        width,
                 int                        height) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser,
                       CefScreenInfo& info) override;

    // Returns the most recently received frame's dimensions and BGRA bytes.
    // The pointer remains valid until UnlockLatestFrame() is called. The
    // bool out-param tells the caller whether the frame is fresh (i.e.
    // changed since the last lock); the caller can skip GPU upload on
    // stale frames.
    struct FrameLock {
        const std::uint8_t* pixels{nullptr};   // BGRA8, row-tight
        int                 width{0};
        int                 height{0};
        bool                fresh{false};
    };
    FrameLock LockLatestFrame();
    void      UnlockLatestFrame();

private:
    std::mutex            mu_;
    std::vector<std::uint8_t> bgra_;            // size = w*h*4
    int                   width_   {0};
    int                   height_  {0};
    bool                  fresh_   {false};
    int                   view_w_  {1280};
    int                   view_h_  {720};

    IMPLEMENT_REFCOUNTING(OsrRenderHandler);
    DISALLOW_COPY_AND_ASSIGN(OsrRenderHandler);
};

}  // namespace weweb
