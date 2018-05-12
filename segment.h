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

#ifndef SEGMENT_H
#define SEGMENT_H

#include <libavformat/avformat.h>

#define AV_BUFSIZE 4096

struct SegmentReadInfo {
    unsigned char *buf;
    int left;
};

struct Segment {
    unsigned char *buf;
    AVIOContext *io_ctx;
    AVFormatContext *fmt_ctx;
    AVInputFormat *ifmt;
    size_t size;
    int64_t *ts;
    int ts_len;
    int nb_read;
    unsigned char *avio_buffer;
    int id;
    pthread_mutex_t nb_read_lock;
};

/**
 * Save segment to a file using filename.
 * Note: Currently produces incorrect files.
 *
 * @param seg pointer to the Segment to save
 * @param filename string to use to save the Segment
 */
void segment_save(struct Segment *seg, const char *filename);

/**
 * Free Segment. Automatically called when a Segment's refcount reaches zero.
 *
 * @param seg pointer to the Segment to free
 */
void segment_free(struct Segment *seg);

/**
 * Increase the reference counter for a Segment.
 *
 * @param seg pointer to the Segment to increase the refcounter for
 */
void segment_ref(struct Segment *seg);

/**
 * Decrease the reference counter for a Segment. Calls segment_free() if the refcounter reaches zero.
 *
 * @param seg pointer to the Segment to unref
 */
void segment_unref(struct Segment *seg);


/**
 * Write buf_size bytes from buf to a Segment opaque.
 *
 * @param opaque void pointer to the Segment to write to
 * @param buf pointer to the data to write
 * @param buf_size number of bytes to write
 * @return number of bytes written. May be less than buf_size.
 */
int segment_write(void *opaque, unsigned char *buf, int buf_size);

/**
 * Read buf_size bytes from a Segment using a SegmentReadInfo struct and store them in buf.
 * Using a SegmentReadInfo struct instead of the Segment directly is needed, because there
 * are multiple readers for a single Segment and each has to keep its own reading state.
 *
 * @param opaque void pointer to the SegmentReadInfo struct to use for reading
 * @param buf pointer to where to store the read data
 * @param buf_size number of bytes to read
 * @return number of bytes read. May be less than buf_size.
 */
int segment_read(void *opaque, unsigned char *buf, int buf_size);

/**
 * Write a Segment's trailer
 *
 * @param seg pointer to the Segment to finalize
 */
void segment_close(struct Segment *seg);

/**
 * Allocate and initialize a new segment given the AVFormatContext fmt
 *
 * @param seg pointer to a pointer to a Segment to be allocated and initialized
 * @param fmt pointer to an AVFormatContext describing the format of the Segment
 */
void segment_init(struct Segment **seg, AVFormatContext *fmt);


#endif // SEGMENT_H
