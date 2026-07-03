local source = import("wallpaper_engine.source")
local wallpaper = import("wallpaper_engine.wallpaper")

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
            wallpaper = {
                properties = true,
                extras = true,
            },
        },
    }
end

M.source = source
M.wallpaper = wallpaper

return M
