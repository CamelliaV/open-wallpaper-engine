local M = {}

local GENERATE_ACCESS_TOKEN =
    "https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForApp/v1/"
local FORMAT_VERSION = "steam-session-v3"
local LINE_FORMAT_VERSION = "steam-session-v2"

local state = {
    account_name = "",
    steamid = "",
    access_token = "",
    refresh_token = "",
}
local last_check = { state = "signed_out", display_value = "Not signed in" }

local function remember(check)
    last_check = check
    return check
end

local function clear()
    state.account_name = ""
    state.steamid = ""
    state.access_token = ""
    state.refresh_token = ""
    last_check = { state = "signed_out", display_value = "Not signed in" }
end

local function jwt_payload(ctx, token)
    local encoded = token and token:match("^[^.]+%.([^.]+)%.")
    if not encoded then return nil end
    local ok, decoded = pcall(ctx.base64.decode, encoded)
    if not ok or not decoded then return nil end
    return ctx.json.parse(decoded)
end

local function token_valid(ctx, token)
    local payload = jwt_payload(ctx, token)
    local expires = payload and tonumber(payload.exp)
    return expires ~= nil and expires > ctx.time.unix() + 60
end

function M.load(blob)
    clear()
    if type(blob) ~= "string" or blob == "" then return end
    if blob:sub(1, #FORMAT_VERSION + 1) == FORMAT_VERSION .. "\n" then
        local fields = {}
        local offset = #FORMAT_VERSION + 2
        for index = 1, 4 do
            local separator = blob:find("\n", offset, true)
            if not separator then return clear() end
            local length = tonumber(blob:sub(offset, separator - 1))
            if not length or length < 0 or length % 1 ~= 0 then return clear() end
            local first = separator + 1
            local last = first + length - 1
            if last > #blob then return clear() end
            fields[index] = blob:sub(first, last)
            offset = last + 1
        end
        if offset <= #blob then return clear() end
        state.account_name = fields[1]
        state.steamid = fields[2]
        state.access_token = fields[3]
        state.refresh_token = fields[4]
        return
    end
    local lines = {}
    for line in (blob .. "\n"):gmatch("(.-)\n") do
        table.insert(lines, line)
    end
    if lines[1] ~= LINE_FORMAT_VERSION then return end
    state.account_name = lines[2] or ""
    state.steamid = lines[3] or ""
    state.access_token = lines[4] or ""
    state.refresh_token = lines[5] or ""
end

function M.save()
    local fields = {
        state.account_name,
        state.steamid,
        state.access_token,
        state.refresh_token,
    }
    local chunks = { FORMAT_VERSION, "\n" }
    for _, value in ipairs(fields) do
        table.insert(chunks, tostring(#value))
        table.insert(chunks, "\n")
        table.insert(chunks, value)
    end
    return table.concat(chunks)
end

local function legacy_json_string(raw, key)
    local _, last = raw:find('"' .. key .. '"%s*:%s*"')
    if not last then return "" end
    local out = {}
    local index = last + 1
    while index <= #raw do
        local char = raw:sub(index, index)
        if char == '"' then return table.concat(out) end
        if char ~= "\\" then
            table.insert(out, char)
            index = index + 1
        else
            local escaped = raw:sub(index + 1, index + 1)
            local replacements = {
                ['"'] = '"', ["\\"] = "\\", ["/"] = "/",
                b = "\b", f = "\f", n = "\n", r = "\r", t = "\t",
            }
            if replacements[escaped] then
                table.insert(out, replacements[escaped])
                index = index + 2
            elseif escaped == "u" then
                local hex = raw:sub(index + 2, index + 5)
                local codepoint = tonumber(hex, 16)
                if not codepoint then return "" end
                table.insert(out, utf8.char(codepoint))
                index = index + 6
            else
                return ""
            end
        end
    end
    return ""
end

function M.migrate(schema_id, raw)
    if schema_id ~= "waywallen-steam-session-v1" then
        error("unsupported Steam session schema")
    end
    state.account_name = legacy_json_string(raw, "account_name")
    state.access_token = legacy_json_string(raw, "access_token")
    state.refresh_token = legacy_json_string(raw, "refresh_token")
    state.steamid = legacy_json_string(raw, "steamid")
    return M.save()
end

function M.set_auth(account_name, access_token, refresh_token)
    state.account_name = account_name or ""
    state.access_token = access_token or ""
    state.refresh_token = refresh_token or ""
    state.steamid = ""
    last_check = {
        state = "signed_in",
        display_value = state.account_name ~= "" and ("Signed in as " .. state.account_name)
            or "Signed in",
    }
end

function M.sign_out()
    clear()
end

function M.account_name()
    return state.account_name
end

function M.signed_in()
    return state.refresh_token ~= ""
end

function M.ensure_access_token(ctx)
    if token_valid(ctx, state.access_token) then
        return state.access_token
    end
    if state.refresh_token == "" then
        error("Steam sign-in is required")
    end

    local payload = jwt_payload(ctx, state.refresh_token)
    local steamid = state.steamid ~= "" and state.steamid or (payload and payload.sub or "")
    if steamid == "" then
        error("Steam session is invalid; sign in again")
    end
    local rsp = ctx.http
        :post(GENERATE_ACCESS_TOKEN)
        :form({
            refresh_token = state.refresh_token,
            steamid = steamid,
            renewal_type = "1",
            format = "json",
        })
        :timeout(20)
        :send()
    if not rsp:ok() then
        error("Steam session refresh failed with HTTP " .. tostring(rsp:status()))
    end
    local response = ((rsp:json() or {}).response or {})
    if type(response.access_token) ~= "string" or response.access_token == "" then
        error("Steam session expired; sign in again")
    end
    state.access_token = response.access_token
    if type(response.refresh_token) == "string" and response.refresh_token ~= "" then
        state.refresh_token = response.refresh_token
    end
    state.steamid = steamid
    return state.access_token
end

function M.check(ctx)
    if not M.signed_in() then
        return remember({ state = "signed_out", display_value = "Not signed in" })
    end
    local ok, error_value = pcall(M.ensure_access_token, ctx)
    if not ok then
        local message = tostring(error_value)
        local expired = message:find("expired; sign in again", 1, true) ~= nil
            or message:find("invalid; sign in again", 1, true) ~= nil
        return remember({
            state = expired and "expired" or "error",
            display_value = state.account_name ~= "" and state.account_name or "Steam",
            error = expired and "Steam session expired; sign in again"
                or "Steam session check failed",
        })
    end
    local display = state.account_name ~= "" and ("Signed in as " .. state.account_name)
        or "Signed in"
    return remember({ state = "signed_in", display_value = display })
end

function M.current_check()
    return last_check
end

return M
