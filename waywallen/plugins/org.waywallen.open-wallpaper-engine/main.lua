local source = import("wallpaper_engine.source")
local wallpaper = import("wallpaper_engine.wallpaper")

local M = {}

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
                    "Typically `~/.steam/steam` or `~/.local/share/Steam` " ..
                    "(or `~/.var/app/com.valvesoftware.Steam/data/Steam` for Flatpak Steam).",
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
