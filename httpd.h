/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef HTTPDINTERFACE_H
#define HTTPDINTERFACE_H

#define HTTPD_OK 0
#define HTTPD_LISTEN_TIMEOUT -1
#define HTTPD_CLIENT_ERROR -2
#define HTTPD_OTHER_ERROR -3

/** Supported stream formats */
enum StreamFormat {
    FMT_MATROSKA = 0,
    FMT_HLS,
    FMT_DASH,
    FMT_NB,
};

/** Stream Config struct */
struct StreamConfig {
    char *stream_name;
    char *input_uri;
    enum StreamFormat *formats;
    int nb_formats;
};

/** HTTPD Config struct */
struct HTTPDConfig {
    char *server_name;
    char *bind_address;
    int port;
    int accept_timeout;
    struct StreamConfig *streams;
    int nb_streams;
};

/** HTTPClient struct, this information is shared between ffserver and the httpd implementation */
struct HTTPClient {
    /** the method requested by the client, this field has to be set and freed by the httpd implementation */
    char *method;
    /** the resource requested by the client, this field has to be set and freed by the httpd implementation */
    char *resource;
    /** httpd implementation specific data */
    void *httpd_data;
};


/** HTTPDInterface that an httpd implementation must provide */
struct HTTPDInterface {
    int (*init)  (void **server, struct HTTPDConfig config);
    int (*free)  (void *server);
    int (*accept)(void *server, struct HTTPClient **client, const char **valid_files);
    int (*write) (void *server, struct HTTPClient *client, const unsigned char *buf, int size);
    int (*read)  (void *server, struct HTTPClient *client, unsigned char *buf, int size);
    void (*close)(void *server, struct HTTPClient *client);
    void (*shutdown)(void *server);
};

/* struct containing server and client info per client AVIOContext */

struct FFServerInfo {
    struct HTTPDInterface *httpd;
    void *server;
    struct HTTPClient *client;
};


/** Current HTTPDInterface implementation using lavformat */
extern struct HTTPDInterface lavfhttpd;
extern struct HTTPDInterface lmhttpd;
#endif
