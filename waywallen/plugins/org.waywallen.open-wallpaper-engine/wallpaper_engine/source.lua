local project_util = import("wallpaper_engine.project")

local M = {}

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
        if not seen[root] and (ctx.file_exists(root .. project_util.WORKSHOP)
            or ctx.file_exists(root .. project_util.PROJECTS_REL)) then
            seen[root] = true
            table.insert(found, root)
        end
    end
    return found
end

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

        local wp_type, resource = project_util.classify(ctx, dir, project, project_type)
        if wp_type and resource then
            table.insert(entries, {
                name = (project and project.title) or (name_prefix .. id),
                wp_type = wp_type,
                resource = resource,
                preview = project_util.pick_preview(ctx, dir, project),
                library_root = steam_root,
                description = project and project.description or nil,
                tags = (project and project.tags) or {},
                content_rating = project and project.contentrating or nil,
                external_id = id,
                metadata = {},
            })
        end
    end
end

function M.scan(ctx)
    local entries = {}

    for _, steam_root in ipairs(ctx.libraries()) do
        local workshop = steam_root .. project_util.WORKSHOP
        if not ctx.file_exists(workshop) then
            ctx.log("wallpaper_engine: no WE workshop under " .. steam_root)
        else
            scan_container(ctx, steam_root, workshop, "Workshop ", entries)
        end
        for _, name in ipairs(project_util.LOCAL_DIRS) do
            scan_container(ctx, steam_root, steam_root .. project_util.PROJECTS_REL .. "/" .. name, "", entries)
        end
    end

    ctx.log("wallpaper_engine: " .. #entries .. " wallpapers")
    return entries
end

function M.remove(ctx, item)
    local root = item.library_root
    local id = item.external_id
    local rel = item.relative_path
    if not root or root == "" or not id or id == "" or not rel then
        return
    end
    -- Only downloaded items sit directly under <library_root>/<id>; scanned Steam
    -- items nest under steamapps/, so never delete those off the user's disk.
    if rel ~= id and rel:sub(1, #id + 1) ~= id .. "/" then
        return
    end
    ctx.remove_dir(root .. "/" .. id)
end

return M
