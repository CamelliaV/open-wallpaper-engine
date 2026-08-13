lito.install({
    artifacts = {
        {
            target = { kind = "bin", name = "waywallen-weweb-renderer" },
            destination = "bin/weweb/waywallen-weweb-renderer",
        },
    },
    external_assets = {
        {
            dependency = "cef",
            set = "runtime",
            destination = "bin/weweb",
        },
    },
})
