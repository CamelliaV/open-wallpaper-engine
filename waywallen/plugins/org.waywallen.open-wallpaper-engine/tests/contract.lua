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
    local encoded_values = {}
    local now = 1000
    local sleep_calls = 0
    local html_query_calls = 0
    local decoded = {
        qr_access = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web:community" },
        },
        refresh = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web", "renew", "derive" },
        },
        community = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web:community" },
        },
        store = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web:store" },
        },
        expired_refresh = {
            exp = 1,
            sub = fixtures.steamid,
            aud = { "web", "derive" },
        },
        no_derive = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web", "renew" },
        },
        ["transfer-ok"] = { result = 1 },
    }

    local function url_host(url)
        return url:match("^https://([^/:?#]+)")
    end

    local function domain_matches(host, domain)
        domain = domain:gsub("^%.", ""):lower()
        host = (host or ""):lower()
        return host == domain or host:sub(-#domain - 1) == "." .. domain
    end

    local function request(client, method, url)
        local value = { client = client, method = method, url = url }
        function value:form(form)
            self.form_data = form
            return self
        end
        function value:multipart(form)
            self.multipart_data = form
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
            local final_url = fixture.url or self.url
            for _, cookie in ipairs(fixture.cookies or {}) do
                client:set_cookie(final_url, cookie)
            end
            local response = {
                status_code = fixture.status or 200,
                json_body = fixture.json,
                text_body = fixture.text or "",
                final_url = final_url,
            }
            function response:ok()
                return self.status_code >= 200 and self.status_code < 300
            end
            function response:status() return self.status_code end
            function response:json() return self.json_body end
            function response:text() return self.text_body end
            function response:url() return self.final_url end
            return response
        end
        return value
    end

    local function new_client()
        local client = { cookies = {} }
        function client:get(url) return request(self, "GET", url) end
        function client:post(url) return request(self, "POST", url) end
        function client:set_cookie(url, value)
            local name, cookie_value = value:match("^([^=;]+)=([^;]*)")
            local domain = value:match("[Dd][Oo][Mm][Aa][Ii][Nn]=([^;]+)") or url_host(url)
            if not name or not domain then error("invalid fake cookie") end
            self.cookies[name .. "\0" .. domain] = {
                name = name,
                value = cookie_value,
                domain = domain,
            }
        end
        function client:cookie(url, name)
            local host = url_host(url)
            for _, cookie in pairs(self.cookies) do
                if cookie.name == name and domain_matches(host, cookie.domain) then
                    return cookie.value
                end
            end
            return nil
        end
        function client:clear_cookies() self.cookies = {} end
        return client
    end

    local ctx = {
        http = new_client(),
        log = function() end,
    }
    ctx.time = {
        unix = function() return 2 end,
        now = function() return now end,
        sleep = function(milliseconds)
            sleep_calls = sleep_calls + 1
            now = now + milliseconds
        end,
    }
    ctx.random = { hex = function(bytes) return string.rep("ab", bytes) end }
    ctx.base64 = { decode = function(value) return value end }
    ctx.json = {
        parse = function(value)
            local result = decoded[value]
            if result == nil then error("invalid fixture JSON") end
            return result
        end,
        encode = function(value)
            table.insert(encoded_values, value)
            return "fixture-json"
        end,
    }
    ctx.url = {
        host = function(value) return value:match("^https?://([^/:?#]+)") end,
        parse = function(value)
            local scheme, host, path = value:match("^(https?)://([^/:?#]+)(.*)$")
            if not scheme then return nil end
            return { scheme = scheme, host = host, path = path }
        end,
        encode_component = function(value)
            return tostring(value):gsub("([^%w%-_%.~])", function(char)
                return string.format("%%%02X", string.byte(char))
            end)
        end,
        decode_component = function(value)
            return value:gsub("+", " "):gsub("%%(%x%x)", function(hex)
                return string.char(tonumber(hex, 16))
            end)
        end,
    }
    local function query_one(html, selector)
            local id = selector:match("#([%w_%-]+)")
            if not id or not html:find('id="' .. id .. '"', 1, true) then return nil end
            local class = html:match('id="' .. id .. '"[^>]-class="([^"]*)"') or ""
            for required in selector:gmatch("%.([%w_%-]+)") do
                if selector:find(":not(." .. required .. ")", 1, true) == nil
                    and not class:match("%f[%w]" .. required .. "%f[%W]") then return nil end
            end
            if selector:find(":not(.subscribed)", 1, true)
                and class:match("%f[%w]subscribed%f[%W]") then return nil end
            return { attrs = { id = id, class = class } }
    end
    ctx.html = {
        query_one = query_one,
        query = function(html, selector)
            html_query_calls = html_query_calls + 1
            local result = {}
            for part in selector:gmatch("[^,]+") do
                local element = query_one(html, part)
                if element then table.insert(result, element) end
            end
            return result
        end,
    }
    function ctx.queue(json, status, text, url, cookies)
        table.insert(queued, {
            json = json,
            status = status,
            text = text,
            url = url,
            cookies = cookies,
        })
    end
    function ctx.queue_response(fixture) table.insert(queued, fixture) end
    function ctx.last_request() return requests[#requests] end
    function ctx.request_at(index) return requests[index] end
    function ctx.request_count() return #requests end
    function ctx.queued_count() return #queued end
    function ctx.encoded_at(index) return encoded_values[index] end
    function ctx.cookie_snapshot()
        local snapshot = {}
        for key, value in pairs(ctx.http.cookies) do
            snapshot[key] = {
                name = value.name,
                value = value.value,
                domain = value.domain,
            }
        end
        return snapshot
    end
    function ctx.restore_cookies(snapshot)
        ctx.http.cookies = snapshot
    end
    function ctx.sleep_count() return sleep_calls end
    function ctx.html_query_count() return html_query_calls end
    function ctx.advance(milliseconds) now = now + milliseconds end
    return ctx
end

local function queue_web_session(ctx, finalize)
    ctx.queue(finalize or fixtures.web_finalize)
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://steamcommunity.com/login/settoken",
        cookies = { fixtures.community_cookie },
    })
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://store.steampowered.com/login/settoken",
        cookies = { fixtures.store_cookie },
    })
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://help.steampowered.com/login/settoken",
        cookies = { fixtures.help_cookie },
    })
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://checkout.steampowered.com/login/settoken",
        cookies = { fixtures.checkout_cookie },
    })
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://steam.tv/login/settoken",
        cookies = { fixtures.tv_cookie },
    })
end

local main = assert(loadfile(plugin_root .. "/main.lua", "t", _ENV))()
local info = main.info()
equal(info.display_name, "Workshop", "display name")
equal(info.capabilities.discover.subscription, true, "subscription capability")
equal(info.capabilities.discover.download, nil, "download capability must be absent")
equal(info.capabilities.discover.resolve, nil, "resolve capability must be absent")
equal(info.capabilities.discover.remote_hint, nil, "remote hint must be absent")
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
equal(request.form_data.input_json, "fixture-json", "QR input_json envelope")
equal(request.header_data.Origin, "https://steamcommunity.com", "QR origin")
local qr_input = auth_ctx.encoded_at(1)
equal(qr_input.website_id, nil, "QR website id must not be sent")
equal(qr_input.platform_type, nil, "QR platform must be nested")
equal(qr_input.device_details.platform_type, 2, "QR WebBrowser platform")
equal(qr_input.device_details.device_friendly_name, "waywallen", "QR device name")

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
local qr_state = session.save()
truthy(qr_state:find("header.qr_access.signature", 1, true), "QR access token not saved")
truthy(qr_state:find("header.refresh.signature", 1, true), "QR refresh token not saved")
local invalid_auth_ok = pcall(
    session.set_auth,
    auth_ctx,
    "other-account",
    "header.qr_access.signature",
    "header.no_derive.signature"
)
equal(invalid_auth_ok, false, "refresh audience validation")
equal(session.account_name(), "fixture-account", "invalid auth must not replace session")

session.set_auth(
    auth_ctx,
    "line one\nline two",
    "header.qr_access.signature",
    "header.refresh.signature"
)
local serialized = session.save()
session.sign_out()
session.load(serialized)
equal(session.account_name(), "line one\nline two", "opaque state round trip")
truthy(session.signed_in(), "v4 session did not restore")

session.load(table.concat({
    "steam-session-v2",
    "line-format-account",
    fixtures.steamid,
    "header.qr_access.signature",
    "header.refresh.signature",
}, "\n"))
equal(session.account_name(), "line-format-account", "line state compatibility")
equal(session.signed_in(), false, "Client state must require a new sign-in")
equal(session.check(auth_ctx).state, "expired", "Client state lifecycle")
local migrated = session.migrate(
    "waywallen-steam-session-v1",
    '{"account_name":"escaped\\nname","access_token":"secret",'
        .. '"refresh_token":"secret","steamid":"' .. fixtures.steamid .. '"}'
)
truthy(not migrated:find("secret", 1, true), "legacy token must not migrate")
session.sign_out()
session.load(migrated)
equal(session.account_name(), "escaped\nname", "legacy JSON migration")
equal(session.check(auth_ctx).state, "expired", "migrated lifecycle")

local web_ctx = fake_context()
session.set_auth(web_ctx, "fixture-account", "header.qr_access.signature", "header.refresh.signature")
equal(session.check(web_ctx).state, "signed_in", "local Web lifecycle")
equal(web_ctx.request_count(), 0, "lifecycle check must not create a Web session")
queue_web_session(web_ctx)
equal(session.ensure_access_token(web_ctx), "header.community.signature", "Community token")
equal(web_ctx.request_count(), 6, "all Web transfers")
request = web_ctx.request_at(1)
truthy(request.url:find("/jwt/finalizelogin", 1, true), "finalization endpoint")
equal(request.multipart_data.nonce, "header.refresh.signature", "finalization nonce")
equal(#request.multipart_data.sessionid, 24, "sessionid length")
equal(request.header_data.Origin, "https://steamcommunity.com", "finalization origin")
equal(web_ctx.request_at(2).multipart_data.steamID, fixtures.steamid, "transfer SteamID")
for index = 3, 6 do
    equal(web_ctx.request_at(index).multipart_data.steamID, fixtures.steamid, "all transfer SteamIDs")
end
equal(
    web_ctx.http:cookie("https://steamcommunity.com/", "sessionid"),
    request.multipart_data.sessionid,
    "shared Community sessionid"
)

local finalized_state = session.save()
session.load(finalized_state)
local restart_ctx = fake_context()
restart_ctx.restore_cookies(web_ctx.cookie_snapshot())
equal(session.ensure_access_token(restart_ctx), "header.community.signature", "restart restore")
equal(restart_ctx.request_count(), 0, "restart must reuse Community token")
equal(
    restart_ctx.http:cookie("https://steamcommunity.com/", "steamLoginSecure"),
    fixtures.steamid .. "%7C%7Cheader.community.signature",
    "restored Community cookie"
)

local retry_ctx = fake_context()
session.set_auth(retry_ctx, "fixture-account", "header.qr_access.signature", "header.refresh.signature")
retry_ctx.queue(fixtures.web_finalize)
retry_ctx.queue({}, 503)
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://steamcommunity.com/login/settoken",
    cookies = { fixtures.community_cookie },
})
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://store.steampowered.com/login/settoken",
    cookies = { fixtures.store_cookie },
})
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://help.steampowered.com/login/settoken",
    cookies = { fixtures.help_cookie },
})
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://checkout.steampowered.com/login/settoken",
    cookies = { fixtures.checkout_cookie },
})
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://steam.tv/login/settoken",
    cookies = { fixtures.tv_cookie },
})
equal(session.ensure_access_token(retry_ctx), "header.community.signature", "transient transfer retry")
equal(retry_ctx.request_count(), 7, "transfer retry request count")
equal(retry_ctx.sleep_count(), 1, "transfer retry delay")

local exhausted_ctx = fake_context()
session.set_auth(
    exhausted_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
exhausted_ctx.queue(fixtures.web_finalize)
for _ = 1, 5 do exhausted_ctx.queue({}, 503) end
local exhausted_ok = pcall(session.ensure_access_token, exhausted_ctx)
equal(exhausted_ok, false, "exhausted transfer retries")
equal(exhausted_ctx.request_count(), 6, "transfer retry limit")
equal(exhausted_ctx.sleep_count(), 4, "transfer retry delay limit")

local expired_ctx = fake_context()
session.set_auth(expired_ctx, "fixture-account", "header.qr_access.signature", "header.refresh.signature")
expired_ctx.queue({}, 403)
local expired_ok, expired_error = pcall(session.ensure_access_token, expired_ctx)
equal(expired_ok, false, "finalization auth failure")
truthy(tostring(expired_error):find("expired; sign in again", 1, true), "auth failure message")
local transient_ctx = fake_context()
session.set_auth(
    transient_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
transient_ctx.queue({}, 503)
local transient_ok = pcall(session.ensure_access_token, transient_ctx)
equal(transient_ok, false, "finalization HTTP failure")

local discover_ctx = fake_context()
session.set_auth(
    discover_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
queue_web_session(discover_ctx)
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
equal(request.query_data.access_token, "header.community.signature", "QueryFiles Community token")
equal(request.query_data.appid, "431960", "QueryFiles appid")
local request_count = discover_ctx.request_count()
local details = main.discover.details(discover_ctx, "3765064055")
equal(details.size, "4096", "cached details size")
equal(discover_ctx.request_count(), request_count, "search metadata must be reused")
discover_ctx.queue(fixtures.file_details)
equal(main.discover.details(discover_ctx, "fallback").size, "8192", "fallback details")

local subscription_ctx = fake_context()
session.set_auth(
    subscription_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
queue_web_session(subscription_ctx)
subscription_ctx.queue(nil, 200, fixtures.subscribed_page)
local states = subscription.status(subscription_ctx, { "3765064055" })
equal(states["3765064055"], "subscribed", "subscribed page state")
equal(subscription_ctx.html_query_count(), 1, "subscription page parse count")
request = subscription_ctx.last_request()
equal(request.header_data["Sec-Fetch-Mode"], "navigate", "detail navigation headers")
equal(request.header_data["Sec-Fetch-Site"], "none", "detail navigation origin")
request_count = subscription_ctx.request_count()
equal(subscription.status(subscription_ctx, { "3765064055" })["3765064055"], "subscribed",
    "cached page state")
equal(subscription_ctx.request_count(), request_count, "status cache request count")
subscription_ctx.advance(60001)
subscription_ctx.queue(nil, 200, fixtures.unsubscribed_page)
equal(subscription.status(subscription_ctx, { "3765064055" })["3765064055"], "unsubscribed",
    "expired cache refresh")
subscription_ctx.queue(nil, 200, fixtures.unknown_page)
subscription_ctx.queue(nil, 200, fixtures.conflict_page)
states = subscription.status(subscription_ctx, { "missing-markers", "conflicting-markers" })
equal(states["missing-markers"], "unknown", "missing page markers")
equal(states["conflicting-markers"], "unknown", "conflicting page markers")

local partial_ctx = fake_context()
session.set_auth(partial_ctx, "fixture-account", "header.qr_access.signature", "header.refresh.signature")
queue_web_session(partial_ctx)
partial_ctx.queue(nil, 200, fixtures.subscribed_page)
partial_ctx.queue({}, 503)
states = subscription.status(partial_ctx, { "subscribed", "failed" })
equal(states.subscribed, "subscribed", "batch successful item")
equal(states.failed, "unknown", "batch failed item")

local login_retry_ctx = fake_context()
session.set_auth(
    login_retry_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
queue_web_session(login_retry_ctx)
login_retry_ctx.queue(nil, 200, fixtures.signed_out_page)
queue_web_session(login_retry_ctx)
login_retry_ctx.queue(nil, 200, fixtures.subscribed_page)
equal(subscription.status(login_retry_ctx, { "retry" }).retry, "subscribed", "login page retry")

local mutation_ctx = fake_context()
session.set_auth(mutation_ctx, "fixture-account", "header.qr_access.signature", "header.refresh.signature")
queue_web_session(mutation_ctx)
mutation_ctx.queue(fixtures.mutation_accepted)
equal(subscription.subscribe(mutation_ctx, "3765064055").accepted, true, "subscribe accepted")
request = mutation_ctx.last_request()
equal(request.form_data.access_token, "header.community.signature", "subscribe Community token")
equal(request.form_data.publishedfileid, "3765064055", "subscribe item id")
request_count = mutation_ctx.request_count()
equal(subscription.status(mutation_ctx, { "3765064055" })["3765064055"], "subscribed",
    "mutation grace state")
equal(mutation_ctx.request_count(), request_count, "mutation grace must avoid page request")
mutation_ctx.advance(30001)
mutation_ctx.queue(nil, 200, fixtures.unsubscribed_page)
equal(subscription.status(mutation_ctx, { "3765064055" })["3765064055"], "unsubscribed",
    "server state corrects mutation expectation")

local retry_mutation_ctx = fake_context()
session.set_auth(
    retry_mutation_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
queue_web_session(retry_mutation_ctx)
retry_mutation_ctx.queue({}, 403)
queue_web_session(retry_mutation_ctx)
retry_mutation_ctx.queue(fixtures.mutation_accepted)
equal(subscription.unsubscribe(retry_mutation_ctx, "retry-item").accepted, true,
    "mutation re-finalization")
equal(retry_mutation_ctx.request_count(), 14, "mutation auth retry request count")

local failed_mutation_ctx = fake_context()
session.set_auth(
    failed_mutation_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
queue_web_session(failed_mutation_ctx)
failed_mutation_ctx.queue({}, 500)
local mutation_ok, mutation_error = pcall(
    subscription.subscribe,
    failed_mutation_ctx,
    "failed-item"
)
equal(mutation_ok, false, "failed mutation")
truthy(not tostring(mutation_error):find("header.community.signature", 1, true),
    "mutation error leaked token")

main.actions.invoke(failed_mutation_ctx, "steam_sign_out")
equal(
    failed_mutation_ctx.http:cookie("https://steamcommunity.com/", "steamLoginSecure"),
    nil,
    "sign-out clears the host Community session"
)
local signed_out_ctx = fake_context()
local signed_out_ok, signed_out_error = pcall(
    subscription.status,
    signed_out_ctx,
    { "3765064055" }
)
equal(signed_out_ok, false, "sign-out invalidates subscription access")
equal(signed_out_ctx.request_count(), 0, "signed-out status must not call Steam")
truthy(not tostring(signed_out_error):find("header.", 1, true), "signed-out error leaked token")

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
        list_dirs = function(path) return path == workshop and { item_dir } or {} end,
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
