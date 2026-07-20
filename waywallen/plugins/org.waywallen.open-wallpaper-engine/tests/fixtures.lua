return {
    qr_begin = {
        response = {
            client_id = "123456789",
            request_id = "cmVxdWVzdA==",
            challenge_url = "https://s.team/q/example",
            interval = 1.5,
        },
    },
    qr_pending = {
        response = {
            had_remote_interaction = false,
        },
    },
    qr_confirmation = {
        response = {
            had_remote_interaction = true,
        },
    },
    qr_rotation = {
        response = {
            new_client_id = "987654321",
            new_challenge_url = "https://s.team/q/rotated",
            had_remote_interaction = false,
        },
    },
    qr_success = {
        response = {
            account_name = "fixture-account",
            access_token = "header.valid.signature",
            refresh_token = "header.refresh.signature",
        },
    },
    token_refresh = {
        response = {
            access_token = "header.valid.signature",
            refresh_token = "header.refresh2.signature",
        },
    },
    query_files = {
        response = {
            total = 1,
            publishedfiledetails = {
                {
                    publishedfileid = "3765064055",
                    title = "Fixture wallpaper",
                    preview_url = "https://example.invalid/preview.jpg",
                    short_description = "Fixture description",
                    file_size = "4096",
                    subscriptions = 42,
                    can_subscribe = true,
                    tags = {
                        { tag = "Scene" },
                        { tag = "Everyone" },
                    },
                },
            },
        },
    },
    file_details = {
        response = {
            publishedfiledetails = {
                {
                    publishedfileid = "fallback",
                    title = "Fallback wallpaper",
                    description = "Fallback description",
                    file_size = "8192",
                    tags = { { tag = "Video" } },
                },
            },
        },
    },
    mutation_accepted = { response = {} },
}
