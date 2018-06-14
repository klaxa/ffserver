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

#ifndef FILESERVER_H
#define FILESERVER_H

#define MAX_CLIENTS 16

#include "httpd.h"
#include <sys/types.h>

struct FileserverClient {
    struct FFServerInfo *ffinfo;
    pthread_mutex_t client_lock;
    unsigned char *buf;
    size_t size;
};


struct FileserverContext {
    struct FileserverClient clients[MAX_CLIENTS];
    int nb_threads;
    int shutdown;
    const char *server_name;
};

/**
 * Schedule file for sending to a client. It is added to a list of clients.
 * @param fs pointer to the FileserverContext to schedule to
 * @param ffinfo pointer to an FFServerInfo struct containing information for serving the file
 * @param filename filename of the file to serve
 */
void fileserver_schedule(struct FileserverContext *fs, struct FFServerInfo *ffinfo, AVIOContext *ofmt_ctx, const char *filename);

/**
 * Initialize a Fileservercontext
 * @param fs_p pointer to a pointer where the FileserverContext will be allocated
 * @param server_name the name of the server
 */
void fileserver_init(struct FileserverContext **fs_p, const char *server_name);

/**
 * Free a Fileservercontext
 */
void fileserver_free(struct FileserverContext *fs);


#endif
