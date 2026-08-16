local M = {}
local session = import("wallpaper_engine.session")

M.APPID = "431960"
local QUERYFILES = "https://api.steampowered.com/IPublishedFileService/QueryFiles/v1/"
local FILEDETAILS = "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/"
local NUMPERPAGE = 30

-- sort key -> EPublishedFileQueryType (+ trend window in days).
-- 0 RankedByVote, 1 RankedByPublicationDate, 3 RankedByTrend, 9 RankedByTotalUniqueSubscriptions.
local SORTS = {
    trend_day = { query_type = 3, days = 1 },
    trend_week = { query_type = 3, days = 7 },
    trend_month = { query_type = 3, days = 30 },
    trend_3months = { query_type = 3, days = 90 },
    trend_6months = { query_type = 3, days = 180 },
    trend_year = { query_type = 3, days = 365 },
    recent = { query_type = 1 },
    most_subscribed = { query_type = 9 },
    top_rated = { query_type = 0 },
}

-- Workshop type tags -> waywallen wp_type. Application is omitted (unrenderable, unless wine..?).
M.TYPE_WP = {
    Scene = "scene",
    Video = "video",
    Web = "web",
}

local RATING_TAGS = { Everyone = true, Questionable = true, Mature = true }

M.filters = {
    {
        id = "type",
        title = "Type",
        type = "multi_select",
        values = { "Scene", "Video", "Web" },
    },
    {
        id = "content_rating",
        title = "Content rating",
        type = "multi_select",
        values = { "Everyone", "Questionable", "Mature" },
        description = "Defaults to Everyone when nothing is selected. Mature includes NSFW wallpapers.",
    },
    {
        id = "miscellaneous",
        title = "Miscellaneous",
        type = "multi_select",
        values = {
            "Approved",
            "Audio responsive",
            "3D",
            "Customizable",
            "Puppet Warp",
            "HDR",
            "Media Integration",
            "User Shortcut",
            "Video Texture",
        },
    },
    {
        id = "genre",
        title = "Genre",
        type = "multi_select",
        values = {
            "Abstract",
            "Animal",
            "Anime",
            "Cartoon",
            "CGI",
            "Cyberpunk",
            "Fantasy",
            "Game",
            "Girls",
            "Guys",
            "Landscape",
            "Medieval",
            "Memes",
            "MMD",
            "Music",
            "Nature",
            "Pixel art",
            "Relaxing",
            "Retro",
            "Sci-Fi",
            "Sports",
            "Technology",
            "Television",
            "Vehicle",
            "Unspecified",
        },
    },
    {
        id = "resolution",
        title = "Resolution",
        type = "select",
        values = {
            "Standard Definition",
            "1280 x 720",
            "1366 x 768",
            "1920 x 1080",
            "2560 x 1440",
            "3840 x 2160",
            "Ultrawide Standard Definition",
            "Ultrawide 2560 x 1080",
            "Ultrawide 3440 x 1440",
            "Dual Standard Definition",
            "Dual 3840 x 1080",
            "Dual 5120 x 1440",
            "Dual 7680 x 2160",
            "Triple Standard Definition",
            "Triple 4096 x 768",
            "Triple 5760 x 1080",
            "Triple 7680 x 1440",
            "Triple 11520 x 2160",
            "Portrait Standard Definition",
            "Portrait 720 x 1280",
            "Portrait 1080 x 1920",
            "Portrait 1440 x 2560",
            "Portrait 2160 x 3840",
            "Other resolution",
            "Dynamic resolution",
        },
    },
}

function M.search(ctx, params)
    local sort = SORTS[params.sort] or SORTS.trend_week

    -- Only wallpapers: never return applications, presets or asset packs.
    -- Content rating defaults to Everyone; explicit selections replace that default.
    local excluded = {
        Application = true, Preset = true, Asset = true, ["Asset Pack"] = true,
    }
    local required = {}
    local selected_types = {}
    local selected_ratings = {}
    local selected_rating_count = 0
    for _, tag in ipairs(params.tags or {}) do
        if M.TYPE_WP[tag] then
            selected_types[tag] = true
        elseif RATING_TAGS[tag] then
            if not selected_ratings[tag] then
                selected_ratings[tag] = true
                selected_rating_count = selected_rating_count + 1
            end
        else
            table.insert(required, tag)
        end
    end
    if next(selected_types) then
        for tag in pairs(M.TYPE_WP) do
            if not selected_types[tag] then excluded[tag] = true end
        end
    end
    if selected_rating_count == 0 then
        selected_ratings.Everyone = true
        selected_rating_count = 1
    end
    for tag in pairs(RATING_TAGS) do
        if not selected_ratings[tag] then excluded[tag] = true end
    end
    if selected_rating_count == 1 then
        for tag in pairs(selected_ratings) do table.insert(required, tag) end
    end

    local query = {
        appid = M.APPID,
        query_type = tostring(sort.query_type),
        page = tostring(params.page or 1),
        numperpage = tostring(NUMPERPAGE),
        search_text = params.query or "",
        return_tags = "true",
        return_previews = "true",
        return_vote_data = "true",
        return_short_description = "true",
        format = "json",
    }
    if sort.days then
        query.days = tostring(sort.days)
    end
    for i, tag in ipairs(required) do
        query["requiredtags[" .. (i - 1) .. "]"] = tag
    end
    local ei = 0
    for tag in pairs(excluded) do
        query["excludedtags[" .. ei .. "]"] = tag
        ei = ei + 1
    end

    local rsp = session.authorized(ctx, function(access_token, http)
        query.access_token = access_token
        return http:get(QUERYFILES):query(query):timeout(20):send()
    end)
    if not rsp:ok() then
        error("steam workshop http " .. tostring(rsp:status()))
    end
    local body = rsp:json() or {}
    local response = body.response or {}
    return {
        items = response.publishedfiledetails or {},
        total = response.total or 0,
        numperpage = NUMPERPAGE,
    }
end

function M.details(ctx, id)
    local rsp = ctx.http
        :post(FILEDETAILS)
        :form({ itemcount = "1", ["publishedfileids[0]"] = tostring(id) })
        :timeout(20)
        :send()
    if not rsp:ok() then
        error("steam workshop http " .. tostring(rsp:status()))
    end
    local body = rsp:json() or {}
    local list = (body.response or {}).publishedfiledetails or {}
    return list[1] or {}
end

return M
