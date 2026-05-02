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
    end

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

            -- Metadata is forwarded to the renderer via the daemon's
            -- typed `Init` message; `path` is the canonical resource
            -- key (v6 schema), `assets` + `workshop_id` ride along as
            -- allow-listed extras. mpv's manifest is schema-less and
            -- falls back to legacy resolution that picks `path` last
            -- in the priority chain, so the video flow keeps working
            -- through the path key alone.
            local metadata = { path = resource }
            if wp_type == "scene" then
                metadata.assets = we_assets
            end
            metadata.workshop_id = workshop_id

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
                metadata = metadata,
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
-- We read what `scan()` cached in entry.metadata so we don't have to
-- re-walk the workshop dir.
function M.extras(entry)
    local md = entry.metadata or {}
    local out = { path = md.path or entry.resource }
    if entry.wp_type == "scene" then
        if md.assets and md.assets ~= "" then
            out.assets = md.assets
        end
    end
    if md.workshop_id and md.workshop_id ~= "" then
        out.workshop_id = md.workshop_id
    end
    return out
end

return M
