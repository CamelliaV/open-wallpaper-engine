#pragma once

#include <atomic>
#include <functional>

#include <nlohmann/json.hpp>

#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"

namespace weweb {

class ClientHandler : public CefClient,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler {
public:
    explicit ClientHandler(nlohmann::json user_props);

    // Called from BrowserHost — fired once when the browser closes.
    void SetCloseCallback(std::function<void()> cb);

    // CefClient.
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler()     override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler()  override { return this; }

    // CefLifeSpanHandler.
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // CefLoadHandler.
    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override;

    // CefDisplayHandler.
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override;

private:
    nlohmann::json user_props_;
    std::function<void()> close_cb_;
    std::atomic<bool> property_injected_{false};

    IMPLEMENT_REFCOUNTING(ClientHandler);
    DISALLOW_COPY_AND_ASSIGN(ClientHandler);
};

}  // namespace weweb
