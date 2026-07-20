local session = import("wallpaper_engine.session")

local M = {}

local SUBSCRIBE = "https://api.steampowered.com/IPublishedFileService/Subscribe/v1/"
local UNSUBSCRIBE = "https://api.steampowered.com/IPublishedFileService/Unsubscribe/v1/"

function M.status(ctx, ids)
    session.ensure_access_token(ctx)
    local result = {}
    for _, id in ipairs(ids) do
        result[tostring(id)] = "unknown"
    end
    return result
end

local function set_subscription(ctx, id, subscribed)
    local access_token = session.ensure_access_token(ctx)
    local rsp = ctx.http
        :post(subscribed and SUBSCRIBE or UNSUBSCRIBE)
        :form({
            access_token = access_token,
            publishedfileid = tostring(id),
            list_type = "1",
            notify_client = "1",
        })
        :timeout(20)
        :send()
    if not rsp:ok() then
        error("Steam subscription request failed with HTTP " .. tostring(rsp:status()))
    end
    return { accepted = true }
end

function M.subscribe(ctx, id)
    return set_subscription(ctx, id, true)
end

function M.unsubscribe(ctx, id)
    return set_subscription(ctx, id, false)
end

return M
