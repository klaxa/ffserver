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

#include <stdio.h>
#include "segment.h"
#include <pthread.h>

#include <libavutil/opt.h>
#include <libavutil/log.h>


void segment_save(struct Segment *seg, const char *filename)
{
    AVFormatContext *ofmt_ctx = NULL;
    int ret;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate output to save Segment %d.\n", seg->id);
        return;
    }

    if ((ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE)) < 0) {
        av_log(ofmt_ctx, AV_LOG_ERROR,
                "Could not open output io context to save Segment %d: %s.\n", seg->id, av_err2str(ret));
        return;
    }

    avio_write(ofmt_ctx->pb, seg->buf, seg->size);
    avio_flush(ofmt_ctx->pb);
    avio_close(ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
}

void segment_free(struct Segment *seg)
{
    av_log(NULL, AV_LOG_DEBUG, "Freeing segment\n");
    avformat_free_context(seg->fmt_ctx);
    av_free(seg->io_ctx->buffer);
    av_free(seg->io_ctx);
    av_free(seg->buf);
    av_free(seg->ts);
    av_free(seg);
}

void segment_ref(struct Segment *seg)
{
    pthread_mutex_lock(&seg->nb_read_lock);
    seg->nb_read++;
    av_log(NULL, AV_LOG_DEBUG, "%04d  ref Readers: %d\n", seg->id, seg->nb_read);
    pthread_mutex_unlock(&seg->nb_read_lock);
}

void segment_unref(struct Segment *seg)
{
    pthread_mutex_lock(&seg->nb_read_lock);
    seg->nb_read--;
    pthread_mutex_unlock(&seg->nb_read_lock);
    av_log(NULL, AV_LOG_DEBUG, "%04d unref Readers: %d\n", seg->id, seg->nb_read);
    if (seg->nb_read == 0) {
        segment_free(seg);
    }
}

int segment_write(void *opaque, unsigned char *buf, int buf_size)
{
    struct Segment *seg = (struct Segment*) opaque;
    seg->size += buf_size;
    seg->buf = av_realloc(seg->buf, seg->size);
    if (!seg->buf) {
        av_log(NULL, AV_LOG_ERROR, "Could not grow segment.\n");
        return AVERROR(ENOMEM);
    }
    memcpy(seg->buf + seg->size - buf_size, buf, buf_size);
    return buf_size;
}

int segment_read(void *opaque, unsigned char *buf, int buf_size)
{
    struct SegmentReadInfo *info = (struct SegmentReadInfo*) opaque;
    buf_size = buf_size < info->left ? buf_size : info->left;

    /* copy internal buffer data to buf */
    memcpy(buf, info->buf, buf_size);
    info->buf  += buf_size;
    info->left -= buf_size;
    return buf_size ? buf_size : AVERROR_EOF;
}


void segment_close(struct Segment *seg)
{
    av_write_trailer(seg->fmt_ctx);
}

void segment_init(struct Segment **seg_p, AVFormatContext *fmt)
{
    int ret;
    int i;
    AVStream *in_stream, *out_stream;
    struct Segment *seg = av_malloc(sizeof(struct Segment));
    *seg_p = NULL;
    if (!seg) {
        av_log(fmt, AV_LOG_ERROR, "Could not allocate segment.\n");
        return;
    }

    seg->ifmt = (AVInputFormat *) av_find_input_format("matroska");
    seg->fmt_ctx = NULL;
    seg->nb_read = 0;
    seg->size = 0;
    seg->ts = NULL;
    seg->ts_len = 0;
    seg->buf = NULL;
    seg->avio_buffer = av_malloc(AV_BUFSIZE);
    if (!seg->avio_buffer) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate segment avio_buffer.\n");
        av_free(seg);
        return;
    }
    pthread_mutex_init(&seg->nb_read_lock, NULL);
    seg->io_ctx = avio_alloc_context(seg->avio_buffer, AV_BUFSIZE, 1, seg, NULL, &segment_write, NULL);
    if (!seg->io_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate segment io context.\n");
        av_free(seg->avio_buffer);
        av_free(seg);
        return;
    }
    seg->io_ctx->seekable = 0;
    avformat_alloc_output_context2(&seg->fmt_ctx, NULL, "matroska", NULL);
    if (!seg->fmt_ctx) {
        av_log(seg->io_ctx, AV_LOG_ERROR, "Could not allocate segment output context.\n");
        av_free(seg->avio_buffer);
        av_free(seg->io_ctx);
        av_free(seg);
        return;
    }
    if ((ret = av_opt_set_int(seg->fmt_ctx, "flush_packets", 1, AV_OPT_SEARCH_CHILDREN)) < 0) {
        av_log(seg->fmt_ctx, AV_LOG_WARNING, "Could not set flush_packets!\n");
    }

    seg->fmt_ctx->flags |= AVFMT_FLAG_GENPTS | AVFMT_FLAG_AUTO_BSF;

    av_log(seg->fmt_ctx, AV_LOG_DEBUG, "Initializing segment\n");

    for (i = 0; i < fmt->nb_streams; i++) {
        in_stream = fmt->streams[i];
        out_stream = avformat_new_stream(seg->fmt_ctx, NULL);
        if (!out_stream) {
            av_log(seg->fmt_ctx, AV_LOG_WARNING, "Failed allocating output stream\n");
            continue;
        }
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            av_log(seg->fmt_ctx, AV_LOG_WARNING, "Failed to copy context from input to output stream codec context\n");
            continue;
        }
        out_stream->codecpar->codec_tag = 0;
        if (out_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (in_stream->sample_aspect_ratio.num)
                out_stream->sample_aspect_ratio = in_stream->sample_aspect_ratio;
            out_stream->avg_frame_rate = in_stream->avg_frame_rate;
            out_stream->r_frame_rate = in_stream->r_frame_rate;
        }
        av_dict_copy(&out_stream->metadata, in_stream->metadata, 0);
    }
    av_dict_copy(&seg->fmt_ctx->metadata, fmt->metadata, 0);
    seg->fmt_ctx->pb = seg->io_ctx;
    ret = avformat_write_header(seg->fmt_ctx, NULL);
    avio_flush(seg->io_ctx);
    if (ret < 0) {
        segment_close(seg);
        av_log(seg->fmt_ctx, AV_LOG_WARNING, "Error occured while writing header: %s\n", av_err2str(ret));
    }

    *seg_p = seg;
    av_log(seg->fmt_ctx, AV_LOG_DEBUG, "Initialized segment.\n");
    return;
}
