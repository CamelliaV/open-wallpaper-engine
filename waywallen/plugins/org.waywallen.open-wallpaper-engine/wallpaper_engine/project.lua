local M = {}

M.WE_APPID = "431960"
M.WORKSHOP = "/steamapps/workshop/content/" .. M.WE_APPID
M.ASSETS_REL = "/steamapps/common/wallpaper_engine/assets"
M.PROJECTS_REL = "/steamapps/common/wallpaper_engine/projects"
M.LOCAL_DIRS = { "defaultprojects", "myprojects" }

local VIDEO_EXTS = { mp4 = true, webm = true, mkv = true, avi = true, mov = true }

function M.pick_preview(ctx, dir, project)
    if project and project.preview then
        local p = dir .. "/" .. project.preview
        if ctx.file_exists(p) then return p end
    end
    for _, p in ipairs({ dir .. "/preview.jpg", dir .. "/preview.png", dir .. "/preview.gif" }) do
        if ctx.file_exists(p) then return p end
    end
    return nil
end

function M.classify(ctx, dir, project, project_type)
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

function M.project_dir_of(entry)
    if entry.wp_type == "web" then return entry.resource end
    return entry.resource and entry.resource:match("^(.*)/[^/]+$") or nil
end

return M
