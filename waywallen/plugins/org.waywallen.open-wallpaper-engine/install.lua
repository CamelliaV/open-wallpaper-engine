local plugin = "share/waywallen/plugins/org.waywallen.open-wallpaper-engine"
local scene_bin = "../../../../bin/waywallen-wescene-renderer"
local web_bin = "../../../../bin/weweb/waywallen-weweb-renderer"

local web_block = lito.render_template({
    input = "weweb-renderer.toml.in",
    values = {
        OWE_WEWEB_BIN = web_bin,
    },
})

lito.install({
    files = {
        { source = "main.lua", destination = plugin .. "/main.lua" },
        { source = "wallpaper_engine/api.lua", destination = plugin .. "/wallpaper_engine/api.lua" },
        { source = "wallpaper_engine/auth.lua", destination = plugin .. "/wallpaper_engine/auth.lua" },
        { source = "wallpaper_engine/discover.lua", destination = plugin .. "/wallpaper_engine/discover.lua" },
        { source = "wallpaper_engine/map.lua", destination = plugin .. "/wallpaper_engine/map.lua" },
        { source = "wallpaper_engine/profile.lua", destination = plugin .. "/wallpaper_engine/profile.lua" },
        { source = "wallpaper_engine/project.lua", destination = plugin .. "/wallpaper_engine/project.lua" },
        { source = "wallpaper_engine/session.lua", destination = plugin .. "/wallpaper_engine/session.lua" },
        { source = "wallpaper_engine/source.lua", destination = plugin .. "/wallpaper_engine/source.lua" },
        { source = "wallpaper_engine/subscription.lua", destination = plugin .. "/wallpaper_engine/subscription.lua" },
        { source = "wallpaper_engine/wallpaper.lua", destination = plugin .. "/wallpaper_engine/wallpaper.lua" },
        { source = "wallpaper_engine/workshop.lua", destination = plugin .. "/wallpaper_engine/workshop.lua" },
    },
    templates = {
        {
            input = "plugin.toml.in",
            destination = plugin .. "/plugin.toml",
            values = {
                OWE_WAYWALLEN_PLUGIN_ID = "org.waywallen.open-wallpaper-engine",
                OWE_PLUGIN_VERSION = lito.package_version,
                OWE_WAYWALLEN_PLUGIN_UPDATE_URL = "https://github.com/waywallen/open-wallpaper-engine/raw/refs/heads/main/update.json",
                OWE_WESCENE_BIN = scene_bin,
                OWE_WEWEB_BLOCK = web_block,
            },
        },
    },
    inventories = {
        {
            destination = plugin .. "/files.txt",
            relative_to = plugin,
        },
    },
})
