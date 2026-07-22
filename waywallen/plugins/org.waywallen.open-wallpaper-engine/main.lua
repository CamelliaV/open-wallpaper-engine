local source = import("wallpaper_engine.source")
local wallpaper = import("wallpaper_engine.wallpaper")
local discover = import("wallpaper_engine.discover")
local api = import("wallpaper_engine.api")
local auth = import("wallpaper_engine.auth")
local session = import("wallpaper_engine.session")
local subscription = import("wallpaper_engine.subscription")

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
        display_name = "Workshop",
        status = {
            { id = "steam_account", label = "Status", group = "Steam account", order = 20 },
        },
        actions = {
            {
                id = "steam_sign_in",
                kind = "qr_login",
                label = "Log in to Steam",
                browse_button_label = "Log in to Steam",
                browse_description =
                    "Waywallen only manages Workshop subscriptions and does not download " ..
                    "wallpapers. Keep the Steam desktop client running to download subscribed items.",
                group = "Steam account",
                order = 21,
                required_for_browsing = true,
            },
            {
                id = "steam_sign_out",
                kind = "invoke",
                label = "Sign out",
                group = "Steam account",
                order = 22,
            },
        },
        state_migrations = {
            { schema_id = "waywallen-steam-session-v1", file = "steam-session.json" },
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
                subscription = true,
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
M.subscription = subscription

M.lifecycle = {}
function M.lifecycle.load(blob)
    session.load(blob)
end
function M.lifecycle.save()
    return session.save()
end
function M.lifecycle.check(ctx)
    return session.check(ctx)
end
function M.lifecycle.migrate(schema_id, raw)
    return session.migrate(schema_id, raw)
end

M.actions = {}
function M.actions.status(ctx)
    local checked = session.current_check()
    local active = checked.state == "signed_in"
    return {
        status = { steam_account = checked.display_value or "" },
        actions = {
            steam_sign_in = { visible = not active, enabled = not active },
            steam_sign_out = { visible = session.signed_in(), enabled = true },
        },
    }
end
function M.actions.invoke(ctx, action_id)
    if action_id ~= "steam_sign_out" then
        error("unsupported Steam action")
    end
    session.sign_out(ctx)
    subscription.clear()
end

M.qrlogin = {}
function M.qrlogin.begin(ctx, action_id)
    if action_id ~= "steam_sign_in" then
        error("unsupported Steam QR action")
    end
    return auth.begin(ctx)
end
M.qrlogin.poll = auth.poll
M.qrlogin.cancel = auth.cancel

return M
