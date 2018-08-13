settings = {
    test = {
        bind_address = "127.0.0.1",
        port = 8081,
        test = {
            input = "http://127.0.0.1:8080/",
            formats = { "mkv", "hls", "dash" }
        }
    }
}
