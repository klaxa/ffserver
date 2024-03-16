# FFserver

----

This project aims to replace the hard to configure and maintain old FFserver component of the FFmpeg project.

Core goals include only using the public API of the FFmpeg-libraries to ease maintainability and use an easy to understand configuration file format.

## Building

To build the project a Makefile is supplied. No additional configuration system is in place (yet).

### Dependencies

To successfully build the project the development files of the following projects must be present:

 - ffmpeg (libav*)
 - lua
 - libmicrohttpd

The versions building was tested on are:

 - ffmpeg 6.0 or newer
 - lua 5.4
 - libmicrohttpd 1.0.0


After successfully building the project with ```make``` it can be used with a configuration file.

## Configuration

A configuration file is a lua file that is evaluated and the resulting object is used to configure servers. A single instance can start multiple servers. An example configuration file containing only one server and one stream could look like this:

```
settings = {
    default_server = {
        bind_address = "0.0.0.0",
        port = 8080,
        default_stream = {
            input = "default.mkv",
            formats = { "mkv", "hls", "dash" }
        }
}
```

Note that multiple streams can be configured for a single server and multiple servers can be defined in a single configuration file.

A sample configuration configuring multiple streams and servers is present as `sample_config.lua`.

## Accessing a stream

Accessing a stream is done with an HTTP client. To receive a stream, request the server name, the stream name and the format in an HTTP GET request. To receive the above stream  as matroska the request would read:

```
GET /default_server/default_stream/mkv
```

or for HLS:

```
GET /default_server/default_stream/hls
```


## Limitations/known issues

 - Input files are not validated for muxability (mostly an issue for HLS and DASH)
 - Client side "unclean" disconnect causes memory leak


## Technical Details

More technical details about the implementation can be found in Technical.txt.
