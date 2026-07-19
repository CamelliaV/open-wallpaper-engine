local source = import("wallpaper_engine.source")
local wallpaper = import("wallpaper_engine.wallpaper")
local discover = import("wallpaper_engine.discover")
local api = import("wallpaper_engine.api")

local M = {}

local function expand_home(path)
    local getenv = os and os.getenv
    local home = getenv and getenv("HOME") or nil
    if type(home) == "string" and home ~= "" and string.sub(path, 1, 1) == "~" then
        return home .. string.sub(path, 2)
    end
    return path
end

function M.info()
    return {
        name = "wallpaper_engine",
        display_name = "Steam Workshop",
        status = {
            { id = "steam_account", label = "Status", group = "Steam account", order = 20 },
        },
        actions = {
            { id = "steam_sign_in", label = "Sign in to Steam", group = "Steam account", order = 21 },
            { id = "steam_sign_out", label = "Sign out", group = "Steam account", order = 22 },
        },
        capabilities = {
            source = {
                types = { "scene", "video", "web" },
                scan = true,
                auto_detect = true,
                library_label = "Steam Library Path",
                library_hint =
                    "Pick the directory that contains the `steamapps` folder.\n" ..
                    "Typically `" .. expand_home("~/.steam/steam") .. "` or `" ..
                    expand_home("~/.local/share/Steam") .. "` " ..
                    "(or `" .. expand_home("~/.var/app/com.valvesoftware.Steam/data/Steam") ..
                    "` for Flatpak Steam).",
            },
            discover = {
                search = true,
                details = true,
                download = true,
                resolve = true,
                sorts = {
                    { key = "trend_day", label = "Trending today" },
                    { key = "trend_week", label = "Trending this week" },
                    { key = "trend_month", label = "Trending this month" },
                    { key = "trend_3months", label = "Trending 3 months" },
                    { key = "trend_6months", label = "Trending 6 months" },
                    { key = "trend_year", label = "Trending this year" },
                    { key = "recent", label = "Most recent" },
                    { key = "most_subscribed", label = "Most subscribed" },
                    { key = "top_rated", label = "Top rated" },
                },
                tags = api.tags,
            },
            wallpaper = {
                properties = true,
                extras = true,
            },
        },
    }
end

M.source = source
M.wallpaper = wallpaper
M.discover = discover

return M
