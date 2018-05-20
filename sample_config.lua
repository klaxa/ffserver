-- Sample configuration file for ffserver
-- Contains all settings
settings = {
    -- A server instance
    default_server = {
        -- Server settings
        bind_address = "0.0.0.0",
        port = 8080,
        -- Stream settings
        default_stream = {
            input = "default.mkv",
            formats = { "mkv" }, -- later possible: { "mkv", "hls", "dash" }
        },
        other_stream = {
            input = "other_file.mkv",
            formats = { "mkv" }
        }
    },
    -- Another server instance
    other_server = {
        bind_address = "127.0.0.1",
        port = 8081,
        default_restream = {
            input = "yet_another_file.mkv",
            formats = { "mkv" }
        }
    }
}
