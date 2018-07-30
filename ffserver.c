/*
 * Copyright (c) 2018 Stephan Holljes
 *
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

/**
 * @file
 * multimedia server based on the FFmpeg libraries
 */

#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#include <libavutil/log.h>
#include <libavutil/timestamp.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/file.h>

#include "segment.h"
#include "publisher.h"
#include "fileserver.h"
#include "httpd.h"
#include "configreader.h"

#define BUFFER_SECS 10
#define LISTEN_TIMEOUT_MSEC 1000
#define AUDIO_ONLY_SEGMENT_SECONDS 2

struct ReadInfo {
    struct PublisherContext *pub;
    struct StreamConfig *config;
    AVFormatContext *ifmt_ctx;
    struct FileserverContext *fs;
    char *input_uri;
    char *server_name;
};

struct WriteInfo {
    struct PublisherContext *pub;
    int thread_id;
};

struct AcceptInfo {
    struct PublisherContext **pubs;
    struct HTTPDInterface *httpd;
    AVFormatContext **ifmt_ctxs;
    struct HTTPDConfig *config;
    struct FileserverContext *fs;
    int nb_pub; /** number of publishers (streams) equal to number of ifmt_ctx */
};


int ffserver_write(void *opaque, unsigned char *buf, int buf_size)
{
    struct FFServerInfo *info = (struct FFServerInfo*) opaque;
    return info->httpd->write(info->server, info->client, buf, buf_size);
}


void *read_thread(void *arg)
{
    struct ReadInfo *info = (struct ReadInfo*) arg;
    AVFormatContext *ifmt_ctx = info->ifmt_ctx;
    int ret, i;
    int video_idx = -1;
    int audio_only = 0;
    int id = 0;
    int64_t pts, pts_tmp, dts_tmp, now, start, last_cut = 0;
    int64_t *ts;
    char playlist_dirname[1024];
    char playlist_filename[1024];
    AVFormatContext *ofmt_ctx[FMT_NB] = { 0 }; // some may be left unused
    struct Segment *seg = NULL;
    AVPacket pkt;
    AVStream *in_stream, *out_stream;
    AVRational tb = {1, AV_TIME_BASE};
    AVStream *stream;
    int stream_formats[FMT_NB] = { 0 };

    for (i = 0; i < info->config->nb_formats; i++)
        stream_formats[info->config->formats[i]] = 1;

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(ifmt_ctx, AV_LOG_ERROR, "Could not get input stream info.\n");
        goto end;
    }

    av_log(ifmt_ctx, AV_LOG_INFO, "Finding video stream.\n");
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        av_log(ifmt_ctx, AV_LOG_DEBUG, "Checking stream %d\n", i);
        stream = ifmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = i;
            break;
        }
    }
    if (video_idx == -1)
        audio_only = 1;

    if (stream_formats[FMT_HLS] || stream_formats[FMT_DASH]) {
        snprintf(playlist_dirname, 1024, "%s/%s", info->server_name, info->config->stream_name);
        ret = mkdir(playlist_dirname, 0755);
        if (ret < 0 && errno != EEXIST) {
            av_log(NULL, AV_LOG_WARNING, "Could not create stream directory (%s) dropping hls/dash\n", strerror(errno));
            stream_formats[FMT_HLS] = 0;
            stream_formats[FMT_DASH] = 0;
        }
    }

    if (stream_formats[FMT_HLS]) {
        snprintf(playlist_filename, 1024, "%s/%s/%s_hls.m3u8", info->server_name, info->config->stream_name,
                                                                                  info->config->stream_name);
        avformat_alloc_output_context2(&ofmt_ctx[FMT_HLS], NULL, "hls", playlist_filename);

        if (!ofmt_ctx[FMT_HLS]) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate hls output context.\n");
            goto end;
        }

        for (i = 0; i < ifmt_ctx->nb_streams; i++) {
            in_stream = ifmt_ctx->streams[i];
            out_stream = avformat_new_stream(ofmt_ctx[FMT_HLS], NULL);
            if (!out_stream) {
                av_log(ofmt_ctx[FMT_HLS], AV_LOG_WARNING, "Failed allocating output stream\n");
                continue;
            }
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                av_log(ofmt_ctx[FMT_HLS], AV_LOG_WARNING, "Failed to copy context from input to output stream codec context\n");
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
        av_dict_copy(&ofmt_ctx[FMT_HLS]->metadata, ifmt_ctx->metadata, 0);
        ret = avformat_write_header(ofmt_ctx[FMT_HLS], NULL);
        if (ret < 0) {
            av_log(ofmt_ctx[FMT_HLS], AV_LOG_WARNING, "Error occured while writing header: %s\n", av_err2str(ret));
        }

        av_log(ofmt_ctx[FMT_HLS], AV_LOG_DEBUG, "Initialized hls.\n");
    }

    if (stream_formats[FMT_DASH]) {
        snprintf(playlist_filename, 1024, "%s/%s/%s_dash.mpd", info->server_name, info->config->stream_name,
                                                                                  info->config->stream_name);
        avformat_alloc_output_context2(&ofmt_ctx[FMT_DASH], NULL, "dash", playlist_filename);

        if (!ofmt_ctx[FMT_DASH]) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate hls output context.\n");
            goto end;
        }

        for (i = 0; i < ifmt_ctx->nb_streams; i++) {
            in_stream = ifmt_ctx->streams[i];
            out_stream = avformat_new_stream(ofmt_ctx[FMT_DASH], NULL);
            if (!out_stream) {
                av_log(ofmt_ctx[FMT_DASH], AV_LOG_WARNING, "Failed allocating output stream\n");
                continue;
            }
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                av_log(ofmt_ctx[FMT_DASH], AV_LOG_WARNING, "Failed to copy context from input to output stream codec context\n");
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
        av_dict_copy(&ofmt_ctx[FMT_DASH]->metadata, ifmt_ctx->metadata, 0);
        ret = avformat_write_header(ofmt_ctx[FMT_DASH], NULL);
        if (ret < 0) {
            av_log(ofmt_ctx[FMT_DASH], AV_LOG_WARNING, "Error occured while writing header: %s\n", av_err2str(ret));
        }

        av_log(ofmt_ctx[FMT_DASH], AV_LOG_DEBUG, "Initialized dash.\n");
    }

    // All information needed to start segmenting the file is gathered now.
    // start BUFFER_SECS seconds "in the past" to "catch up" to real-time. Has no effect on streamed sources.
    start = av_gettime_relative() - BUFFER_SECS * AV_TIME_BASE;

    // segmenting main-loop

    for (;;) {
        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0)
            break;

        in_stream = ifmt_ctx->streams[pkt.stream_index];
        if (pkt.pts == AV_NOPTS_VALUE) {
            pkt.pts = 0;
        }
        if (pkt.dts == AV_NOPTS_VALUE) {
            pkt.dts = 0;
        }

        // current pts in AV_TIME_BASE
        pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, tb, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);

        // current stream "uptime"
        now = av_gettime_relative() - start;

        // simulate real-time reading
        while (pts > now) {
            usleep(1000);
            now = av_gettime_relative() - start;
        }

        if (stream_formats[FMT_MATROSKA]) {
            // keyframe or first Segment or audio_only and more than AUDIO_ONLY_SEGMENT_SECONDS passed since last cut
            if ((pkt.flags & AV_PKT_FLAG_KEY && pkt.stream_index == video_idx) || !seg ||
                (audio_only && pts - last_cut >= AUDIO_ONLY_SEGMENT_SECONDS * AV_TIME_BASE)) {
                if (seg) {
                    segment_close(seg);
                    publisher_push_segment(info->pub, seg);
                    av_log(NULL, AV_LOG_DEBUG, "New segment pushed.\n");
                    publish(info->pub);
                    av_log(NULL, AV_LOG_DEBUG, "Published new segment.\n");
                }
                last_cut = pts;
                segment_init(&seg, ifmt_ctx);
                if (!seg) {
                    av_log(NULL, AV_LOG_ERROR, "Segment initialization failed, shutting down.\n");
                    goto end;
                }
                seg->id = id++;
                av_log(NULL, AV_LOG_DEBUG, "Starting new segment, id: %d\n", seg->id);
            }
            dts_tmp = av_rescale_q_rnd(pkt.dts, in_stream->time_base, tb, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            ts = av_dynarray2_add((void **)&seg->ts, &seg->ts_len, sizeof(int64_t),
                                (const void *)&dts_tmp);
            if (!ts) {
                av_log(seg->fmt_ctx, AV_LOG_ERROR, "could not write dts\n.");
                goto end;
            }

            ts = av_dynarray2_add((void **)&seg->ts, &seg->ts_len, sizeof(int64_t),
                                (const void *)&pts);
            if (!ts) {
                av_log(seg->fmt_ctx, AV_LOG_ERROR, "could not write pts\n.");
                goto end;
            }
            pts_tmp = pkt.pts;
            dts_tmp = pkt.dts;
            pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, seg->fmt_ctx->streams[pkt.stream_index]->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, seg->fmt_ctx->streams[pkt.stream_index]->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            ret = av_write_frame(seg->fmt_ctx, &pkt);
            pkt.pts = pts_tmp;
            pkt.dts = dts_tmp;
            if (ret < 0) {
                av_log(seg->fmt_ctx, AV_LOG_ERROR, "av_write_frame() failed.\n");
                goto end;
            }
        }

        if (stream_formats[FMT_DASH]) {
            pts_tmp = pkt.pts;
            dts_tmp = pkt.dts;
            pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, ofmt_ctx[FMT_DASH]->streams[pkt.stream_index]->time_base,
                                                               AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, ofmt_ctx[FMT_DASH]->streams[pkt.stream_index]->time_base,
                                                               AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            pkt.duration = av_rescale_q_rnd(pkt.duration, in_stream->time_base, ofmt_ctx[FMT_DASH]->streams[pkt.stream_index]->time_base,
                                                               AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);

            ret = av_write_frame(ofmt_ctx[FMT_DASH], &pkt);
            pkt.pts = pts_tmp;
            pkt.dts = dts_tmp;
            if (ret < 0) {
                fprintf(stderr, "Error muxing packet\n");
                break;
            }
        }

        if (stream_formats[FMT_HLS]) {
            pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, ofmt_ctx[FMT_HLS]->streams[pkt.stream_index]->time_base,
                                                               AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, ofmt_ctx[FMT_HLS]->streams[pkt.stream_index]->time_base,
                                                               AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            pkt.duration = av_rescale_q_rnd(pkt.duration, in_stream->time_base, ofmt_ctx[FMT_HLS]->streams[pkt.stream_index]->time_base,
                                                               AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);

            ret = av_write_frame(ofmt_ctx[FMT_HLS], &pkt);
            if (ret < 0) {
                fprintf(stderr, "Error muxing packet\n");
                break;
            }
        }
        av_packet_unref(&pkt);
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        av_log(seg->fmt_ctx, AV_LOG_ERROR, "Error occurred during read: %s\n", av_err2str(ret));
        goto end;
    }
    if (stream_formats[FMT_MATROSKA]) {
        segment_close(seg);
        publisher_push_segment(info->pub, seg);
        publish(info->pub);
    }


end:
    avformat_close_input(&ifmt_ctx);
    if (info->pub)
        info->pub->shutdown = 1;
    for (i = 0; i < FMT_NB; i++) {
        if (ofmt_ctx[i]) {
            av_write_trailer(ofmt_ctx[i]);
            avformat_free_context(ofmt_ctx[i]);
        }
    }
    if (info->fs) {
        sleep(BUFFER_SECS);
        info->fs->shutdown = 1;
    }
    return NULL;
}

void write_segment(struct Client *c)
{
    struct Segment *seg;
    int ret;
    int pkt_count = 0;
    AVRational tb = {1, AV_TIME_BASE};
    pthread_mutex_lock(&c->buffer_lock);
    if (av_fifo_size(c->buffer) > 0) {
        AVFormatContext *fmt_ctx;
        AVIOContext *avio_ctx;
        AVPacket pkt;
        struct SegmentReadInfo info;
        unsigned char *avio_buffer;

        av_fifo_generic_peek(c->buffer, &seg, sizeof(seg), NULL);
        pthread_mutex_unlock(&c->buffer_lock);
        c->current_segment_id = seg->id;
        info.buf = seg->buf;
        info.left = seg->size;

        if (!(fmt_ctx = avformat_alloc_context())) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate format context\n");
            client_disconnect(c, 0);
            return;
        }

        avio_buffer = av_malloc(AV_BUFSIZE);
        if (!avio_buffer) {
            av_log(fmt_ctx, AV_LOG_ERROR, "Could not allocate avio_buffer\n");
            avformat_free_context(fmt_ctx);
            client_disconnect(c, 0);
            return;
        }
        avio_ctx = avio_alloc_context(avio_buffer, AV_BUFSIZE, 0, &info, &segment_read, NULL, NULL);
        if (!avio_ctx) {
            av_log(fmt_ctx, AV_LOG_ERROR, "Could not allocate avio_ctx\n");
            avformat_free_context(fmt_ctx);
            av_free(avio_buffer);
            client_disconnect(c, 0);
            return;
        }
        fmt_ctx->pb = avio_ctx;
        ret = avformat_open_input(&fmt_ctx, NULL, seg->ifmt, NULL);
        if (ret < 0) {
            av_log(avio_ctx, AV_LOG_ERROR, "Could not open input\n");
            avformat_close_input(&fmt_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            client_disconnect(c, 0);
            return;
        }

        ret = avformat_find_stream_info(fmt_ctx, NULL);
        if (ret < 0) {
            av_log(fmt_ctx, AV_LOG_ERROR, "Could not find stream information\n");
            avformat_close_input(&fmt_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            client_disconnect(c, 0);
            return;
        }

        av_log(fmt_ctx, AV_LOG_DEBUG, "Client: %d, Segment: %d\n", c->id, seg->id);

        for (;;) {
            ret = av_read_frame(fmt_ctx, &pkt);
            if (ret < 0)
                break;

            pkt.dts = av_rescale_q_rnd(seg->ts[pkt_count], tb,
                                                        c->ofmt_ctx->streams[pkt.stream_index]->time_base,
                                                               AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            pkt.pts = av_rescale_q_rnd(seg->ts[pkt_count+1], tb,
                                                        c->ofmt_ctx->streams[pkt.stream_index]->time_base,
                                                               AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);

            pkt.pos = -1;
            pkt_count += 2;
            ret = av_interleaved_write_frame(c->ofmt_ctx, &pkt);
            //av_packet_unref(&pkt);
            if (ret < 0) {
                av_log(fmt_ctx, AV_LOG_ERROR, "write_frame failed, disconnecting client: %d\n", c->id);
                avformat_close_input(&fmt_ctx);
                av_free(avio_ctx->buffer);
                avio_context_free(&avio_ctx);
                client_disconnect(c, 0);
                return;
            }
        }
        av_free(avio_ctx->buffer);
        avformat_close_input(&fmt_ctx);
        avio_context_free(&avio_ctx);
        pthread_mutex_lock(&c->buffer_lock);
        av_fifo_drain(c->buffer, sizeof(seg));
        pthread_mutex_unlock(&c->buffer_lock);
        segment_unref(seg);
        client_set_state(c, WRITABLE);
    } else {
        pthread_mutex_unlock(&c->buffer_lock);
        client_set_state(c, WAIT);
    }
}

void *accept_thread(void *arg)
{
    struct AcceptInfo *info = (struct AcceptInfo*) arg;
    struct FFServerInfo *ffinfo = NULL;
    struct PublisherContext *pub;
    char status[4096], requested_file[1024], sanitized_file[1024];
    char *stream_name, *resource;
    struct HTTPClient *client = NULL;
    void *server = NULL;
    AVIOContext *client_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVFormatContext *ifmt_ctx;
    unsigned char *avio_buffer;
    AVOutputFormat *ofmt;
    AVDictionary *mkvopts = NULL;
    AVStream *in_stream, *out_stream;
    int ret, i, reply_code;
    int shutdown;
    struct HTTPDConfig *config = info->config;

    info->httpd->init(&server, *config);

    for (;;) {
        shutdown = 1;
        for (i = 0; i < config->nb_streams; i++) {
            if (info->pubs[i] && !info->pubs[i]->shutdown)
                shutdown = 0;
        }
        if (info->fs && !info->fs->shutdown)
            shutdown = 0;
        if (shutdown)
            break;
        for (i = 0; i < config->nb_streams; i++) {
            if (info->pubs[i]) {
                publisher_gen_status_json(info->pubs[i], status);
                av_log(NULL, AV_LOG_INFO, status);
            }
        }
        client = NULL;
        av_log(NULL, AV_LOG_DEBUG, "Accepting new clients.\n");
        reply_code = 200;

        if ((ret = info->httpd->accept(server, &client, NULL)) < 0) {
            if (ret == HTTPD_LISTEN_TIMEOUT) {
                continue;
            } else if (ret == HTTPD_CLIENT_ERROR) {
                info->httpd->close(server, client);
            }
            av_log(NULL, AV_LOG_WARNING, "Error during accept, retrying.\n");
            continue;
        }

        pub = NULL;
        ifmt_ctx = NULL;
        resource = client->resource;
        snprintf(requested_file, 1024, "%s", resource);
        for (i = 0; i < config->nb_streams; i++) {
            stream_name = info->config->streams[i].stream_name;
            //  skip leading '/'  ---v
            if (resource && strlen(resource) > strlen(stream_name)
                && !strncmp(resource + 1, stream_name, strlen(stream_name))) {
                resource++;
                while (resource && *resource++ != '/');
                if (strlen(resource) > 2 && !strncmp(resource, "mkv", 3)) {
                    pub = info->pubs[i];
                    ifmt_ctx = info->ifmt_ctxs[i];
                    memset(requested_file, 0, 1024);
                    break;
                } else if (strlen(resource) > 2 && !strncmp(resource, "hls", 3)) {
                    snprintf(requested_file, 1024, "/%s/%s_hls.m3u8", stream_name, stream_name);
                    break;
                } else if (strlen(resource) > 3 && !strncmp(resource, "dash", 4)) {
                    snprintf(requested_file, 1024, "/%s/%s_dash.mpd", stream_name, stream_name);
                    break;
                }
            }
        }


        if ((!pub || !ifmt_ctx) && !requested_file[0]) {
            av_log(client_ctx, AV_LOG_WARNING, "No suitable publisher found for resource: %s.\n",
                                                        client->resource ? client->resource : "(null)");
            reply_code = 404;
        }


        if (pub && ifmt_ctx && publisher_reserve_client(pub)) {
            av_log(client_ctx, AV_LOG_WARNING, "No more client slots free, Returning 503.\n");
            reply_code = 503;
        }

        if (reply_code != 200) {
            if (pub && ifmt_ctx)
                publisher_cancel_reserve(pub);
            info->httpd->close(server, client);
            continue;
        }

        ffinfo = av_malloc(sizeof(*ffinfo));
        if (!ffinfo) {
            av_log(client_ctx, AV_LOG_ERROR, "Could not allocate FFServerInfo struct.\n");
            publisher_cancel_reserve(pub);
            info->httpd->close(server, client);
            continue;
        }
        ffinfo->httpd = info->httpd;
        ffinfo->client = client;
        ffinfo->server = server;


        // try to serve file
        if (info->fs && requested_file[0]) {
            snprintf(sanitized_file, 1024, "%s", requested_file);
            resource = requested_file;
            while(resource && *resource == '/') {
                resource++;
            }

            snprintf(sanitized_file, 1024, "%s", resource);
            fileserver_schedule(info->fs, ffinfo, sanitized_file);
            continue;
        }

        if (!info->fs && requested_file[0]) {
            info->httpd->close(server, client);
            continue;
        }

        avio_buffer = av_malloc(AV_BUFSIZE);
        if (!avio_buffer) {
            av_log(client_ctx, AV_LOG_ERROR, "Could not allocate output format context.\n");
            publisher_cancel_reserve(pub);
            info->httpd->close(server, client);
            continue;
        }


        client_ctx = avio_alloc_context(avio_buffer, AV_BUFSIZE, 1, ffinfo, NULL, &ffserver_write, NULL);
        if (!client_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate output format context.\n");
            publisher_cancel_reserve(pub);
            info->httpd->close(server, client);
            av_free(client_ctx->buffer);
            avio_context_free(&client_ctx);
            av_free(ffinfo);
            continue;
        }
        avformat_alloc_output_context2(&ofmt_ctx, NULL, "matroska", NULL);
        if (!ofmt_ctx) {
            av_log(client_ctx, AV_LOG_ERROR, "Could not allocate output format context.\n");
            publisher_cancel_reserve(pub);
            info->httpd->close(server, client);
            avformat_free_context(ofmt_ctx);
            av_free(client_ctx->buffer);
            avio_context_free(&client_ctx);
            av_free(ffinfo);
            continue;
        }
        if ((ret = av_dict_set(&mkvopts, "live", "1", 0)) < 0) {
            av_log(client, AV_LOG_ERROR, "Failed to set live mode for matroska: %s\n", av_err2str(ret));
            publisher_cancel_reserve(pub);
            info->httpd->close(server, client);
            avformat_free_context(ofmt_ctx);
            av_free(client_ctx->buffer);
            avio_context_free(&client_ctx);
            av_free(ffinfo);
            continue;
        }
        ofmt_ctx->flags |= AVFMT_FLAG_GENPTS;
        ofmt = ofmt_ctx->oformat;
        ofmt->flags |= AVFMT_NOFILE | AVFMT_FLAG_AUTO_BSF;

        for (i = 0; i < ifmt_ctx->nb_streams; i++) {
            in_stream = ifmt_ctx->streams[i];
            out_stream = avformat_new_stream(ofmt_ctx, NULL);

            if (!out_stream) {
                av_log(client, AV_LOG_ERROR, "Could not allocate output stream.\n");
                publisher_cancel_reserve(pub);
                info->httpd->close(server, client);
                avformat_free_context(ofmt_ctx);
                av_free(client_ctx->buffer);
                avio_context_free(&client_ctx);
                av_free(ffinfo);
                continue;
            }

            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                av_log(client, AV_LOG_ERROR, "Failed to copy context from input to output stream codec context: %s.\n", av_err2str(ret));
                publisher_cancel_reserve(pub);
                info->httpd->close(server, client);
                avformat_free_context(ofmt_ctx);
                av_free(client_ctx->buffer);
                avio_context_free(&client_ctx);
                av_free(ffinfo);
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
        av_dict_copy(&ifmt_ctx->metadata, ofmt_ctx->metadata, 0);
        ofmt_ctx->pb = client_ctx;
        ret = avformat_write_header(ofmt_ctx, &mkvopts);
        if (ret < 0) {
            av_log(client_ctx, AV_LOG_ERROR, "Could not write header to client: %s.\n", av_err2str(ret));
            publisher_cancel_reserve(pub);
            info->httpd->close(server, client);
            avformat_free_context(ofmt_ctx);
            av_free(client_ctx->buffer);
            avio_context_free(&client_ctx);
            av_free(ffinfo);
            continue;
        }
        publisher_add_client(pub, ofmt_ctx, ffinfo);
        ofmt_ctx = NULL;

    }
    av_log(NULL, AV_LOG_INFO, "Shutting down http server.\n");
    info->httpd->shutdown(server);
    av_log(NULL, AV_LOG_INFO, "Shut down http server.\n");
    return NULL;
}

void *write_thread(void *arg)
{
    struct WriteInfo *info = (struct WriteInfo*) arg;
    int i, nb_free;
    struct Client *c;
    for(;;) {
        nb_free = 0;
        usleep(500000);
        av_log(NULL, AV_LOG_DEBUG, "Checking clients, thread: %d\n", info->thread_id);
        for (i = 0; i < MAX_CLIENTS; i++) {
            c = &info->pub->clients[i];
            switch(c->state) {
                case WRITABLE:
                    client_set_state(c, BUSY);
                    write_segment(c);
                    if (info->pub->shutdown && info->pub->current_segment_id == c->current_segment_id) {
                        client_disconnect(c, 1);
                    }
                    continue;
                case FREE:
                    nb_free++;
                default:
                    continue;
            }
        }
        if (info->pub->shutdown && nb_free == MAX_CLIENTS)
            break;
    }

    return NULL;
}

void *fileserver_thread(void *arg)
{
    struct FileserverContext *fs = (struct FileserverContext*) arg;
    int i, clients_served;
    struct FileserverClient *c;
    for (;;) {
        usleep(500000);
        clients_served = 0;
        for (i = 0; i < MAX_CLIENTS; i++) {
            c = &fs->clients[i];
            pthread_mutex_lock(&c->client_lock);
            if (c->buf) {
                c->ffinfo->httpd->write(c->ffinfo->server, c->ffinfo->client, c->buf, c->size);
                av_file_unmap(c->buf, c->size);
                c->ffinfo->httpd->close(c->ffinfo->server, c->ffinfo->client);
                c->buf = NULL;
                c->size = 0;
                av_freep(&c->ffinfo);
                clients_served++;
            }
            pthread_mutex_unlock(&c->client_lock);
        }
        av_log(NULL, AV_LOG_INFO, "Checked clients, fileserver-thread %s %d/%d served\n", fs->server_name, clients_served, MAX_CLIENTS);
        if (fs->shutdown) {
            printf("shutting down\n");
            break;
        }
    }

    return NULL;
}

void *run_server(void *arg) {
    struct AcceptInfo ainfo;
    struct ReadInfo *rinfos;
    struct WriteInfo **winfos_p;
    struct HTTPDConfig *config = (struct HTTPDConfig*) arg;
    struct PublisherContext **pubs;
    struct FileserverContext *fs = NULL;
    AVFormatContext **ifmt_ctxs;
    int ret, i, stream_index;
    int stream_formats[FMT_NB] = { 0 };
    pthread_t *r_threads;
    pthread_t **w_threads_p;
    pthread_t fs_thread = 0;

    pubs = av_mallocz_array(config->nb_streams, sizeof(struct PublisherContext*));
    if (!pubs) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate publishers\n");
        goto error_cleanup;
    }
    ifmt_ctxs = av_mallocz_array(config->nb_streams, sizeof(AVFormatContext*));
    if (!ifmt_ctxs) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate input format contexts.\n");
        goto error_cleanup;
    }

    for (stream_index = 0; stream_index < config->nb_streams; stream_index++) {
        for (i = 0; i < config->streams[stream_index].nb_formats; i++)
            stream_formats[config->streams[stream_index].formats[i]] = 1;
    }

    if (stream_formats[FMT_HLS] || stream_formats[FMT_DASH]) {
        fileserver_init(&fs, config->server_name);
        ret = mkdir(config->server_name, 0755);
        if (ret < 0 && errno != EEXIST) {
            av_log(NULL, AV_LOG_WARNING, "Could not create server directory (%d) dropping hls/dash\n", errno);
            stream_formats[FMT_HLS] = 0;
            stream_formats[FMT_DASH] = 0;
            fileserver_free(fs);
            fs = NULL;
        }
    }

    av_log_set_level(AV_LOG_INFO);

    ainfo.pubs = pubs;
    ainfo.fs = fs;
    ainfo.ifmt_ctxs = ifmt_ctxs;
    ainfo.nb_pub = config->nb_streams;
    ainfo.httpd = &lmhttpd;
    ainfo.config = config;

    rinfos = av_mallocz_array(config->nb_streams, sizeof(struct ReadInfo));
    if (!rinfos) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate read infos.\n");
        goto error_cleanup;
    }
    winfos_p = av_mallocz_array(config->nb_streams, sizeof(struct WriteInfo*));
    if (!winfos_p) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate write info pointers.\n");
        goto error_cleanup;
    }
    r_threads = av_mallocz_array(config->nb_streams, sizeof(pthread_t));
    if (!r_threads) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate read thread handles.\n");
        goto error_cleanup;
    }
    w_threads_p = av_mallocz_array(config->nb_streams, sizeof(pthread_t*));
    if (!w_threads_p) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate write thread handle pointers.\n");
        goto error_cleanup;
    }

    for (stream_index = 0; stream_index < config->nb_streams; stream_index++) {
        struct PublisherContext *pub = NULL;
        struct AVFormatContext *ifmt_ctx = NULL;
        struct ReadInfo rinfo;
        struct WriteInfo *winfos = NULL;
        pthread_t *w_threads = NULL;
        pthread_t r_thread;
        int stream_formats[FMT_NB] = { 0 };
        rinfo.input_uri = config->streams[stream_index].input_uri;
        rinfo.server_name = config->server_name;
        rinfo.config = &config->streams[stream_index];
        rinfo.fs = fs;

        for (i = 0; i < config->streams[stream_index].nb_formats; i++)
            stream_formats[config->streams[stream_index].formats[i]] = 1;

        if ((ret = avformat_open_input(&ifmt_ctx, rinfo.input_uri, NULL, NULL))) {
            av_log(NULL, AV_LOG_ERROR, "run_server: Could not open input\n");
            continue;
        }


        ifmt_ctxs[stream_index] = ifmt_ctx;
        if (stream_formats[FMT_MATROSKA])
            publisher_init(&pub, config->streams[stream_index].stream_name);

        pubs[stream_index] = pub;

        rinfo.ifmt_ctx = ifmt_ctx;
        rinfo.pub = pub;

        rinfos[stream_index] = rinfo;

        ret = pthread_create(&r_thread, NULL, read_thread, &rinfos[stream_index]);
        if (ret != 0) {
            pub->shutdown = 1;
            r_thread = 0;
            goto end;
        }
        r_threads[stream_index] = r_thread;

        if (stream_formats[FMT_MATROSKA]) {
            w_threads = av_mallocz_array(pub->nb_threads, sizeof(pthread_t));
            if (!w_threads) {
                av_log(NULL, AV_LOG_ERROR, "Could not allocate write thread handles.\n");
                continue;
            }
            winfos = av_mallocz_array(pub->nb_threads, sizeof(struct WriteInfo));
            if (!winfos) {
                av_log(NULL, AV_LOG_ERROR, "Could not allocate write infos.\n");
                continue;
            }
            w_threads_p[stream_index] = w_threads;
            winfos_p[stream_index] = winfos;

            for (i = 0; i < pub->nb_threads; i++) {
                winfos[i].pub = pub;
                winfos[i].thread_id = i;
                ret = pthread_create(&w_threads[i], NULL, write_thread, &winfos_p[stream_index][i]);
                if (ret != 0) {
                    pub->shutdown = 1;
                    w_threads[i] = 0;
                    goto end;
                }
            }
            w_threads_p[stream_index] = w_threads;
        }

    }
    if (stream_formats[FMT_HLS] || stream_formats[FMT_DASH]) {
        ret = pthread_create(&fs_thread, NULL, fileserver_thread, fs);
        if (ret != 0) {
            fs->shutdown = 1;
            fs_thread = 0;
            goto end;
        }
    }



    //pthread_create(&a_thread, NULL, accept_thread, &ainfo);
    accept_thread(&ainfo);

end:
    for (stream_index = 0; stream_index < config->nb_streams; stream_index++) {
        // in case of thread creation failure this might NULL
        if (r_threads[stream_index])
            pthread_join(r_threads[stream_index], NULL);
        if (pubs[stream_index]) {
            for (i = 0; i < pubs[stream_index]->nb_threads; i++) {
                // might also be NULL because of thread creation failure
                if (w_threads_p[stream_index][i])
                    pthread_join(w_threads_p[stream_index][i], NULL);
            }
        }
        av_free(winfos_p[stream_index]);
        av_free(w_threads_p[stream_index]);
        // pubs[stream_index] could be null if the file could not be opened or mkv was not requested
        if (pubs[stream_index])
            publisher_free(pubs[stream_index]);
    }

    if (fs_thread) {
        fs->shutdown = 1;
        pthread_join(fs_thread, NULL);
        fileserver_free(fs);
    }
error_cleanup:
    av_free(rinfos);
    av_free(winfos_p);
    av_free(r_threads);
    av_free(w_threads_p);
    av_free(pubs);
    av_free(ifmt_ctxs);
    return NULL;
}

int main(int argc, char *argv[])
{
    struct HTTPDConfig *configs;
    int nb_configs;
    pthread_t *server_threads;
    int i, ret;

    if (argc < 2) {
        printf("Usage: %s config.lua\n", argv[0]);
        return 1;
    }

    nb_configs = configs_read(&configs, argv[1]);
    if (nb_configs <= 0) {
        printf("No valid configurations parsed.\n");
        return 1;
    }
    server_threads = av_mallocz_array(nb_configs, sizeof(pthread_t));
    if (!server_threads) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate server thread handles.\n");
        return AVERROR(ENOMEM);
    }
    for (i = 0; i < nb_configs; i++) {
        config_dump(configs + i, stderr);
        ret = pthread_create(&server_threads[i], NULL, run_server, configs + i);
        if (ret != 0) {
            server_threads[i] = 0;
        }
    }

    for (i = 0; i < nb_configs; i++) {
        if (server_threads[i])
            pthread_join(server_threads[i], NULL);
        config_free(configs + i);
    }
    av_free(configs);
    av_free(server_threads);
    return 0;
}
