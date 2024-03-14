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

#include "fileserver.h"
#include "httpd.h"

#include <stdio.h>
#include <pthread.h>

#include <libavutil/log.h>
#include <libavutil/file.h>
#include <libavutil/error.h>

void fileserver_schedule(struct FileserverContext *fs, struct FFServerInfo *ffinfo, const char *filename)
{
    int i, ret;
    char final_filename[1024];
    struct FileserverClient *fsc = NULL;
    for (i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_lock(&fs->clients[i].client_lock);
        if (!fs->clients[i].ffinfo) {
            fsc = &fs->clients[i];
            break;
        }
        pthread_mutex_unlock(&fs->clients[i].client_lock);
    }
    // fsc still locked
    if (fsc) {
        fsc->ffinfo = ffinfo;
        snprintf(final_filename, 1024, "%s/%s", fs->server_name, filename);
        ret = av_file_map(final_filename, &fsc->buf, &fsc->size, 0, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not read file: %s: %s.\n", final_filename, av_err2str(ret));
            fsc->ffinfo->httpd->close(fsc->ffinfo->server, fsc->ffinfo->client);
            av_freep(&fsc->ffinfo);
            fsc->buf = NULL;
            fsc->size = 0;
        }
        pthread_mutex_unlock(&fsc->client_lock);
    } else {
        av_log(NULL, AV_LOG_WARNING, "Could not find free client slot.\n");
        ffinfo->httpd->close(ffinfo->server, ffinfo->client);
        av_free(ffinfo);
    }
    av_log(NULL, AV_LOG_INFO, "Scheduled serving file: %s\n", final_filename);
    return;
}

void fileserver_init(struct FileserverContext **fs_p, const char *server_name)
{
    struct FileserverContext *fs = av_mallocz(sizeof(struct FileserverContext));
    int i;
    *fs_p = NULL;

    if (!fs) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate fileserver context.\n");
        return;
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
        fs->clients[i].ffinfo = NULL;
        fs->clients[i].buf = NULL;
        fs->clients[i].size = 0;
        pthread_mutex_init(&fs->clients[i].client_lock, NULL);
    }

    fs->nb_threads = 1;
    fs->server_name = server_name;
    fs->shutdown = 0;
    *fs_p = fs;
    return;
}

void fileserver_free(struct FileserverContext *fs) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_lock(&fs->clients[i].client_lock);
        av_free(fs->clients[i].ffinfo);
        av_free(fs->clients[i].buf);
        pthread_mutex_unlock(&fs->clients[i].client_lock);
    }
    av_free(fs);
}
