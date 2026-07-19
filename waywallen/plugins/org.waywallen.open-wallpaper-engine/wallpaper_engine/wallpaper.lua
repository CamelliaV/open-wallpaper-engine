local project_util = import("wallpaper_engine.project")

local M = {}

local _locale_cache = {}

local LOCALE_REL = "/steamapps/common/wallpaper_engine/locale/ui_en-us.json"
local PROPERTY_TITLE_PREFIX = "<style>\n  img { max-width: 100%; }\n  </style>\n"

local function load_locale(ctx, library_root)
    if library_root == nil or library_root == "" then return nil end
    if _locale_cache[library_root] ~= nil then
        return _locale_cache[library_root] or nil
    end
    local path = library_root .. LOCALE_REL
    if not ctx.file_exists(path) then
        _locale_cache[library_root] = false
        return nil
    end
    local content = ctx.read_file(path)
    if not content then
        _locale_cache[library_root] = false
        return nil
    end
    local parsed = ctx.json_parse(content)
    if type(parsed) ~= "table" then
        _locale_cache[library_root] = false
        return nil
    end
    _locale_cache[library_root] = parsed
    return parsed
end

local PROPERTY_KEY_MAP = {
    schemecolor = "waywallen.scheme_color"
}

local PREDEFINED_PROPERTIES = {
    ["waywallen.enable_audio"] = {
        text = "Enable audio",
        type = "bool",
        value = true,
    },
}

local function map_property_keys(props)
    for from, to in pairs(PROPERTY_KEY_MAP) do
        local v = props[from]
        if v ~= nil then
            if props[to] == nil then props[to] = v end
            props[from] = nil
        end
    end
end

local function prefix_property_titles(props)
    for _, v in pairs(props) do
        if type(v) == "table" and type(v.text) == "string" then
            v.text = PROPERTY_TITLE_PREFIX .. v.text
        end
    end
end

local function add_predefined_properties(entry, props)
    if entry.wp_type == "web" then return end
    for k, v in pairs(PREDEFINED_PROPERTIES) do
        if props[k] == nil then props[k] = v end
    end
end

function M.properties(entry, ctx)
    local dir = project_util.project_dir_of(entry)
    if not dir then return nil end
    local proj = dir .. "/project.json"
    if not ctx.file_exists(proj) then return nil end
    local content = ctx.read_file(proj)
    if not content then return nil end
    local parsed = ctx.json_parse(content)
    if not parsed or type(parsed) ~= "table" then return nil end
    local props = parsed.general and parsed.general.properties or {}
    if type(props) ~= "table" then props = {} end
    map_property_keys(props)

    local locale = load_locale(ctx, entry.library_root)
    if locale then
        for _, v in pairs(props) do
            if type(v) == "table" and type(v.text) == "string" then
                local mapped = locale[v.text]
                if type(mapped) == "string" and mapped ~= "" then
                    v.text = mapped
                end
            end
        end
    end

    prefix_property_titles(props)
    add_predefined_properties(entry, props)

    return ctx.json_encode(props)
end

local function data_home(ctx)
    local xdg = ctx.env("XDG_DATA_HOME")
    if xdg and xdg ~= "" then
        return xdg
    end
    local home = ctx.env("HOME")
    if home and home ~= "" then
        return home .. "/.local/share"
    end
    return nil
end

-- Downloaded items have no Steam library_root, so fall back to a configured or
-- daemon-managed assets tree, then any local Steam install.
local function we_assets(ctx)
    local configured = ctx.plugin_config and ctx.plugin_config("wallpaper_engine_assets")
    if configured and configured ~= "" and ctx.file_exists(configured) then
        return configured
    end
    local data = data_home(ctx)
    if data then
        local managed = data .. "/waywallen/wallpaper_engine/assets"
        if ctx.file_exists(managed) then
            return managed
        end
    end
    local home = ctx.env("HOME") or ""
    local roots = {
        home .. "/.local/share/Steam",
        home .. "/.steam/steam",
        home .. "/.steam/root",
        home .. "/.var/app/com.valvesoftware.Steam/data/Steam",
    }
    for _, root in ipairs(roots) do
        local p = root .. project_util.ASSETS_REL
        if ctx.file_exists(p) then
            return p
        end
    end
    return nil
end

function M.extras(entry, ctx)
    local out = { path = entry.resource }
    if entry.wp_type == "scene" then
        local assets
        if entry.library_root and entry.library_root ~= "" then
            local candidate = entry.library_root .. project_util.ASSETS_REL
            if ctx.file_exists(candidate) then
                assets = candidate
            end
        end
        assets = assets or we_assets(ctx)
        if assets then
            out.assets = assets
        end
    end
    if entry.external_id and entry.external_id ~= "" then
        out.workshop_id = entry.external_id
    end
    return out
end

return M
