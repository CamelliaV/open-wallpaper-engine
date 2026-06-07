#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "include/cef_browser.h"

namespace weweb
{

// Build the JS payload that drives Wallpaper Engine's
// `window.wallpaperPropertyListener.applyUserProperties()` hook on initial
// page load. `props` is the parsed `project.json:general.properties`
// object (may be null/empty).
std::string BuildPropertyListenerSnippet(const nlohmann::json& props);

// Inject the snippet into the page's main frame.
void InjectUserProperties(CefRefPtr<CefBrowser> browser, const nlohmann::json& props);

} // namespace weweb
