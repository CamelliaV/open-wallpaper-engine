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
    if not ctx.fs.exists(path) then
        _locale_cache[library_root] = false
        return nil
    end
    local content = ctx.fs.read(path)
    if not content then
        _locale_cache[library_root] = false
        return nil
    end
    local parsed = ctx.json.parse(content)
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

local ENABLE_AUDIO_PROPERTY = {
    text = "Enable audio",
    type = "bool",
    value = true,
}

local PLAYBACK_SPEED_PROPERTY = {
    text = "Playback speed",
    type = "slider",
    min = 10,
    max = 400,
    step = 10,
    suffix = "%",
    value = 100,
}

local function load_project_properties(entry, ctx)
    local dir = project_util.project_dir_of(entry)
    if not dir then return nil end
    local proj = dir .. "/project.json"
    if not ctx.fs.exists(proj) then return nil end
    local content = ctx.fs.read(proj)
    if not content then return nil end
    local parsed = ctx.json.parse(content)
    if not parsed or type(parsed) ~= "table" then return nil end
    local props = parsed.general and parsed.general.properties or {}
    if type(props) ~= "table" then return {} end
    return props
end

local function map_property_keys(props)
    for from, to in pairs(PROPERTY_KEY_MAP) do
        local v = props[from]
        if v ~= nil then
            if props[to] == nil then props[to] = v end
            props[from] = nil
        end
    end
end

local function color_wire_value(value)
    if type(value) == "string" then return value end
    if type(value) ~= "table" then return nil end
    local components = {}
    for index = 1, #value do
        if type(value[index]) ~= "number" then return nil end
        components[index] = tostring(value[index])
    end
    if #components < 3 or #components > 4 then return nil end
    return table.concat(components, " ")
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
    if props["waywallen.enable_audio"] == nil then
        props["waywallen.enable_audio"] = ENABLE_AUDIO_PROPERTY
    end
    if (entry.wp_type == "scene" or entry.wp_type == "video") and
        props["waywallen.playback_speed"] == nil then
        props["waywallen.playback_speed"] = PLAYBACK_SPEED_PROPERTY
    end
end

function M.properties(entry, ctx)
    local props = load_project_properties(entry, ctx)
    if not props then return nil end
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

    return ctx.json.encode(props)
end

local function we_assets(ctx)
    local configured = ctx.config.get("wallpaper_engine_assets")
    if configured and configured ~= "" and ctx.fs.exists(configured) then
        return configured
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
        if ctx.fs.exists(p) then
            return p
        end
    end
    return nil
end

function M.apply(entry, ctx)
    local extras = { path = entry.resource }
    local default_user_properties = {}
    if entry.wp_type == "video" then
        local props = load_project_properties(entry, ctx)
        if props then
            map_property_keys(props)
            local scheme = props["waywallen.scheme_color"]
            if type(scheme) == "table" then
                local value = color_wire_value(scheme.value)
                if value then
                    default_user_properties["waywallen.scheme_color"] = value
                end
            end
        end
    end
    if entry.wp_type == "scene" then
        local assets
        if entry.library_root and entry.library_root ~= "" then
            local candidate = entry.library_root .. project_util.ASSETS_REL
            if ctx.fs.exists(candidate) then
                assets = candidate
            end
        end
        assets = assets or we_assets(ctx)
        if assets then
            extras.assets = assets
        end
    end
    if entry.external_id and entry.external_id ~= "" then
        extras.workshop_id = entry.external_id
    end
    return {
        extras = extras,
        default_user_properties = default_user_properties,
    }
end

return M
