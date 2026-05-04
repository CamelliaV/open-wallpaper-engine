#pragma once

#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"

namespace weweb {

class AppHandler : public CefApp,
                   public CefBrowserProcessHandler {
public:
    AppHandler();

    // CefApp.
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> cmd) override;

    // CefBrowserProcessHandler.
    void OnContextInitialized() override;

private:
    IMPLEMENT_REFCOUNTING(AppHandler);
    DISALLOW_COPY_AND_ASSIGN(AppHandler);
};

}  // namespace weweb
