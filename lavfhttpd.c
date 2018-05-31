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
 
#ifndef LAVFHTTPD_H
#define LAVFHTTPD_H

#include "httpd.h"
#include <libavutil/opt.h>


int lavfhttpd_init(void **server, struct HTTPDConfig config)
{
    char out_uri[1024];
    int ret;
    AVDictionary *opts = NULL;
    AVIOContext *server_ctx = NULL;
    
    snprintf(out_uri, 1024, "http://%s:%d", config.bind_address, config.port);
    
    avformat_network_init();
    
    if ((ret = av_dict_set(&opts, "listen", "2", 0)) < 0) {
        av_log(opts, AV_LOG_ERROR, "Failed to set listen mode for server: %s\n", av_err2str(ret));
        av_free(opts);
        return -1;
    }
    
    if ((ret = av_dict_set_int(&opts, "listen_timeout", config.accept_timeout, 0)) < 0) {
        av_log(opts, AV_LOG_ERROR, "Failed to set listen_timeout for server: %s\n", av_err2str(ret));
        av_free(opts);
        return -1;
    }
    
    if ((ret = avio_open2(&server_ctx, out_uri, AVIO_FLAG_WRITE, NULL, &opts)) < 0) {
        av_log(server, AV_LOG_ERROR, "Failed to open server: %s\n", av_err2str(ret));
        av_free(opts);
        return -1;
    }
    av_free(opts);
    
    *server = server_ctx;
    return 0;
}

int lavfhttpd_accept(void *server, struct HTTPClient **client, int reply_code)
{
    AVIOContext *server_ctx = (AVIOContext*) server;
    AVIOContext *client_ctx = NULL;
    struct HTTPClient *client_http = NULL;
    int ret, ret2, handshake;
    int reply_code2 = reply_code;
    char *method, *resource;
    if ((ret = avio_accept(server_ctx, &client_ctx)) < 0) {
        if (ret == AVERROR(ETIMEDOUT)) {
            return HTTPD_LISTEN_TIMEOUT;
        } else {
            if (client_ctx)
                avio_context_free(&client_ctx);
            return HTTPD_OTHER_ERROR;
        }
    }
    client_ctx->seekable = 0;
    ret2 = HTTPD_OK;
    client_http = av_malloc(sizeof(*client_http));
    if (!client_http) {
        av_log(server, AV_LOG_ERROR, "Could not allocate http client.\n");
        return HTTPD_OTHER_ERROR;
    }
    client_http->method = NULL;
    client_http->resource = NULL;
    client_http->httpd_data = client_ctx;
    while ((handshake = avio_handshake(client_ctx)) > 0) {
        av_opt_get(client_ctx, "method", AV_OPT_SEARCH_CHILDREN, (uint8_t**) &method);
        av_opt_get(client_ctx, "resource", AV_OPT_SEARCH_CHILDREN, (uint8_t**) &resource);
        av_log(client_ctx, AV_LOG_DEBUG, "method: %s resource: %s\n", method, resource);
        if (method && strlen(method) && strncmp("GET", method, 3)) {
            ret2 = HTTPD_CLIENT_ERROR;
            reply_code2 = 400;
            if ((ret = av_opt_set_int(client_ctx, "reply_code", reply_code2, AV_OPT_SEARCH_CHILDREN)) < 0) {
                av_log(client_ctx, AV_LOG_WARNING, "Failed to set reply_code: %s.\n", av_err2str(ret));
            }
        }
        av_free(client_http->method);
        av_free(client_http->resource);
        client_http->method = av_strdup(method);
        client_http->resource = av_strdup(resource);
        av_free(method);
        av_free(resource);
    }
    if (handshake < 0) {
        ret2 = HTTPD_CLIENT_ERROR;
        reply_code2 = 400;
    }
    
    if ((ret = av_opt_set_int(client_ctx, "reply_code", reply_code2, AV_OPT_SEARCH_CHILDREN)) < 0) {
        av_log(client_ctx, AV_LOG_WARNING, "Failed to set reply_code: %s.\n", av_err2str(ret));
    }
    
    *client = client_http;
    return ret2;
}

int lavfhttpd_write(void *server, struct HTTPClient *client, const unsigned char *buf, int size)
{
    AVIOContext *client_ctx = (AVIOContext*) client->httpd_data;
    int64_t old_written = client_ctx->written;
    int64_t actual_written;
    avio_write(client_ctx, buf, size);
    avio_flush(client_ctx);
    actual_written = client_ctx->written - old_written;
    if (actual_written < size) {
        return AVERROR_EOF;
    }
    return size;
}

int lavfhttpd_read(void *server, struct HTTPClient *client, unsigned char *buf, int size)
{
    AVIOContext *client_ctx = (AVIOContext*) client->httpd_data;
    return avio_read(client_ctx, buf, size);
}

void lavfhttpd_close(void *server, struct HTTPClient *client)
{
    AVIOContext *client_ctx = (AVIOContext*) client->httpd_data;
    avio_close(client_ctx);
    av_free(client->method);
    av_free(client->resource);
    av_free(client);
}

void lavfhttpd_shutdown(void *server)
{
    AVIOContext *server_ctx = (AVIOContext*) server;
    avio_close(server_ctx);
    avformat_network_deinit();
}

struct HTTPDInterface lavfhttpd = {
    .init = lavfhttpd_init,
    .accept = lavfhttpd_accept,
    .write = lavfhttpd_write,
    .close = lavfhttpd_close,
    .shutdown = lavfhttpd_shutdown,
};

#endif
