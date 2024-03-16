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

#ifndef LMHTTPD_H
#define LMHTTPD_H

#define MAX_CLIENTS 16
#define INITIAL_BUFSIZE 16 * 1024 * 1024

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include <microhttpd.h>

#include <libavutil/log.h>
#include <libavutil/fifo.h>
#include <libavutil/mem.h>

#include "httpd.h"

struct MHDServer {
    struct MHD_Daemon *daemon;
    AVFifoBuffer *clients;
};

struct ConnectionInfo {
    AVFifoBuffer *buffer;
    pthread_mutex_t buffer_lock;
    struct MHD_Connection *connection;
    int close_connection;
};


/**
 * Helper callback that fills the libmicrohttpd-client buffer with data.
 */

ssize_t helper_callback(void *cls, uint64_t pos, char *buf, size_t max)
{
    struct ConnectionInfo *cinfo = (struct ConnectionInfo*) cls;
    pthread_mutex_lock(&cinfo->buffer_lock);
    int buf_size = av_fifo_size(cinfo->buffer);
    if (buf_size > 0) {
        max = max > buf_size ? buf_size : max;
        av_fifo_generic_read(cinfo->buffer, buf, max, NULL);
    } else {
        max = 0;
    }
    if (max == 0 && cinfo->close_connection) {
        av_fifo_free(cinfo->buffer);
        pthread_mutex_unlock(&cinfo->buffer_lock);
        av_free(cinfo);
        return MHD_CONTENT_READER_END_OF_STREAM;
    }
    pthread_mutex_unlock(&cinfo->buffer_lock);
    return max;
}

/**
 * Free allocated callback params. Usually passed to MHD_create_response_from_callback, however it frees data too soon.
 */

static void free_callback_param (void *cls)
{
    av_free(cls);
}

/**
 * Callback that handles incoming connections.
 *
 * Incoming connections are initialized and added to a queue of new clients.
 */

static enum MHD_Result answer_to_connection (void *cls, struct MHD_Connection *connection,
                      const char *url, const char *method,
                      const char *version, const char *upload_data,
                      size_t *upload_data_size, void **con_cls)
{
    static int aptr;
    struct MHD_Response *response;
    struct HTTPClient *client;
    int ret;
    struct MHDServer *server = (struct MHDServer*) cls;
    if (&aptr != *con_cls)
    {
        /* do never respond on first call (why? this is in every example... something about keeping track of con_cls?) */
        *con_cls = &aptr;
        return MHD_YES;
    }
    *con_cls = NULL;
    struct ConnectionInfo *cinfo = av_malloc(sizeof(struct ConnectionInfo));
    if (!cinfo)
        return MHD_NO;

    pthread_mutex_init(&cinfo->buffer_lock, NULL);
    if (MHD_set_connection_option(connection, MHD_CONNECTION_OPTION_TIMEOUT, (unsigned int) 10) != MHD_YES)
        return MHD_NO;

    av_log(NULL, AV_LOG_ERROR, "Accepted new client %s %s %p\n", method, url, connection);
    client = av_malloc(sizeof(struct HTTPClient));
    if (!client)
        return MHD_NO;
    // no locking needed, running on the same thread

    if (!strcmp("GET", method)) {
        if (av_fifo_space(server->clients) > sizeof(struct HTTPClient*)) {
            cinfo->buffer = av_fifo_alloc(INITIAL_BUFSIZE);
            cinfo->connection = connection;
            cinfo->close_connection = 0;

            if (!cinfo->buffer)
                return MHD_NO;
            *con_cls = cinfo;
            client->resource = av_strdup(url);
            client->method = av_strdup(method);
            client->httpd_data = cinfo;
            av_fifo_generic_write(server->clients, &client, sizeof(struct HTTPClient*), NULL);
        }
        response = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN,
                                                    1024,
                                                    &helper_callback,
                                                    cinfo,
                                                    NULL);

    } else {
        response = MHD_create_response_from_buffer (0,
                          (void *) "",
                          MHD_RESPMEM_MUST_COPY);
    }
    ret = MHD_queue_response (connection, MHD_HTTP_OK, response);
    MHD_destroy_response (response);

    return ret;
}

/**
 * Initialize the libmicrohttpd server with a config.
 *
 * Allocates and starts daemon.
 */

int lmhttpd_init(void **server, struct HTTPDConfig config) {
    struct MHDServer *server_p = av_malloc(sizeof(struct MHDServer));
    *server = NULL;
    if (!server_p) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate MHDServer struct\n");
        return -1;
    }
    server_p->clients = av_fifo_alloc_array(sizeof(struct HTTPClient), MAX_CLIENTS);
    server_p->daemon = MHD_start_daemon (0, config.port, NULL, NULL,
                             &answer_to_connection, server_p, MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int) 10, MHD_OPTION_END);
    if (!server_p->daemon || !server_p->clients) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate MHD_Daemon\n");
        return -1;
    }
    *server = server_p;
    return 0;
}

/**
 * Return a single new client that connected and run webserver write and read operations.
 *
 * This basically synchronizes the libmicrohttpd client API through a queue.
 */

int lmhttpd_accept(void *server, struct HTTPClient **client, const char **valid_files)
{
    fd_set rs;
    fd_set ws;
    fd_set es;
    struct timeval tv;
    MHD_socket max;
    //MHD_UNSIGNED_LONG_LONG mhd_timeout;
    struct MHDServer *s = (struct MHDServer*) server;

    max = 0;
    FD_ZERO (&rs);
    FD_ZERO (&ws);
    FD_ZERO (&es);

    if (MHD_get_fdset (s->daemon, &rs, &ws, &es, &max) != MHD_YES)
        return HTTPD_OTHER_ERROR;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 0.5 seconds
    if (select (max + 1, &rs, &ws, &es, &tv) == -1)
        return HTTPD_OTHER_ERROR;

    // run read and write operations
    if (MHD_run_from_select(s->daemon, &rs, &ws, &es) != MHD_YES)
        return HTTPD_OTHER_ERROR;
    if(av_fifo_size(s->clients)) {
        av_fifo_generic_read(s->clients, client, sizeof(struct HTTPClient*), NULL);

        return 0;
    }
    return HTTPD_LISTEN_TIMEOUT;
}

/**
 * Write data into a client buffer managed by libmicrohttpd.
 */

int lmhttpd_write(void *server, struct HTTPClient *client, const unsigned char *buf, int size)
{
    struct ConnectionInfo* cinfo = (struct ConnectionInfo*) client->httpd_data;
    int ret;
    pthread_mutex_lock(&cinfo->buffer_lock);
    if (!cinfo->close_connection && av_fifo_space(cinfo->buffer) >= size) {
        ret = av_fifo_generic_write(cinfo->buffer, (void*) buf, size, NULL);

    } else {
        ret = -1;
    }
    pthread_mutex_unlock(&cinfo->buffer_lock);
    if (cinfo->close_connection) {
        free_callback_param(cinfo);
        ret = -1;
    }
    return ret;
}

/**
 * Unimplemented
 */

int lmhttpd_read(void *server, struct HTTPClient *client, unsigned char *buf, int size)
{
    return 0;
}

/**
 * Close a connection by signaling to close it through the ConnectionInfo struct.
 */
void lmhttpd_close(void *server, struct HTTPClient *client)
{
    struct ConnectionInfo* cinfo = (struct ConnectionInfo*) client->httpd_data;
    cinfo->close_connection = 1;
    av_free(client->method);
    av_free(client->resource);
    av_free(client);
}

/**
 * Shutdown the libmicrohttpd daemon.
 */
void lmhttpd_shutdown(void *server)
{
    struct MHDServer *mhd_server = (struct MHDServer*) server;
    fd_set rs;
    fd_set ws;
    fd_set es;
    struct timeval tv;
    MHD_socket max;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 0.5 seconds

    MHD_quiesce_daemon(mhd_server->daemon);

    while (1) {
        max = 0;
        FD_ZERO (&rs);
        FD_ZERO (&ws);
        FD_ZERO (&es);

        if (MHD_get_fdset (mhd_server->daemon, &rs, &ws, &es, &max) != MHD_YES)
            break;

        if (max == 0)
            break;

        if (select (max + 1, &rs, &ws, &es, &tv) == -1)
            break;

        if (MHD_run_from_select(mhd_server->daemon, &rs, &ws, &es) != MHD_YES)
            break;
    }

    MHD_stop_daemon(mhd_server->daemon);
    av_fifo_free(mhd_server->clients);
    av_free(mhd_server);
}

struct HTTPDInterface lmhttpd = {
    .init = lmhttpd_init,
    .accept = lmhttpd_accept,
    .write = lmhttpd_write,
    .close = lmhttpd_close,
    .shutdown = lmhttpd_shutdown,
};

#endif
