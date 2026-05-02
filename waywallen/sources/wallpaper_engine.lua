local M = {}

function M.info()
    return {
        name = "wallpaper_engine",
        types = {"scene", "video"},
        version = "0.2.0",
    }
end

function M.auto_detect(ctx)
    -- Probe the standard Steam library paths for the Wallpaper Engine
    -- workshop dir (appid 431960). Returns every candidate that
    -- actually exists on disk so the daemon can register them.
    local home = ctx.env("HOME") or ""
    local candidates = {}
    if home ~= "" then
        table.insert(candidates, home .. "/.steam/steam/steamapps/workshop/content/431960")
        table.insert(candidates, home .. "/.local/share/Steam/steamapps/workshop/content/431960")
        table.insert(candidates,
            home .. "/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/workshop/content/431960")
    end
    local found, seen = {}, {}
    for _, p in ipairs(candidates) do
        if not seen[p] and ctx.file_exists(p) then
            seen[p] = true
            table.insert(found, p)
        end
    end
    return found
end

function M.scan(ctx)
    local entries = {}

    -- Libraries are owned by the daemon DB. Each registered library
    -- should point at a Steam "workshop/content/431960" directory;
    -- the plugin iterates over every user-configured root.
    local workshop_dirs = {}
    for _, d in ipairs(ctx.libraries()) do
        if ctx.file_exists(d) then table.insert(workshop_dirs, d) end
    end
    if #workshop_dirs == 0 then
        ctx.log("wallpaper_engine: no workshop libraries configured")
        return entries
    end

    local video_exts = {mp4 = true, webm = true, mkv = true, avi = true, mov = true}

    for _, workshop_dir in ipairs(workshop_dirs) do
    -- Derive WE installation assets dir from workshop path.
    -- workshop_dir = .../steamapps/workshop/content/431960
    -- we_assets    = .../steamapps/common/wallpaper_engine/assets
    local steamapps = workshop_dir:match("(.*/steamapps)/workshop/content/%d+$")
    local we_assets = steamapps and (steamapps .. "/common/wallpaper_engine/assets") or ""
    if we_assets == "" or not ctx.file_exists(we_assets) then
        ctx.log("wallpaper_engine: WE assets dir not found under " .. workshop_dir
                .. ", shaders may be missing")
        we_assets = ""
    end
    -- Stash the WE assets dir on `library.metadata` so `extras(entry, ctx)`
    -- can fetch it later without re-deriving it. Stored once per
    -- library (per workshop dir), shared by every scene under it.
    ctx.library_meta_set(workshop_dir, "assets", we_assets ~= "" and we_assets or nil)

    local dirs = ctx.list_dirs(workshop_dir)
    for _, dir in ipairs(dirs) do
        local workshop_id = ctx.basename(dir) or dir
        local name = "Workshop " .. workshop_id

        -- Parse project.json first to determine wallpaper type.
        local project = nil
        local project_path = dir .. "/project.json"
        if ctx.file_exists(project_path) then
            local content = ctx.read_file(project_path)
            if content then
                project = ctx.json_parse(content)
            end
        end

        local project_type = project and project.type and string.lower(project.type) or nil
        if project and project.title then
            name = project.title
        end

        local wp_type = nil
        local resource = nil

        if project_type == "video" then
            -- Resolve the video file referenced by project.json.
            local file = project and project.file
            if file and ctx.file_exists(dir .. "/" .. file) then
                wp_type = "video"
                resource = dir .. "/" .. file
            else
                -- Fallback: first video file in the directory.
                local pkg_dir_files = ctx.glob(dir .. "/*.*")
                for _, path in ipairs(pkg_dir_files) do
                    local ext = ctx.extension(path)
                    if ext and video_exts[string.lower(ext)] then
                        wp_type = "video"
                        resource = path
                        break
                    end
                end
            end
        else
            -- Scene wallpaper (default). Prefer scene.pkg, fall back to scene.json.
            local pkg_path = dir .. "/scene.pkg"
            local json_path = dir .. "/scene.json"
            if ctx.file_exists(pkg_path) then
                wp_type = "scene"
                resource = pkg_path
            elseif ctx.file_exists(json_path) then
                wp_type = "scene"
                resource = json_path
            end
        end

        if wp_type and resource then
            -- Look for preview image
            local preview = nil
            if project and project.preview then
                local p = dir .. "/" .. project.preview
                if ctx.file_exists(p) then
                    preview = p
                end
            end
            if not preview then
                local preview_candidates = {
                    dir .. "/preview.jpg",
                    dir .. "/preview.png",
                    dir .. "/preview.gif",
                }
                for _, p in ipairs(preview_candidates) do
                    if ctx.file_exists(p) then
                        preview = p
                        break
                    end
                end
            end

            -- `assets` lives on library.metadata (set once per
            -- workshop dir above), `workshop_id` rides as
            -- `external_id`. Both are reconstructed in
            -- `extras(entry, ctx)` so we don't duplicate them on
            -- every entry.
            table.insert(entries, {
                id = workshop_id,
                name = name,
                wp_type = wp_type,
                resource = resource,
                preview = preview,
                library_root = workshop_dir,
                description = project and project.description or nil,
                tags = (project and project.tags) or {},
                external_id = workshop_id,
                metadata = {},
            })
        end
    end

    end -- per-workshop_dir loop

    ctx.log("wallpaper_engine: found " .. #entries .. " wallpapers across "
            .. #workshop_dirs .. " libraries")
    return entries
end

-- SPAWN_VERSION 3: daemon calls this at WallpaperApply time to build
-- the renderer's CLI argv. For scene wallpapers we surface `path` +
-- the wescene manifest's whitelisted extras (`assets`, `workshop_id`);
-- for video wallpapers (mpv/video plugin), only `path` is meaningful.
-- `assets` is pulled from library.metadata (cached at scan time);
-- `workshop_id` is the entry's external_id verbatim.
function M.extras(entry, ctx)
    local out = { path = entry.resource }
    if entry.wp_type == "scene" and entry.library_root then
        local assets = ctx.library_meta_get(entry.library_root, "assets")
        if assets and assets ~= "" then
            out.assets = assets
        end
    end
    if entry.external_id and entry.external_id ~= "" then
        out.workshop_id = entry.external_id
    end
    return out
end

return M
