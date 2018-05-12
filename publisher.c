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

#include "publisher.h"
#include "segment.h"
#include <libavutil/log.h>

void client_log(struct Client *c)
{
    av_log(NULL, AV_LOG_INFO, "State: ");
    switch(c->state) {
    case FREE:
        av_log(NULL, AV_LOG_INFO, "FREE\n");
        break;
    case RESERVED:
        av_log(NULL, AV_LOG_INFO, "RESERVED\n");
        break;
    case WAIT:
        av_log(NULL, AV_LOG_INFO, "WAIT\n");
        break;
    case WRITABLE:
        av_log(NULL, AV_LOG_INFO, "WRITABLE\n");
        break;
    case BUSY:
        av_log(NULL, AV_LOG_INFO, "BUSY\n");
        break;
    case BUFFER_FULL:
        av_log(NULL, AV_LOG_INFO, "BUFFER_FULL\n");
        break;
    default:
        av_log(NULL, AV_LOG_INFO, "UNKOWN\n");
        break;
    }
}

void client_disconnect(struct Client *c, int write_trailer)
{
    struct Segment *seg;
    client_set_state(c, BUSY);
    if (write_trailer)
        av_write_trailer(c->ofmt_ctx);
    c->ffinfo->httpd->close(c->ffinfo->server, c->ffinfo->client);
    av_free(c->ofmt_ctx->pb->buffer);
    avio_context_free(&c->ofmt_ctx->pb);
    avformat_free_context(c->ofmt_ctx);
    av_free(c->ffinfo);
    c->ofmt_ctx = NULL;
    c->ffinfo = NULL;
    pthread_mutex_lock(&c->buffer_lock);
    while(av_fifo_size(c->buffer)) {
        av_fifo_generic_read(c->buffer, &seg, sizeof(struct Segment*), NULL);
        segment_unref(seg);
    }
    pthread_mutex_unlock(&c->buffer_lock);
    c->current_segment_id = -1;
    client_set_state(c, FREE);
}

void client_set_state(struct Client *c, enum State state)
{
    pthread_mutex_lock(&c->state_lock);
    c->state = state;
    pthread_mutex_unlock(&c->state_lock);
}

void client_push_segment(struct Client *c, struct Segment *seg)
{
    pthread_mutex_lock(&c->buffer_lock);
    if (av_fifo_space(c->buffer) == 0) {
        av_log(NULL, AV_LOG_WARNING, "Client buffer full, dropping Segment.\n");
        client_set_state(c, BUFFER_FULL);
        pthread_mutex_unlock(&c->buffer_lock);
        return;
    }
    segment_ref(seg);
    av_fifo_generic_write(c->buffer, &seg, sizeof(struct Segment*), NULL);
    pthread_mutex_unlock(&c->buffer_lock);
    client_set_state(c, WRITABLE);
}

void publisher_init(struct PublisherContext **pub)
{
    int i;
    struct PublisherContext *pc = (struct PublisherContext*) av_malloc(sizeof(struct PublisherContext));
    pc->nb_threads = 8;
    pc->current_segment_id = -1;
    pc->shutdown = 0;
    pc->buffer = av_fifo_alloc_array(sizeof(struct Segment), MAX_SEGMENTS);
    pc->fs_buffer = av_fifo_alloc_array(sizeof(struct Segment), MAX_SEGMENTS);
    pthread_mutex_init(&pc->buffer_lock, NULL);
    pthread_mutex_init(&pc->fs_buffer_lock, NULL);
    for (i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &pc->clients[i];
        c->buffer = av_fifo_alloc_array(sizeof(struct Segment), MAX_SEGMENTS);
        c->ofmt_ctx = NULL;
        c->ffinfo = NULL;
        c->id = i;
        c->current_segment_id = -1;
        pthread_mutex_init(&c->state_lock, NULL);
        pthread_mutex_init(&c->buffer_lock, NULL);
        client_set_state(c, FREE);
    }
    *pub = pc;
}

void publisher_push_segment(struct PublisherContext *pub, struct Segment *seg)
{
    struct Segment *drop;
    pthread_mutex_lock(&pub->buffer_lock);
    pthread_mutex_lock(&pub->fs_buffer_lock);
    av_fifo_generic_write(pub->buffer, &seg, sizeof(struct Segment*), NULL);
    segment_ref(seg);
    if (av_fifo_size(pub->fs_buffer) >= BUFFER_SEGMENTS * sizeof(struct Segment*)) {
        av_fifo_generic_read(pub->fs_buffer, &drop, sizeof(struct Segment*), NULL);
        segment_unref(drop);
    }
    av_fifo_generic_write(pub->fs_buffer, &seg, sizeof(struct Segment*), NULL);
    pthread_mutex_unlock(&pub->buffer_lock);
    pthread_mutex_unlock(&pub->fs_buffer_lock);
    segment_ref(seg);
}

int publisher_reserve_client(struct PublisherContext *pub)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        switch(pub->clients[i].state) {
            case FREE:
                client_set_state(&pub->clients[i], RESERVED);
                return 0;
            default:
                continue;
        }
    }
    return 1;
}

void publisher_cancel_reserve(struct PublisherContext *pub)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        switch(pub->clients[i].state) {
            case RESERVED:
                client_set_state(&pub->clients[i], FREE);
                return;
            default:
                continue;
        }
    }
    return;
}

void client_push_prebuffer(struct PublisherContext *pub, struct Client *c)
{
    int off;
    int size;
    struct Segment *seg;
    pthread_mutex_lock(&pub->fs_buffer_lock);
    size = av_fifo_size(pub->fs_buffer);
    for (off = 0; off < size; off += sizeof(struct Segment*)) {
        av_fifo_generic_peek_at(pub->fs_buffer, &seg, off, sizeof(struct Segment*), NULL);
        client_push_segment(c, seg);
    }
    pthread_mutex_unlock(&pub->fs_buffer_lock);
}

void publisher_add_client(struct PublisherContext *pub, AVFormatContext *ofmt_ctx, struct FFServerInfo *ffinfo)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        switch(pub->clients[i].state) {
            case RESERVED:
                pub->clients[i].ofmt_ctx = ofmt_ctx;
                pub->clients[i].ffinfo = ffinfo;
                client_set_state(&pub->clients[i], WRITABLE);
                client_push_prebuffer(pub, &pub->clients[i]);
                return;
            default:
                continue;
        }
    }
}

void publisher_free(struct PublisherContext *pub)
{
    int i;
    struct Segment *seg;
    pthread_mutex_lock(&pub->buffer_lock);
    while(av_fifo_size(pub->buffer)) {
        av_fifo_generic_read(pub->buffer, &seg, sizeof(struct Segment*), NULL);
        segment_unref(seg);
    }
    av_fifo_freep(&pub->buffer);
    pthread_mutex_unlock(&pub->buffer_lock);
    
    pthread_mutex_lock(&pub->fs_buffer_lock);
    while(av_fifo_size(pub->fs_buffer)) {
        av_fifo_generic_read(pub->fs_buffer, &seg, sizeof(struct Segment*), NULL);
        segment_unref(seg);
    }
    av_fifo_freep(&pub->fs_buffer);
    for (i = 0; i < MAX_CLIENTS; i++) {
        av_fifo_freep(&pub->clients[i].buffer);
    }
    pthread_mutex_unlock(&pub->fs_buffer_lock);
    av_free(pub);
    return;
}

void publisher_freep(struct PublisherContext **pub)
{
    publisher_free(*pub);
    *pub = NULL;
    return;
}

void publish(struct PublisherContext *pub)
{
    int i;
    struct Segment *seg;
    char filename[128] = {0};
    pthread_mutex_lock(&pub->buffer_lock);
    av_log(NULL, AV_LOG_DEBUG, "pub->buffer size: %d\n", av_fifo_size(pub->buffer));
    if (av_fifo_size(pub->buffer) == 0) {
        pthread_mutex_unlock(&pub->buffer_lock);
        return;
    }
    av_fifo_generic_read(pub->buffer, &seg, sizeof(struct Segment*), NULL);
    pthread_mutex_unlock(&pub->buffer_lock);
    if (seg) {
        pub->current_segment_id = seg->id;
        snprintf(filename, 127, "segment-%04d.mkv", seg->id);
//        segment_save(seg, filename);
        
        for (i = 0; i < MAX_CLIENTS; i++) {
            switch(pub->clients[i].state) {
                case BUFFER_FULL:
                    av_log(pub, AV_LOG_WARNING, "Dropping segment for client %d, buffer full.\n", i);
                    continue;
                case WAIT:
                case WRITABLE:
                    client_push_segment(&pub->clients[i], seg);
                default:
                    continue;
            }
        }
        segment_unref(seg);
    }
}

void publisher_gen_status_json(struct PublisherContext *pub, char *status)
{
    int states[STATE_NB] = {0};
    int current_read = 0, newest_write = 0, oldest_write = 0;
    int i;
    struct Client *c;

    current_read = pub->current_segment_id;
    oldest_write = current_read;

    for (i = 0; i < MAX_CLIENTS; i++) {
        c = &pub->clients[i];
        if (c->current_segment_id > 0 && c->current_segment_id < oldest_write) {
            oldest_write = c->current_segment_id;
        }
        if (c->current_segment_id > newest_write) {
            newest_write = c->current_segment_id;
        }
        states[c->state]++;
    }
    

    snprintf(status, 4095,
    "{\n\t\"free\": %d,\n"
    "\t\"reserved\": %d,\n"
    "\t\"wait\": %d,\n"
    "\t\"writable\": %d,\n"
    "\t\"busy\": %d,\n"
    "\t\"buffer_full\": %d,\n"
    "\t\"current_read\": %d,\n"
    "\t\"newest_write\": %d,\n"
    "\t\"oldest_write\": %d\n"
    "}\n",
    states[FREE],
    states[RESERVED],
    states[WAIT],
    states[WRITABLE],
    states[BUSY],
    states[BUFFER_FULL],
    current_read,
    newest_write,
    oldest_write);
}
