module;

#include <nlohmann/json.hpp>

module weweb;

import rstd.cppstd;

import :cef;
import :cef_internal;

namespace weweb
{

ClientHandler::ClientHandler(nlohmann::json user_props, CefRefPtr<OsrRenderHandler> render_handler)
    : user_props_(std::move(user_props)), render_handler_(std::move(render_handler)) {}

void ClientHandler::SetCloseCallback(std::function<void()> cb) { close_cb_ = std::move(cb); }

void ClientHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) { browser_ = browser; }

bool ClientHandler::DoClose(CefRefPtr<CefBrowser> /*browser*/) {
    return false; // proceed with the default close
}

void ClientHandler::OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) {
    browser_ = nullptr;
    if (close_cb_) close_cb_();
}

void ClientHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                              int /*httpStatusCode*/) {
    if (! frame || ! frame->IsMain()) return;
    bool expected = false;
    if (! property_injected_.compare_exchange_strong(expected, true)) return;
    InjectUserProperties(browser, user_props_);
}

bool ClientHandler::OnConsoleMessage(CefRefPtr<CefBrowser> /*browser*/, cef_log_severity_t level,
                                     const CefString& message, const CefString& source, int line) {
    const char* level_str = "info";
    // LOGSEVERITY_VERBOSE and LOGSEVERITY_DEBUG share the same numeric
    // value in CEF; only one needs a case label.
    switch (level) {
    case LOGSEVERITY_DEBUG: level_str = "debug"; break;
    case LOGSEVERITY_INFO: level_str = "info"; break;
    case LOGSEVERITY_WARNING: level_str = "warn"; break;
    case LOGSEVERITY_ERROR: level_str = "error"; break;
    case LOGSEVERITY_FATAL: level_str = "fatal"; break;
    default: break;
    }
    std::fprintf(stderr,
                 "weweb [%s] %s (%s:%d)\n",
                 level_str,
                 message.ToString().c_str(),
                 source.ToString().c_str(),
                 line);
    return false; // also let CEF's default handler log
}

} // namespace weweb
