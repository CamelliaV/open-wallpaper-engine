local plugin_root = assert(arg[1], "plugin root argument required")
local cache = {}

function import(name)
    if cache[name] ~= nil then return cache[name] end
    local path = plugin_root .. "/" .. name:gsub("%.", "/") .. ".lua"
    local chunk = assert(loadfile(path, "t", _ENV))
    local module = assert(chunk(), "module did not return a value: " .. name)
    cache[name] = module
    return module
end

local fixtures = assert(loadfile(plugin_root .. "/tests/fixtures.lua", "t", _ENV))()

local function equal(actual, expected, message)
    if actual ~= expected then
        error((message or "values differ") .. ": expected " .. tostring(expected)
            .. ", got " .. tostring(actual), 2)
    end
end

local function truthy(value, message)
    if not value then error(message or "expected a truthy value", 2) end
end

local function fake_context()
    local queued = {}
    local requests = {}
    local decoded = {
        expired = { exp = 1 },
        valid = { exp = 4102444800 },
        refresh = { exp = 4102444800, sub = "76561198000000000" },
        refresh2 = { exp = 4102444800, sub = "76561198000000000" },
    }

    local function request(method, url)
        local value = { method = method, url = url }
        function value:form(form)
            self.form_data = form
            return self
        end
        function value:query(query)
            self.query_data = query
            return self
        end
        function value:headers(headers)
            self.header_data = headers
            return self
        end
        function value:timeout(seconds)
            self.timeout_seconds = seconds
            return self
        end
        function value:send()
            table.insert(requests, self)
            local fixture = table.remove(queued, 1)
            if not fixture then error("no fake HTTP response queued for " .. self.url) end
            local response = {
                status_code = fixture.status or 200,
                json_body = fixture.json,
                text_body = fixture.text or "",
            }
            function response:ok()
                return self.status_code >= 200 and self.status_code < 300
            end
            function response:status() return self.status_code end
            function response:json() return self.json_body end
            function response:text() return self.text_body end
            return response
        end
        return value
    end

    local ctx = {
        http = {
            get = function(_, url) return request("GET", url) end,
            post = function(_, url) return request("POST", url) end,
        },
        log = function() end,
    }
    ctx.time = { unix = function() return 2 end }
    ctx.base64 = { decode = function(value) return value end }
    ctx.json = {
        parse = function(value) return decoded[value] end,
        encode = function() return '{"device_friendly_name":"waywallen"}' end,
    }
    function ctx.queue(json, status, text)
        table.insert(queued, { json = json, status = status, text = text })
    end
    function ctx.last_request() return requests[#requests] end
    function ctx.request_at(index) return requests[index] end
    function ctx.request_count() return #requests end
    function ctx.queued_count() return #queued end
    return ctx
end

local main = assert(loadfile(plugin_root .. "/main.lua", "t", _ENV))()
local info = main.info()
equal(info.display_name, "Workshop", "display name")
equal(info.capabilities.discover.subscription, true, "subscription capability")
equal(info.capabilities.discover.download, nil, "download capability must be absent")
equal(info.capabilities.discover.resolve, nil, "resolve capability must be absent")
truthy(info.capabilities.discover.remote_hint:find("Refresh", 1, true) ~= nil,
    "subscription refresh hint missing")
equal(main.discover.download, nil, "download callback must be absent")
equal(main.discover.resolve, nil, "resolve callback must be absent")
truthy(type(main.subscription.status) == "function", "subscription.status missing")
truthy(type(main.qrlogin.begin) == "function", "qrlogin.begin missing")

local session = import("wallpaper_engine.session")
local auth = import("wallpaper_engine.auth")
local subscription = import("wallpaper_engine.subscription")

session.sign_out()
local auth_ctx = fake_context()
auth_ctx.queue(fixtures.qr_begin)
local begin = auth.begin(auth_ctx)
equal(begin.challenge, "https://s.team/q/example", "QR challenge")
equal(begin.poll_after_ms, 1500, "QR interval")
local request = auth_ctx.last_request()
equal(request.method, "POST", "QR begin method")
equal(request.form_data.website_id, "Client", "QR website id")
equal(request.form_data.platform_type, "1", "QR platform")

auth_ctx.queue(fixtures.qr_pending)
equal(auth.poll(auth_ctx, begin.key).state, "awaiting_scan", "pending QR state")
auth_ctx.queue(fixtures.qr_confirmation)
equal(auth.poll(auth_ctx, begin.key).state, "awaiting_confirmation", "confirmation QR state")
auth_ctx.queue(fixtures.qr_rotation)
local rotated = auth.poll(auth_ctx, begin.key)
equal(rotated.state, "challenge_changed", "rotation QR state")
equal(rotated.challenge, "https://s.team/q/rotated", "rotated challenge")
auth_ctx.queue(fixtures.qr_success)
equal(auth.poll(auth_ctx, begin.key).state, "succeeded", "successful QR state")
equal(session.account_name(), "fixture-account", "QR account")

session.set_auth("line one\nline two", "header.valid.signature", "header.refresh.signature")
local serialized = session.save()
session.sign_out()
session.load(serialized)
equal(session.account_name(), "line one\nline two", "opaque state round trip")
session.load(table.concat({
    "steam-session-v2",
    "line-format-account",
    "76561198000000000",
    "header.valid.signature",
    "header.refresh.signature",
}, "\n"))
equal(session.account_name(), "line-format-account", "line state compatibility")
local migrated = session.migrate(
    "waywallen-steam-session-v1",
    '{"account_name":"escaped\\nname","access_token":"header.valid.signature",'
        .. '"refresh_token":"header.refresh.signature","steamid":"76561198000000000"}'
)
session.sign_out()
session.load(migrated)
equal(session.account_name(), "escaped\nname", "legacy JSON migration")

local refresh_ctx = fake_context()
session.set_auth("fixture-account", "header.expired.signature", "header.refresh.signature")
refresh_ctx.queue(fixtures.token_refresh)
equal(session.ensure_access_token(refresh_ctx), "header.valid.signature", "refreshed token")
request = refresh_ctx.last_request()
equal(request.form_data.refresh_token, "header.refresh.signature", "refresh token parameter")
equal(request.form_data.steamid, "76561198000000000", "refresh SteamID parameter")
equal(request.form_data.renewal_type, "1", "refresh renewal type")
local refreshed_state = session.save()
session.sign_out()
session.load(refreshed_state)
local restart_ctx = fake_context()
equal(session.ensure_access_token(restart_ctx), "header.valid.signature",
    "refreshed token survives state reload")
equal(restart_ctx.request_count(), 0, "valid restored token must not refresh again")

local failed_refresh_ctx = fake_context()
session.set_auth("fixture-account", "header.expired.signature", "header.refresh.signature")
failed_refresh_ctx.queue({ response = {} })
equal(session.check(failed_refresh_ctx).state, "expired", "empty refresh response expires session")

local transient_refresh_ctx = fake_context()
session.set_auth("fixture-account", "header.expired.signature", "header.refresh.signature")
transient_refresh_ctx.queue({}, 503)
equal(session.check(transient_refresh_ctx).state, "error", "refresh HTTP failure is transient")
session.sign_out()
equal(session.check(refresh_ctx).state, "signed_out", "sign-out state")

local discover_ctx = fake_context()
session.set_auth("fixture-account", "header.valid.signature", "header.refresh.signature")
discover_ctx.queue(fixtures.query_files)
local search = main.discover.search(discover_ctx, {
    query = "fixture",
    sort = "trend_week",
    page = 1,
    tags = { "Scene" },
})
equal(#search.items, 1, "search item count")
equal(search.items[1].id, "3765064055", "search item id")
equal(search.items[1].wp_type, "scene", "search item type")
request = discover_ctx.last_request()
equal(request.method, "GET", "QueryFiles method")
equal(request.query_data.access_token, "header.valid.signature", "QueryFiles access token")
equal(request.query_data.appid, "431960", "QueryFiles appid")
local request_count = discover_ctx.request_count()
local details = main.discover.details(discover_ctx, "3765064055")
equal(details.size, "4096", "cached details size")
equal(discover_ctx.request_count(), request_count, "search metadata must be reused")
discover_ctx.queue(fixtures.file_details)
equal(main.discover.details(discover_ctx, "fallback").size, "8192", "fallback details")

local subscription_ctx = fake_context()
local states = subscription.status(subscription_ctx, { "3765064055", "other" })
equal(states["3765064055"], "unknown", "authoritative status gate")
equal(states.other, "unknown", "unknown status")
equal(subscription_ctx.request_count(), 0, "unknown status must not probe another endpoint")

subscription_ctx.queue(fixtures.mutation_accepted)
equal(subscription.subscribe(subscription_ctx, "3765064055").accepted, true, "subscribe accepted")
request = subscription_ctx.last_request()
equal(request.method, "POST", "subscribe method")
equal(request.form_data.access_token, "header.valid.signature", "subscribe access token")
equal(request.form_data.publishedfileid, "3765064055", "subscribe item id")
equal(request.form_data.list_type, "1", "subscribe list type")
equal(request.form_data.notify_client, "1", "subscribe notify client")

subscription_ctx.queue({}, 403)
local unsubscribe_ok, unsubscribe_error = pcall(
    subscription.unsubscribe, subscription_ctx, "3765064055")
equal(unsubscribe_ok, false, "unconfirmed unsubscribe response")
request = subscription_ctx.last_request()
truthy(request.url:find("/Unsubscribe/v1/", 1, true) ~= nil, "unsubscribe endpoint")
equal(request.form_data.access_token, "header.valid.signature", "unsubscribe access token")
equal(request.form_data.publishedfileid, "3765064055", "unsubscribe item id")
equal(request.form_data.list_type, "1", "unsubscribe list type")
equal(request.form_data.notify_client, "1", "unsubscribe notify client")
truthy(not tostring(unsubscribe_error):find("header.valid.signature", 1, true),
    "unsubscribe error leaked access token")

subscription_ctx.queue({}, 401)
local ok, err = pcall(subscription.subscribe, subscription_ctx, "3765064055")
equal(ok, false, "subscription HTTP error")
truthy(not tostring(err):find("header.valid.signature", 1, true), "error leaked access token")

local refreshed_subscription_ctx = fake_context()
session.set_auth("fixture-account", "header.expired.signature", "header.refresh.signature")
refreshed_subscription_ctx.queue(fixtures.token_refresh)
refreshed_subscription_ctx.queue(fixtures.mutation_accepted)
equal(subscription.subscribe(refreshed_subscription_ctx, "refresh-item").accepted, true,
    "subscribe after token refresh")
equal(refreshed_subscription_ctx.request_count(), 2, "subscribe refresh request count")
truthy(refreshed_subscription_ctx.request_at(1).url:find(
    "/GenerateAccessTokenForApp/v1/", 1, true) ~= nil, "subscribe refresh endpoint")
truthy(refreshed_subscription_ctx.request_at(2).url:find(
    "/Subscribe/v1/", 1, true) ~= nil, "subscribe endpoint after refresh")

local refreshed_status_ctx = fake_context()
session.set_auth("fixture-account", "header.expired.signature", "header.refresh.signature")
refreshed_status_ctx.queue(fixtures.token_refresh)
local refreshed_states = subscription.status(refreshed_status_ctx, { "refresh-item" })
equal(refreshed_states["refresh-item"], "unknown", "status after token refresh")
equal(refreshed_status_ctx.request_count(), 1, "status only refreshes while endpoint is unknown")

session.sign_out()
local signed_out_ctx = fake_context()
local signed_out_ok, signed_out_error = pcall(
    subscription.status, signed_out_ctx, { "3765064055" })
equal(signed_out_ok, false, "sign-out invalidates subscription access")
equal(signed_out_ctx.request_count(), 0, "signed-out status must not call Steam")
truthy(not tostring(signed_out_error):find("header.", 1, true),
    "signed-out status leaked token")

local project = import("wallpaper_engine.project")
local original_classify = project.classify
local classify_calls = 0
project.classify = function(...)
    classify_calls = classify_calls + 1
    return original_classify(...)
end
equal(classify_calls, 0, "remote calls must not classify local projects")

local steam_root = "/fixture/steam"
local workshop = steam_root .. project.WORKSHOP
local item_dir = workshop .. "/3765064055"
local source_ctx = {
    libraries = function() return { steam_root } end,
    fs = {
        exists = function(path)
            return path == workshop
                or path == item_dir .. "/project.json"
                or path == item_dir .. "/scene.pkg"
                or path == item_dir .. "/preview.jpg"
        end,
        list_dirs = function(path)
            return path == workshop and { item_dir } or {}
        end,
        basename = function(path) return path:match("([^/]+)$") end,
        read = function(path)
            if path == item_dir .. "/project.json" then return "fixture-project" end
            return nil
        end,
        glob = function() return {} end,
        extension = function(path) return path:match("%.([^./]+)$") end,
    },
    json = { parse = function(value)
        if value == "fixture-project" then
            return {
                title = "Local fixture",
                type = "Scene",
                file = "scene.json",
                preview = "preview.jpg",
            }
        end
        return nil
    end },
    log = function() end,
}
local source_items = main.source.scan(source_ctx)
equal(classify_calls, 1, "explicit source scan must classify local projects")
equal(#source_items, 1, "source scan item count")
equal(source_items[1].resource, item_dir .. "/scene.pkg", "source scan resource")
project.classify = original_classify

print("OWE waywallen Lua contract fixtures passed")
