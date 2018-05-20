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
#include "configreader.h"

#define BUFFER_SECS 30
#define LISTEN_TIMEOUT_MSEC 1000

struct ReadInfo {
    struct PublisherContext *pub;
    AVFormatContext *ifmt_ctx;
    char *input_uri;
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
    int nb_pub; /* number of publishers (streams) equal to number of ifmt_ctx */
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
    struct PublisherContext *pub;
    char status[4096];
    char *stream_name;
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
        if (shutdown)
            break;
        for (i = 0; i < config->nb_streams; i++) {
            publisher_gen_status_json(info->pubs[i], status);
            av_log(server, AV_LOG_INFO, status);
        }
        client = NULL;
        av_log(server, AV_LOG_DEBUG, "Accepting new clients.\n");
        reply_code = 200;

        if ((ret = info->httpd->accept(server, &client, reply_code)) < 0) {
            if (ret == HTTPD_LISTEN_TIMEOUT) {
                continue;
            } else if (ret == HTTPD_CLIENT_ERROR) {
                info->httpd->close(server, client);
            }
            av_log(server, AV_LOG_WARNING, "Error during accept, retrying.\n");
            continue;
        }

        pub = NULL;
        ifmt_ctx = NULL;
        for (i = 0; i < config->nb_streams; i++) {
            stream_name = info->pubs[i]->stream_name;
            //       skip leading '/'  ---v
            if(!strncmp(client->resource + 1, stream_name, strlen(stream_name))) {
                pub = info->pubs[i];
                ifmt_ctx = info->ifmt_ctxs[i];
                break;
            }
        }

        if (!pub || !ifmt_ctx) {
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
        
        avio_buffer = av_malloc(AV_BUFSIZE);
        ffinfo = av_malloc(sizeof(struct FFServerInfo));
        ffinfo->httpd = info->httpd;
        ffinfo->client = client;
        ffinfo->server = server;
        client_ctx = avio_alloc_context(avio_buffer, AV_BUFSIZE, 1, ffinfo, NULL, &ffserver_write, NULL);
        if (!client_ctx) {
            av_log(client, AV_LOG_ERROR, "Could not allocate output format context.\n");
            publisher_cancel_reserve(pub);
            info->httpd->close(server, client);
            av_free(client_ctx->buffer);
            avio_context_free(&client_ctx);
            av_free(ffinfo);
            continue;
        }
        avformat_alloc_output_context2(&ofmt_ctx, NULL, "matroska", NULL);
        if (!ofmt_ctx) {
            av_log(client, AV_LOG_ERROR, "Could not allocate output format context.\n");
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
            av_log(client, AV_LOG_ERROR, "Could not write header to client: %s.\n", av_err2str(ret));
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

void *run_server(void *arg) {
    struct AcceptInfo ainfo;
    struct ReadInfo *rinfos;
    struct WriteInfo **winfos_p;
    struct HTTPDConfig *config = (struct HTTPDConfig*) arg;
    struct PublisherContext **pubs;
    AVFormatContext **ifmt_ctxs;
    int ret, i, stream_index;
    pthread_t *r_threads;
    pthread_t **w_threads_p;
    
    pubs = av_mallocz_array(config->nb_streams, sizeof(struct PublisherContext*));
    ifmt_ctxs = av_mallocz_array(config->nb_streams, sizeof(AVFormatContext*));
    
    av_log_set_level(AV_LOG_INFO);
    
    ainfo.pubs = pubs;
    ainfo.ifmt_ctxs = ifmt_ctxs;
    ainfo.nb_pub = config->nb_streams;
    ainfo.httpd = &lavfhttpd;
    ainfo.config = config;
    
    rinfos = av_mallocz_array(config->nb_streams, sizeof(struct ReadInfo));
    winfos_p = av_mallocz_array(config->nb_streams, sizeof(struct WriteInfo*));
    r_threads = av_mallocz_array(config->nb_streams, sizeof(pthread_t));
    w_threads_p = av_mallocz_array(config->nb_streams, sizeof(pthread_t*));
    
    for (stream_index = 0; stream_index < config->nb_streams; stream_index++) {
        struct PublisherContext *pub = NULL;
        struct AVFormatContext *ifmt_ctx = NULL;
        struct ReadInfo rinfo;
        struct WriteInfo *winfos = NULL;
        pthread_t *w_threads = NULL;
        pthread_t r_thread;
        rinfo.input_uri = config->streams[stream_index].input_uri;

        if ((ret = avformat_open_input(&ifmt_ctx, rinfo.input_uri, NULL, NULL))) {
            av_log(NULL, AV_LOG_ERROR, "run_server: Could not open input\n");
            continue;
        }

        ifmt_ctxs[stream_index] = ifmt_ctx;

        publisher_init(&pub, config->streams[stream_index].stream_name);
        pubs[stream_index] = pub;

        rinfo.ifmt_ctx = ifmt_ctx;
        rinfo.pub = pub;

        rinfos[stream_index] = rinfo;

        w_threads = av_mallocz_array(pub->nb_threads, sizeof(pthread_t));
        winfos = av_mallocz_array(pub->nb_threads, sizeof(struct WriteInfo));

        w_threads_p[stream_index] = w_threads;
        winfos_p[stream_index] = winfos;

        for (i = 0; i < pub->nb_threads; i++) {
            winfos[i].pub = pub;
            winfos[i].thread_id = i;
            pthread_create(&w_threads[i], NULL, write_thread, &winfos_p[stream_index][i]);
        }
        w_threads_p[stream_index] = w_threads;
        pthread_create(&r_thread, NULL, read_thread, &rinfos[stream_index]);
        r_threads[stream_index] = r_thread;
    }


    //pthread_create(&a_thread, NULL, accept_thread, &ainfo);
    accept_thread(&ainfo);
    for (stream_index = 0; stream_index < config->nb_streams; stream_index++) {
        pthread_join(r_threads[stream_index], NULL);
        if (pubs[stream_index]) {
            for (i = 0; i < pubs[stream_index]->nb_threads; i++) {
                pthread_join(w_threads_p[stream_index][i], NULL);
            }
        }
        av_free(winfos_p[stream_index]);
        av_free(w_threads_p[stream_index]);
        // pubs[stream_index] could be null if the file could not be opened
        if (pubs[stream_index])
            publisher_free(pubs[stream_index]);
    }
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
    int i;

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
    for (i = 0; i < nb_configs; i++) {
        config_dump(configs + i);
        pthread_create(&server_threads[i], NULL, run_server, configs + i);
    }

    for (i = 0; i < nb_configs; i++) {
        pthread_join(server_threads[i], NULL);
        config_free(configs + i);
    }
    av_free(configs);
    av_free(server_threads);
    return 0;
}
