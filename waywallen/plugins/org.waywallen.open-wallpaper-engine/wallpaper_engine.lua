-- Source plugin for Wallpaper Engine. A library is a Steam Library
-- root (the directory that contains `steamapps/`); workshop and
-- assets paths are derived at apply time.

local M = {}

local WE_APPID   = "431960"
local WORKSHOP   = "/steamapps/workshop/content/" .. WE_APPID
local ASSETS_REL = "/steamapps/common/wallpaper_engine/assets"
-- WE's bundled wallpapers (`defaultprojects`) and user-authored ones
-- (`myprojects`) live beside the workshop content, keyed by folder name
-- instead of a numeric workshop id.
local PROJECTS_REL = "/steamapps/common/wallpaper_engine/projects"
local LOCAL_DIRS   = { "defaultprojects", "myprojects" }

local VIDEO_EXTS = { mp4 = true, webm = true, mkv = true, avi = true, mov = true }

function M.info()
    return {
        name = "wallpaper_engine",
        types = { "scene", "video", "web" },
        version = "0.4.0",
        library_label = "Steam Library Path",
        library_hint =
            "Pick the directory that contains the `steamapps` folder.\n" ..
            "Typically `~/.steam/steam` or `~/.local/share/Steam` " ..
            "(or `~/.var/app/com.valvesoftware.Steam/data/Steam` for Flatpak Steam).",
    }
end

function M.auto_detect(ctx)
    local home = ctx.env("HOME") or ""
    if home == "" then return {} end
    local candidates = {
        home .. "/.steam/steam",
        home .. "/.local/share/Steam",
        home .. "/.var/app/com.valvesoftware.Steam/data/Steam",
        home .. "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
    }
    local found, seen = {}, {}
    for _, root in ipairs(candidates) do
        -- A Steam library qualifies iff WE content lives under it (either
        -- workshop subscriptions or the bundled/local projects), so we
        -- don't auto-register Steam roots without WE installed.
        if not seen[root] and (ctx.file_exists(root .. WORKSHOP)
            or ctx.file_exists(root .. PROJECTS_REL)) then
            seen[root] = true
            table.insert(found, root)
        end
    end
    return found
end

local function pick_preview(ctx, dir, project)
    if project and project.preview then
        local p = dir .. "/" .. project.preview
        if ctx.file_exists(p) then return p end
    end
    for _, p in ipairs({ dir .. "/preview.jpg", dir .. "/preview.png", dir .. "/preview.gif" }) do
        if ctx.file_exists(p) then return p end
    end
    return nil
end

local function classify(ctx, dir, project, project_type)
    if project_type == "web" then
        if ctx.file_exists(dir .. "/project.json") then
            return "web", dir
        end
    elseif project_type == "video" then
        local file = project and project.file
        if file and ctx.file_exists(dir .. "/" .. file) then
            return "video", dir .. "/" .. file
        end
        for _, path in ipairs(ctx.glob(dir .. "/*.*")) do
            local ext = ctx.extension(path)
            if ext and VIDEO_EXTS[string.lower(ext)] then
                return "video", path
            end
        end
    else
        if ctx.file_exists(dir .. "/scene.pkg") then
            return "scene", dir .. "/scene.pkg"
        elseif ctx.file_exists(dir .. "/scene.json") then
            return "scene", dir .. "/scene.json"
        end
    end
    return nil, nil
end

-- Scan one container of project folders (workshop content or a local
-- `projects/*` dir) and append a wallpaper entry per recognized folder.
-- `name_prefix` is the fallback title prefix when project.json has none.
local function scan_container(ctx, steam_root, container, name_prefix, entries)
    if not ctx.file_exists(container) then return end
    for _, dir in ipairs(ctx.list_dirs(container)) do
        local id = ctx.basename(dir) or dir
        local project = nil
        local project_path = dir .. "/project.json"
        if ctx.file_exists(project_path) then
            local content = ctx.read_file(project_path)
            if content then project = ctx.json_parse(content) end
        end
        local project_type = project and project.type and string.lower(project.type) or nil

        local wp_type, resource = classify(ctx, dir, project, project_type)
        if wp_type and resource then
            table.insert(entries, {
                name         = (project and project.title) or (name_prefix .. id),
                wp_type      = wp_type,
                resource     = resource,
                preview      = pick_preview(ctx, dir, project),
                library_root = steam_root,
                description  = project and project.description or nil,
                tags         = (project and project.tags) or {},
                content_rating = project and project.contentrating or nil,
                external_id  = id,
                metadata     = {},
            })
        end
    end
end

function M.scan(ctx)
    local entries = {}

    for _, steam_root in ipairs(ctx.libraries()) do
        local workshop = steam_root .. WORKSHOP
        if not ctx.file_exists(workshop) then
            ctx.log("wallpaper_engine: no WE workshop under " .. steam_root)
        else
            scan_container(ctx, steam_root, workshop, "Workshop ", entries)
        end
        for _, name in ipairs(LOCAL_DIRS) do
            scan_container(ctx, steam_root, steam_root .. PROJECTS_REL .. "/" .. name, "", entries)
        end
    end

    ctx.log("wallpaper_engine: " .. #entries .. " wallpapers")
    return entries
end

-- Lazy cache for WE's English UI i18n table. Keys like
-- `ui_browse_properties_scheme_color` map to friendly labels
-- ("Scheme color"). One library root ⇒ one cache slot; `false` means
-- "looked up and not found" so we don't retry on every WallpaperGet.
local _locale_cache = {}

local LOCALE_REL = "/steamapps/common/wallpaper_engine/locale/ui_en-us.json"

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

-- Called on demand by WallpaperGet to surface the per-wallpaper
-- editable property schema (`general.properties` from `project.json`).
-- Returns a JSON string or nil when the wallpaper has no properties.
-- The project dir is the folder holding project.json. For web the
-- resource is that folder; for scene/video it's a file inside it.
local function project_dir_of(entry)
    if entry.wp_type == "web" then return entry.resource end
    return entry.resource and entry.resource:match("^(.*)/[^/]+$") or nil
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

function M.properties(entry, ctx)
    local dir = project_dir_of(entry)
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

    -- Resolve `text` from WE's UI locale when it's a known i18n key.
    -- Author-written labels (free text) miss the lookup and stay as-is.
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

    return ctx.json_encode(props)
end

-- SPAWN_VERSION 3: daemon calls this at WallpaperApply time to build
-- the renderer's CLI argv. `assets` is derived from the Steam library
-- root; `workshop_id` is the entry's external_id verbatim.
function M.extras(entry, ctx)
    local out = { path = entry.resource }
    if entry.wp_type == "scene" and entry.library_root then
        local assets = entry.library_root .. ASSETS_REL
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
