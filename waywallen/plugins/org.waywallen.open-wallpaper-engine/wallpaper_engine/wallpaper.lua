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

function M.properties(entry, ctx)
    local dir = project_util.project_dir_of(entry)
    if not dir then return nil end
    local proj = dir .. "/project.json"
    if not ctx.file_exists(proj) then return nil end
    local content = ctx.read_file(proj)
    if not content then return nil end
    local parsed = ctx.json_parse(content)
    if not parsed or type(parsed) ~= "table" then return nil end
    local props = parsed.general and parsed.general.properties or nil
    if not props or type(props) ~= "table" or next(props) == nil then return nil end
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

    return ctx.json_encode(props)
end

function M.extras(entry, ctx)
    local out = { path = entry.resource }
    if entry.wp_type == "scene" and entry.library_root then
        local assets = entry.library_root .. project_util.ASSETS_REL
        if ctx.file_exists(assets) then
            out.assets = assets
        end
    end
    if entry.external_id and entry.external_id ~= "" then
        out.workshop_id = entry.external_id
    end
    return out
end

return M
