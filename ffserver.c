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

#include <libavutil/log.h>
#include <libavutil/timestamp.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "segment.h"
#include "publisher.h"
#include "httpd.h"

#define BUFFER_SECS 30
#define LISTEN_TIMEOUT_MSEC 1000

struct ReadInfo {
    struct PublisherContext *pub;
    AVFormatContext *ifmt_ctx;
    char *in_filename;
};

struct WriteInfo {
    struct PublisherContext *pub;
    int thread_id;
};

struct AcceptInfo {
    struct PublisherContext *pub;
    struct HTTPDInterface *httpd;
    AVFormatContext *ifmt_ctx;
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
    int id = 0;
    int64_t pts, now, start;
    int64_t *ts;
    struct Segment *seg = NULL;
    AVPacket pkt;
    AVStream *in_stream;
    AVRational tb = {1, AV_TIME_BASE};
    AVStream *stream;
    AVCodecParameters *params;
    enum AVMediaType type;
    
    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(ifmt_ctx, AV_LOG_ERROR, "Could not get input stream info.\n");
        goto end;
    }
    
    av_log(ifmt_ctx, AV_LOG_INFO, "Finding video stream.\n");
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        av_log(ifmt_ctx, AV_LOG_DEBUG, "Checking stream %d\n", i);
        stream = ifmt_ctx->streams[i];
        params = stream->codecpar;
        type = params->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO) {
            video_idx = i;
            break;
        }
    }
    if (video_idx == -1) {
        av_log(ifmt_ctx, AV_LOG_ERROR, "No video stream found.\n");
        goto end;
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
        
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, tb, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, tb, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, tb);
        pkt.pos = -1;
        
        // current pts
        pts = pkt.pts;
        
        // current stream "uptime"
        now = av_gettime_relative() - start;

        // simulate real-time reading
        while (pts > now) {
            usleep(1000);
            now = av_gettime_relative() - start;
        }
        
        // keyframe or first Segment
        if ((pkt.flags & AV_PKT_FLAG_KEY && pkt.stream_index == video_idx) || !seg) {
            if (seg) {
                segment_close(seg);
                publisher_push_segment(info->pub, seg);
                av_log(NULL, AV_LOG_DEBUG, "New segment pushed.\n");
                publish(info->pub);
				av_log(NULL, AV_LOG_DEBUG, "Published new segment.\n");
            }
            segment_init(&seg, ifmt_ctx);
            seg->id = id++;
            av_log(NULL, AV_LOG_DEBUG, "Starting new segment, id: %d\n", seg->id);
        }
        
        ts = av_dynarray2_add((void **)&seg->ts, &seg->ts_len, sizeof(int64_t),
                              (const void *)&pkt.dts);
        if (!ts) {
            av_log(seg->fmt_ctx, AV_LOG_ERROR, "could not write dts\n.");
            goto end;
        }
        
        ts = av_dynarray2_add((void **)&seg->ts, &seg->ts_len, sizeof(int64_t),
                              (const void *)&pkt.pts);
        if (!ts) {
            av_log(seg->fmt_ctx, AV_LOG_ERROR, "could not write pts\n.");
            goto end;
        }
        ret = av_write_frame(seg->fmt_ctx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) {
            av_log(seg->fmt_ctx, AV_LOG_ERROR, "av_write_frame() failed.\n");
            goto end;
        }
    }
    
    if (ret < 0 && ret != AVERROR_EOF) {
        av_log(seg->fmt_ctx, AV_LOG_ERROR, "Error occurred during read: %s\n", av_err2str(ret));
        goto end;
    }

    segment_close(seg);
    publisher_push_segment(info->pub, seg);
    publish(info->pub);


end:
    avformat_close_input(&ifmt_ctx);
    info->pub->shutdown = 1;
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
        
        av_fifo_generic_peek(c->buffer, &seg, sizeof(struct Segment*), NULL);
        pthread_mutex_unlock(&c->buffer_lock);
        c->current_segment_id = seg->id;
        info.buf = seg->buf;
        info.left = seg->size;
        
        if (!(fmt_ctx = avformat_alloc_context())) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate format context\n");
            client_disconnect(c, 0);
            return;
        }
        
        avio_buffer = (unsigned char*) av_malloc(AV_BUFSIZE);
        avio_ctx = avio_alloc_context(avio_buffer, AV_BUFSIZE, 0, &info, &segment_read, NULL, NULL);
        
        fmt_ctx->pb = avio_ctx;
        ret = avformat_open_input(&fmt_ctx, NULL, seg->ifmt, NULL);
        if (ret < 0) {
            av_log(avio_ctx, AV_LOG_ERROR, "Could not open input\n");
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            client_disconnect(c, 0);
            return;
        }
        
        ret = avformat_find_stream_info(fmt_ctx, NULL);
        if (ret < 0) {
            av_log(fmt_ctx, AV_LOG_ERROR, "Could not find stream information\n");
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
            ret = av_write_frame(c->ofmt_ctx, &pkt);
            av_packet_unref(&pkt);
            if (ret < 0) {
                av_log(fmt_ctx, AV_LOG_ERROR, "write_frame failed, disconnecting client: %d\n", c->id);
                avformat_close_input(&fmt_ctx);
                av_free(avio_ctx->buffer);
                avio_context_free(&avio_ctx);
                client_disconnect(c, 0);
                return;
            }
        }
        avformat_close_input(&fmt_ctx);
        av_free(avio_ctx->buffer);
        avformat_free_context(fmt_ctx);
        avio_context_free(&avio_ctx);
        pthread_mutex_lock(&c->buffer_lock);
        av_fifo_drain(c->buffer, sizeof(struct Segment*));
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
    char status[4096];
    struct HTTPClient *client = NULL;
    void *server = NULL;
    AVIOContext *client_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    unsigned char *avio_buffer;
    AVOutputFormat *ofmt;
    AVDictionary *mkvopts = NULL;
    AVStream *in_stream, *out_stream;
    int ret, i, reply_code;
    struct HTTPDConfig config = {
        .bind_address = "0",
        .port = 8080,
        .accept_timeout = LISTEN_TIMEOUT_MSEC,
    };
    
    info->httpd->init(&server, config);
    
    
    for (;;) {
        if (info->pub->shutdown)
            break;
        publisher_gen_status_json(info->pub, status);
        av_log(server, AV_LOG_INFO, status);
        client = NULL;
        av_log(server, AV_LOG_DEBUG, "Accepting new clients.\n");
        reply_code = 200;
        if (publisher_reserve_client(info->pub)) {
            av_log(client, AV_LOG_WARNING, "No more client slots free, Returning 503.\n");
            reply_code = 503;
        }
        
        if ((ret = info->httpd->accept(server, &client, reply_code)) < 0) {
            if (ret == HTTPD_LISTEN_TIMEOUT) {
                publisher_cancel_reserve(info->pub);
                continue;
            } else if (ret == HTTPD_CLIENT_ERROR) {
                info->httpd->close(server, client);
            }
            av_log(server, AV_LOG_WARNING, "Error during accept, retrying.\n");
            publisher_cancel_reserve(info->pub);
            continue;
        }
        
        if (reply_code != 200) {
            publisher_cancel_reserve(info->pub);
            info->httpd->close(server, client);
            continue;
        }
        
        avio_buffer = av_malloc(AV_BUFSIZE);
        ffinfo = av_malloc(sizeof(struct FFServerInfo));
        ffinfo->httpd = info->httpd;
        ffinfo->client = client;
        ffinfo->server = server;
        client_ctx = avio_alloc_context(avio_buffer, AV_BUFSIZE, 1, ffinfo, NULL, &ffserver_write, NULL);
        if (!client_ctx) {
            av_log(client, AV_LOG_ERROR, "Could not allocate output format context.\n");
            publisher_cancel_reserve(info->pub);
            info->httpd->close(server, client);
            av_free(client_ctx->buffer);
            avio_context_free(&client_ctx);
            av_free(ffinfo);
            continue;
        }
        avformat_alloc_output_context2(&ofmt_ctx, NULL, "matroska", NULL);
        if (!ofmt_ctx) {
            av_log(client, AV_LOG_ERROR, "Could not allocate output format context.\n");
            publisher_cancel_reserve(info->pub);
            info->httpd->close(server, client);
            avformat_free_context(ofmt_ctx);
            av_free(client_ctx->buffer);
            avio_context_free(&client_ctx);
            av_free(ffinfo);
            continue;
        }
        if ((ret = av_dict_set(&mkvopts, "live", "1", 0)) < 0) {
            av_log(client, AV_LOG_ERROR, "Failed to set live mode for matroska: %s\n", av_err2str(ret));
            publisher_cancel_reserve(info->pub);
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
        
        for (i = 0; i < info->ifmt_ctx->nb_streams; i++) {
            in_stream = info->ifmt_ctx->streams[i];
            out_stream = avformat_new_stream(ofmt_ctx, NULL);
            
            if (!out_stream) {
                av_log(client, AV_LOG_ERROR, "Could not allocate output stream.\n");
                publisher_cancel_reserve(info->pub);
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
                publisher_cancel_reserve(info->pub);
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
        av_dict_copy(&info->ifmt_ctx->metadata, ofmt_ctx->metadata, 0);
        ofmt_ctx->pb = client_ctx;
        ret = avformat_write_header(ofmt_ctx, &mkvopts);
        if (ret < 0) {
            av_log(client, AV_LOG_ERROR, "Could not write header to client: %s.\n", av_err2str(ret));
            publisher_cancel_reserve(info->pub);
            info->httpd->close(server, client);
            avformat_free_context(ofmt_ctx);
            av_free(client_ctx->buffer);
            avio_context_free(&client_ctx);
            av_free(ffinfo);
            continue;
        }
        publisher_add_client(info->pub, ofmt_ctx, ffinfo);
        ofmt_ctx = NULL;
        
    }
    av_log(server, AV_LOG_INFO, "Shutting down http server.\n");
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


int main(int argc, char *argv[])
{
    struct ReadInfo rinfo;
    struct AcceptInfo ainfo;
    struct WriteInfo *winfos;
    struct PublisherContext *pub;
    int ret, i;
    pthread_t r_thread, a_thread;
    pthread_t *w_threads;
    
    AVFormatContext *ifmt_ctx = NULL;
    
    rinfo.in_filename = "pipe:0";
    if (argc > 1)
        rinfo.in_filename = argv[1];
    
    av_log_set_level(AV_LOG_INFO);
    
    if ((ret = avformat_open_input(&ifmt_ctx, rinfo.in_filename, NULL, NULL))) {
        av_log(NULL, AV_LOG_ERROR, "main: Could not open input\n");
        return 1;
    }
    
    publisher_init(&pub);
    
    rinfo.ifmt_ctx = ifmt_ctx;
    rinfo.pub = pub;
    ainfo.ifmt_ctx = ifmt_ctx;
    ainfo.pub = pub;
    ainfo.httpd = &lavfhttpd;
    
    w_threads = (pthread_t*) av_malloc(sizeof(pthread_t) * pub->nb_threads);
    winfos = (struct WriteInfo*) av_malloc(sizeof(struct WriteInfo) * pub->nb_threads);
    
    for (i = 0; i < pub->nb_threads; i++) {
        winfos[i].pub = pub;
        winfos[i].thread_id = i;
        pthread_create(&w_threads[i], NULL, write_thread, &winfos[i]);
    }
    
    pthread_create(&r_thread, NULL, read_thread, &rinfo);
    
    accept_thread(&ainfo);
    
    pthread_join(r_thread, NULL);
    
    for (i = 0; i < pub->nb_threads; i++) {
        pthread_join(w_threads[i], NULL);
    }
    av_free(w_threads);
    av_free(winfos);
    
    publisher_freep(&pub);
    return 0;
}
