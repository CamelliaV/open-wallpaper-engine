#pragma once

#include <cstdint>
#include <functional>

namespace weweb {

// One DMA-BUF plane as delivered by CEF's OnAcceleratedPaint. The `fd` is
// borrowed: it is only valid for the lifetime of the synchronous
// AcceleratedPaintCallback invocation; CEF reclaims the underlying buffer
// when the callback returns. Receivers that need to keep the FD must
// dup() it.
struct DmaBufPlane {
    int      fd      {-1};
    uint32_t stride  {0};
    uint64_t offset  {0};
    uint64_t size    {0};
};

enum class DmaBufFormat : int {
    BGRA8_UNORM = 0,    // CEF_COLOR_TYPE_BGRA_8888 — chunked layout B,G,R,A
    RGBA8_UNORM = 1,    // CEF_COLOR_TYPE_RGBA_8888
};

// Snapshot of a CEF accelerated-paint frame, expressed without any CEF
// types so the viewer doesn't need CEF headers.
//
// `modifier` is the DRM format modifier; `0x00ffffffffffffff` (= -1 in
// signed 64-bit / `DRM_FORMAT_MOD_INVALID`) means the producer did not
// pick a modifier explicitly — for the cases we observe from CEF this
// is functionally equivalent to `DRM_FORMAT_MOD_LINEAR` (stride matches
// width*bpp exactly, no tiling padding).
struct DmaBufFrame {
    DmaBufPlane  planes[4]   {};
    int          plane_count {0};
    uint64_t     modifier    {0};
    DmaBufFormat format      {DmaBufFormat::BGRA8_UNORM};

    // Dimensions of the underlying GPU buffer (= "coded size").
    int          coded_width  {0};
    int          coded_height {0};
    // Visible portion (typically equals coded for our use case).
    int          visible_width  {0};
    int          visible_height {0};
};

// Synchronous receiver for accelerated-paint frames. Defined here so
// both internal (OsrRenderHandler) and public (BrowserHost) headers can
// reference one canonical type.
using AcceleratedPaintCallback = std::function<void(const DmaBufFrame&)>;

}  // namespace weweb
