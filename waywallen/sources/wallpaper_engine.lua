-- Source plugin for Wallpaper Engine. A library is a Steam Library
-- root (the directory that contains `steamapps/`); workshop and
-- assets paths are derived at apply time.

local M = {}

local WE_APPID   = "431960"
local WORKSHOP   = "/steamapps/workshop/content/" .. WE_APPID
local ASSETS_REL = "/steamapps/common/wallpaper_engine/assets"

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
    }
    local found, seen = {}, {}
    for _, root in ipairs(candidates) do
        -- A Steam library qualifies iff WE workshop lives under it,
        -- so we don't auto-register Steam roots without WE installed.
        if not seen[root] and ctx.file_exists(root .. WORKSHOP) then
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

function M.scan(ctx)
    local entries = {}

    for _, steam_root in ipairs(ctx.libraries()) do
        local workshop = steam_root .. WORKSHOP
        if not ctx.file_exists(workshop) then
            ctx.log("wallpaper_engine: no WE workshop under " .. steam_root)
        else
            for _, dir in ipairs(ctx.list_dirs(workshop)) do
                local workshop_id = ctx.basename(dir) or dir
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
                        id           = workshop_id,
                        name         = (project and project.title) or ("Workshop " .. workshop_id),
                        wp_type      = wp_type,
                        resource     = resource,
                        preview      = pick_preview(ctx, dir, project),
                        library_root = steam_root,
                        description  = project and project.description or nil,
                        tags         = (project and project.tags) or {},
                        external_id  = workshop_id,
                        metadata     = {},
                    })
                end
            end
        end
    end

    ctx.log("wallpaper_engine: " .. #entries .. " wallpapers")
    return entries
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
