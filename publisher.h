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

#ifndef PUBLISHER_H
#define PUBLISHER_H

#include <libavformat/avformat.h>
#include <libavutil/fifo.h>
#include <pthread.h>
#include "segment.h"
#include "httpd.h"

#define MAX_CLIENTS 16
#define MAX_SEGMENTS 16
#define BUFFER_SEGMENTS 10

/* Client State enum */

enum State {
    FREE,         // no client connected
    RESERVED,     // reserved for a client that just connected
    WAIT,         // up to date, no new Segments to write
    WRITABLE,     // buffer is not full, new Segments can be pushed
    BUSY,         // currently writing to this client
    BUFFER_FULL,  // client buffer is full, new Segments will be dropped
    STATE_NB
};


struct Client {
    AVFormatContext *ofmt_ctx; // writable AVFormatContext, basically our tcp connection to the client
    AVFifoBuffer *buffer; // Client buffer of Segment references
    char *method;
    char *resource;
    struct FFServerInfo *ffinfo;
    enum State state;
    pthread_mutex_t buffer_lock;
    pthread_mutex_t state_lock;
    int id;
    int current_segment_id; // The stream-based id of the segment that has last been worked on.
};

struct PublisherContext {
    struct Client clients[MAX_CLIENTS]; // currently compile-time configuration, easly made dynamic with malloc?
    AVFifoBuffer *buffer; // publisher buffer for new Segments
    AVFifoBuffer *fs_buffer; // fast start buffer
    pthread_mutex_t buffer_lock;
    pthread_mutex_t fs_buffer_lock;
    int nb_threads;
    int current_segment_id;
    int shutdown; // indicate shutdown, gracefully close client connections and files and exit
    char *stream_name;
};

/**
 * Log a client's stats to the console.
 *
 * @param c pointer to the client to print
 */
void client_log(struct Client *c);

/**
 * Disconnect a client.
 *
 * @param c pointer to the client to disconnect.
 */
void client_disconnect(struct Client *c, int write_trailer);

/**
 * Set a client's state. Note: This is protected by mutex locks.
 *
 * @param c pointer to the client to set the state of
 * @param state the state to set the client to
 */
void client_set_state(struct Client *c, enum State state);

/**
 * Allocate and initialize a PublisherContext
 *
 * @param pub pointer to a pointer to a PublisherContext. It will be allocated and initialized.
 * @param stream_name string containing the name of the stream.
 */
void publisher_init(struct PublisherContext **pub, char *stream_name);

/**
 * Push a Segment to a PublisherContext.
 *
 * @param pub pointer to a PublisherContext
 * @param seg pointer to the Segment to add
 */
void publisher_push_segment(struct PublisherContext *pub, struct Segment *seg);

/**
 * Reserve a slot in the client struct of a PublisherContext. May fail if the number
 * of maximum clients has been reached.
 *
 * @param pub pointer to a PublisherContext
 * @return 0 in case of success, 1 in case of failure
 */
int publisher_reserve_client(struct PublisherContext *pub);

/**
 * Cancel a single reservation. This can be used if a client spot was reserved, but the client
 * unexpectedly disconnects or sends an invalid request.
 *
 * @param pub pointer to a PublisherContext
 */
void publisher_cancel_reserve(struct PublisherContext *pub);

/**
 * Add a client by its ofmt_ctx. This initializes an element in the client struct of the PublisherContext
 * that has been reserved prior to calling this function.
 *
 * @param pub pointer to a PublisherContext
 * @param ofmt_ctx AVFormatContext of a client
 * @param ffinfo pointer to struct containing custom IO information for server independent write implementation
 */
void publisher_add_client(struct PublisherContext *pub, AVFormatContext *ofmt_ctx, struct FFServerInfo *ffinfo);

/**
 * Free buffers and associated client buffers.
 *
 * @param pub pointer to the PublisherContext to free
 */
void publisher_free(struct PublisherContext *pub);

/**
 * Free buffers and associated client buffers and set *pub to NULL.
 *
 * @param pub pointer to the PublisherContext pointer to free
 */
void publisher_freep(struct PublisherContext **pub);

/**
 * Signal to the PublisherContext to check its buffer and publish pending Segments.
 *
 * @param pub pointer to a PublisherContext
 */
void publish(struct PublisherContext *pub);

/**
 * Print the current client and file reading status to a json string.
 * @param pub pointer to a PublisherContext
 * @param status string of at least 4096 bytes size.
 */
void publisher_gen_status_json(struct PublisherContext *pub, char *status);

#endif // PUBLISHER_H
