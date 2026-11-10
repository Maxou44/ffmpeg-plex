/*
 * Copyright (c) 2016 Plex, Inc.
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

#include "libavfilter/vf_inlineass.h"
#include "libavformat/http.h"
#include "libavformat/internal.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "plex.h"
#include "ffmpeg.h"
#include "opt_common.h"
#include "config_components.h"

PlexContext plexContext = {0};

#define LOG_LINE_SIZE 1024

#if HAVE_PTHREADS
static pthread_key_t logging_key, using_http_key, cur_line_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static void make_keys(void)
{
    pthread_key_create(&logging_key, NULL);
    pthread_key_create(&using_http_key, NULL);
    pthread_key_create(&cur_line_key, NULL);
}
#endif

static int av_log_level_plex = AV_LOG_QUIET;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int av_log_get_level_plex(void)
{
    return av_log_level_plex;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void av_log_set_level_plex(int level)
{
    av_log_level_plex = level;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
char* PMS_IssueHttpRequest(const char* url, const char* verb)
{
    char* reply = NULL;
    static AVIOContext *ioctx = NULL;
    static AVMutex req_mutex = AV_MUTEX_INITIALIZER;
    int size = 0;
    int ret = 0;
    const char *token = getenv("X_PLEX_TOKEN");
#if HAVE_PTHREADS
    pthread_once(&key_once, make_keys);
#else
    static __thread int using_http = 0;
#endif

#if HAVE_PTHREADS
    if (pthread_getspecific(using_http_key))
        return NULL;
    pthread_setspecific(using_http_key, (void*)1);
#else
    if (using_http)
        return NULL;
    using_http = 1;
#endif

    ff_mutex_lock(&req_mutex);

    if (ioctx) {
        // Try to reuse the existing context
        if ((ret = avformat_http_do_new_request(ioctx, url, verb)) < 0)
            avio_closep(&ioctx);
    }

    if (!ioctx) {
        AVDictionary *settings = NULL;
        av_dict_set(&settings, "method", verb, 0);
        av_dict_set(&settings, "multiple_requests", "1", 0);
        if (token && *token) {
            char headers[1024];
            snprintf(headers, sizeof(headers), "X-Plex-Token: %s\r\nX-Plex-Http-Pipeline: infinite\r\n", token);
            av_dict_set(&settings, "headers", headers, 0);
        }

        ret = avio_open2(&ioctx, url, AVIO_FLAG_READ, NULL, &settings);
        av_dict_free(&settings);
        if (ret < 0)
            goto fail;
    }

    size = avio_size(ioctx);
    if (size < 0)
        size = 4095;

    reply = av_malloc(size + 1);
    if (!reply) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = avio_read(ioctx, reply, size)) < 0)
        goto fail;

    reply[ret] = 0;

fail:
    if (ret < 0) {
        avio_closep(&ioctx);
        if (reply)
            av_freep(&reply);
    }

#if HAVE_PTHREADS
    pthread_setspecific(using_http_key, NULL);
#else
    using_http = 0;
#endif

    ff_mutex_unlock(&req_mutex);

    return reply;
}

void PMS_Log(LogLevel level, const char* format, ...)
{
    // Format the mesage.
    char msg[2048];
    char url[4096];
    va_list va;
    AVBPrint dstbuf;
    if (av_log_level_plex == AV_LOG_QUIET)
        return;

    va_start(va, format);
    vsnprintf(msg, sizeof(msg), format, va);
    va_end(va);

    av_bprint_init_for_buffer(&dstbuf, url, sizeof(url));

    // Build the URL.
    if (plexContext.progress_url)
        av_bprintf(&dstbuf, "%s/log?", plexContext.progress_url);
    else
        av_bprintf(&dstbuf, "http://127.0.0.1:32400/log?source=Transcoder&");
    av_bprintf(&dstbuf, "level=%d&message=", level);
    av_bprint_escape(&dstbuf, msg, NULL, AV_ESCAPE_MODE_URL, 0);

    // Issue the request.
    av_free(PMS_IssueHttpRequest(url, "POST"));
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void plex_log_callback(void* ptr, int level, const char* fmt, va_list vl)
{
    int print_prefix = 1;
    char line[1024];
    va_list vl2;

#if HAVE_PTHREADS
    char *cur_line;
    pthread_once(&key_once, make_keys);
#else
    static __thread char cur_line[LOG_LINE_SIZE] = {0};
    static __thread int logging = 0;
#endif

    va_copy(vl2, vl);
    av_log_default_callback(ptr, level, fmt, vl2);

    if (level > av_log_level_plex)
        return;

    //Avoid recusive logging
#if HAVE_PTHREADS
    if (pthread_getspecific(logging_key))
        return;
    cur_line = pthread_getspecific(cur_line_key);
    if (!cur_line) {
        cur_line = av_mallocz(LOG_LINE_SIZE);
        if (!cur_line)
            return;
        pthread_setspecific(cur_line_key, cur_line);
    }
    pthread_setspecific(logging_key, (void*)1);
#else
    if (logging)
        return;
    logging = 1;
#endif

    print_prefix = cur_line[0] == 0;

    av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);
    av_strlcat(cur_line, line, LOG_LINE_SIZE);
    if (print_prefix) {
        int len = strlen(cur_line);
        if (len) {
            cur_line[len - 1] = 0;
            PMS_Log(level < AV_LOG_ERROR ? LOG_LEVEL_ERROR : (level / 8) - 2, "%s", cur_line);
            cur_line[0] = 0;
        }
    }

#if HAVE_PTHREADS
    pthread_setspecific(logging_key, NULL);
#else
    logging = 0;
#endif
}

void plex_report_stream(const AVStream *st)
{
    if (plexContext.progress_url &&
        (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
         st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) &&
        !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        char url[4096];
        AVBPrint dstbuf;
        const char *profile = avcodec_profile_name(st->codecpar->codec_id, st->codecpar->profile);

        av_bprint_init_for_buffer(&dstbuf, url, sizeof(url));
        av_bprintf(&dstbuf, "%s/stream?index=%i&id=%i&codec=%s&type=%s",
                   plexContext.progress_url, st->index, st->id,
                   avcodec_get_name(st->codecpar->codec_id),
                   av_get_media_type_string(st->codecpar->codec_type));

        if (profile) {
            av_bprintf(&dstbuf, "&profile=");
            av_bprint_escape(&dstbuf, profile, NULL, AV_ESCAPE_MODE_URL, 0);
        }

        av_free(PMS_IssueHttpRequest(url, "PUT"));
    }
}

void plex_report_stream_detail(AVStream *st)
{
    FFStream *const sti = ffstream(st);

    if ((st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
         st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) &&
        sti->codec_info_nb_frames == 0)
        return; // Unparsed stream; will be skipped in output

    if (plexContext.progress_url &&
        (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
         st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
         st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) &&
        !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        char url[4096];
        AVBPrint dstbuf;
        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        const char *profile = avcodec_profile_name(st->codecpar->codec_id, st->codecpar->profile);

        av_bprint_init_for_buffer(&dstbuf, url, sizeof(url));

        av_bprintf(&dstbuf, "%s/streamDetail?index=%i&id=%i&codec=%s&type=%s",
                   plexContext.progress_url, st->index, st->id,
                   avcodec_get_name(st->codecpar->codec_id),
                   av_get_media_type_string(st->codecpar->codec_type));

        if (st->codecpar->bit_rate)
            av_bprintf(&dstbuf, "&bitrate=%"PRId64, st->codecpar->bit_rate);

        if (profile) {
            av_bprintf(&dstbuf, "&profile=");
            av_bprint_escape(&dstbuf, profile, NULL, AV_ESCAPE_MODE_URL, 0);
        }

        if (lang && lang->value && *lang->value) {
            av_bprintf(&dstbuf, "&language=");
            av_bprint_escape(&dstbuf, lang->value, NULL, AV_ESCAPE_MODE_URL, 0);
        }

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            av_bprintf(&dstbuf, "&width=%i&height=%i",
                        st->codecpar->width, st->codecpar->height);
            av_bprintf(&dstbuf, "&interlaced=%i",
                        (st->codecpar->field_order != AV_FIELD_PROGRESSIVE &&
                         st->codecpar->field_order != AV_FIELD_UNKNOWN));
            if (st->codecpar->separate_fields)
                av_bprintf(&dstbuf, "&separateFields=1");
            if (st->codecpar->sample_aspect_ratio.num && st->codecpar->sample_aspect_ratio.den)
                av_bprintf(&dstbuf, "&sar=%d:%d",
                            st->codecpar->sample_aspect_ratio.num,
                            st->codecpar->sample_aspect_ratio.den);
            if (st->codecpar->level != AV_LEVEL_UNKNOWN)
                av_bprintf(&dstbuf, "&level=%d", st->codecpar->level);
            if (st->avg_frame_rate.num && st->avg_frame_rate.den)
                av_bprintf(&dstbuf, "&frameRate=%.3f",
                            av_q2d(st->avg_frame_rate));
#if FF_API_CODEC_PROPS
            if (sti && sti->avctx &&
                sti->avctx->properties & FF_CODEC_PROPERTY_CLOSED_CAPTIONS)
                av_bprintf(&dstbuf, "&closedCaptions=1");
#endif
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            char layout[256];
            char *layoutP = layout;
            char *l, *r;
            av_bprintf(&dstbuf, "&channels=%i", st->codecpar->ch_layout.nb_channels);
            av_channel_layout_describe(&st->codecpar->ch_layout, layout, sizeof(layout));
            l = strchr(layout, '(');
            r = strrchr(layout, ')');
            if (l && r && l > layoutP + 8) {
                layoutP = l + 1;
                *r = 0;
            }
            av_bprintf(&dstbuf, "&layout=");
            av_bprint_escape(&dstbuf, layout, NULL, AV_ESCAPE_MODE_URL, 0);
            av_bprintf(&dstbuf, "&sampleRate=%i", st->codecpar->sample_rate);
            if (st->codecpar->bits_per_raw_sample)
                av_bprintf(&dstbuf, "&bitDepth=%i", st->codecpar->bits_per_raw_sample);
        }

#define SEND_DISPOSITION(flagname, name) if (st->disposition & AV_DISPOSITION_##flagname) \
    av_bprintf(&dstbuf, "&disp_" name "=1");

        SEND_DISPOSITION(DEFAULT,          "default");
        SEND_DISPOSITION(DUB,              "dub");
        SEND_DISPOSITION(ORIGINAL,         "original");
        SEND_DISPOSITION(COMMENT,          "comment");
        SEND_DISPOSITION(LYRICS,           "lyrics");
        SEND_DISPOSITION(KARAOKE,          "karaoke");
        SEND_DISPOSITION(FORCED,           "forced");
        SEND_DISPOSITION(HEARING_IMPAIRED, "hearing_impaired");
        SEND_DISPOSITION(VISUAL_IMPAIRED,  "visual_impaired");
        SEND_DISPOSITION(CLEAN_EFFECTS,    "clean_effects");
        SEND_DISPOSITION(ATTACHED_PIC,     "attached_pic");
        SEND_DISPOSITION(TIMED_THUMBNAILS, "timed_thumbnails");
        SEND_DISPOSITION(CAPTIONS,         "captions");
        SEND_DISPOSITION(DESCRIPTIONS,     "descriptions");
        SEND_DISPOSITION(METADATA,         "metadata");
        SEND_DISPOSITION(DEPENDENT,        "dependent");
        SEND_DISPOSITION(STILL_IMAGE,      "still_image");

        av_free(PMS_IssueHttpRequest(url, "PUT"));
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_init(int argc, char **argv, const OptionDef *options)
{
    int idx;
    av_log_set_callback(plex_log_callback);

    idx = locate_option(argc, argv, options, "loglevel_plex");
    if (idx && argv[idx + 1])
        opt_loglevel_ext("loglevel_plex", argv[idx + 1], av_log_set_level_plex);
    idx = locate_option(argc, argv, options, "progressurl");
    if (idx && argv[idx + 1])
        plex_opt_progress_url(NULL, "progressurl", argv[idx + 1]);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_progress_url(void *optctx, const char *opt, const char *arg)
{
    plexContext.progress_url = (char*)arg;
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_loglevel(void *o, const char *opt, const char *arg)
{
    return opt_loglevel_ext(opt, arg, av_log_set_level_plex);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_feedback(const AVFormatContext *ic)
{
    if (plexContext.progress_url) {
        char url[4096];
        double duration = -1;
        if (ic && ic->duration != AV_NOPTS_VALUE)
            duration = ic->duration / (double)AV_TIME_BASE;
        snprintf(url, sizeof(url), "%s?duration=%f", plexContext.progress_url, duration);
        av_free(PMS_IssueHttpRequest(url, "PUT"));
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_report_progress(int64_t pts, int64_t total_size, int64_t run_time)
{
    static int64_t last_pts = 0;
    static int lastRemaining = 0;
    int64_t secs, totalSecs;
    char url[4096];

    if (pts == AV_NOPTS_VALUE)
        return;

    // Notify about progress.
    secs = pts / AV_TIME_BASE;
    totalSecs = (nb_input_files && input_files[0] && input_files[0]->ctx->duration != AV_NOPTS_VALUE)
              ? input_files[0]->ctx->duration / AV_TIME_BASE
              : 0;

    if (plexContext.progress_url) {
        char* reply;
        int hw_state = -1;
        // Compute speed of transcode as a multiple of real-time.
        float speed = run_time > 0 ? (float)(pts - last_pts) / (float)run_time : 0;
        // Compute estimated time remaining.
        int remainingSecs = speed > 0 ? (totalSecs - secs) / speed : -1;
        int smoothedRemaining = remainingSecs;
        if (lastRemaining != 0)
            smoothedRemaining = lastRemaining*0.5 + remainingSecs*0.5;

        // Sanity check.
        if (smoothedRemaining < 0)
            smoothedRemaining = -1;

        snprintf(url, sizeof(url),
                 "%s?progress=%.1f&size=%lld&remaining=%d",
                 plexContext.progress_url, totalSecs == 0 ? -1 : (float)secs*100.0/totalSecs,
                 (long long)total_size, smoothedRemaining);

        // Only pass back speed if we're not throttled.
        if (atomic_load(&plexContext.throttle_delay) == 0)
            av_strlcatf(url, sizeof(url), "&speed=%.1f", speed);

        for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
            // As long as all video streams (that are decoded) use hw decoding,
            // signal that hw decoding is used.
            if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                ist->decoder &&
                ist->decoder->frames_decoded)
                hw_state = (hw_state < 0 || hw_state == 1) && plex_dec_hwaccel_active(ist->decoder);
        }
        if (hw_state >= 0)
            av_strlcatf(url, sizeof(url), "&vdec_hw_status=%d", hw_state);

        reply = PMS_IssueHttpRequest(url, "PUT");

        // Handle throttling.
        if (reply && strstr(reply, "canThrottle")) {
            if (atomic_load(&plexContext.throttle_delay) == 0)
                PMS_Log(LOG_LEVEL_DEBUG, "Throttle - Going into sloth mode.");

            atomic_store(&plexContext.throttle_delay, 100);
        } else {
            if (atomic_load(&plexContext.throttle_delay) == 100)
                PMS_Log(LOG_LEVEL_DEBUG, "Throttle - Getting back to work.");

            atomic_store(&plexContext.throttle_delay, 0);
        }

        av_free(reply);
        lastRemaining = remainingSecs;
    }
    last_pts = pts;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_report_output_size(const OutputStream *ost)
{
    const AVCodecParameters* par = ost->st->codecpar;
    if (par && par->codec_type == AVMEDIA_TYPE_VIDEO && par->width && plexContext.progress_url) {
        // Compute real width/height based on storage aspect ratio.
        char url[1024];
        int width = par->width;
        if (par->sample_aspect_ratio.num && par->sample_aspect_ratio.den)
            width = av_rescale(width, par->sample_aspect_ratio.num, par->sample_aspect_ratio.den);

        snprintf(url, sizeof(url), "%s?width=%d&height=%d", plexContext.progress_url, width, par->height);
        av_free(PMS_IssueHttpRequest(url, "PUT"));
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_throttle(void)
{
    int delay = atomic_load(&plexContext.throttle_delay);
    if (delay > 0)
        av_usleep(1000 * delay);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The inlineass bindings are shared between the main thread, the decoder
// threads (subtitles) and the filtergraph threads (graph (re)configuration).
static AVMutex inlineass_mutex = AV_MUTEX_INITIALIZER;

#define MAX_PENDING_SUBTITLES 1024

// must be called with inlineass_mutex held
static void flush_pending_subtitles(InlineAssContext *assCtx)
{
    for (int i = 0; i < assCtx->nb_pending; i++) {
#if CONFIG_INLINEASS_FILTER
        AVFrame *frame = assCtx->pending[i];
        if (assCtx->ctx && frame->buf[0])
            avfilter_inlineass_append_data(assCtx->ctx, assCtx->dec_ctx,
                                           (AVSubtitle *)frame->buf[0]->data);
#endif
        av_frame_free(&assCtx->pending[i]);
    }
    assCtx->nb_pending = 0;
}

void plex_uninit(void)
{
    ff_mutex_lock(&inlineass_mutex);
    for (int i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *assCtx = &plexContext.inlineass_ctxs[i];
        assCtx->ctx = NULL;
        flush_pending_subtitles(assCtx);
        av_freep(&assCtx->pending);
    }
    av_freep(&plexContext.inlineass_ctxs);
    plexContext.nb_inlineass_ctxs = 0;
    ff_mutex_unlock(&inlineass_mutex);
}

void plex_prepare_setup_streams_for_input_stream(InputStream* ist)
{
#if CONFIG_INLINEASS_FILTER
    for (int i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *ctx = &plexContext.inlineass_ctxs[i];
        if (ist->st->index == ctx->stream_index &&
            ist->file->index == ctx->file_index)
            // Do not let the demuxer drop the subtitle packets. Whether they
            // are decoded still depends on the stream being used by an
            // output or a filtergraph, like in the 6.1 version.
            ist->st->discard = AVDISCARD_NONE;
    }
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_link_subtitles_to_graph(AVFilterGraph* graph)
{
#if CONFIG_INLINEASS_FILTER
    int contextId = 0;

    if (!graph)
        return;

    ff_mutex_lock(&inlineass_mutex);
    for (unsigned i = 0; i < graph->nb_filters && contextId < plexContext.nb_inlineass_ctxs; i++) {
        AVFilterContext *ctx = graph->filters[i];
        InlineAssContext *assCtx;

        if (strcmp(ctx->filter->name, "inlineass"))
            continue;

        // bind to the first subtitle mapping that is not bound to a live
        // filter yet
        while (contextId < plexContext.nb_inlineass_ctxs &&
               plexContext.inlineass_ctxs[contextId].ctx &&
               plexContext.inlineass_ctxs[contextId].ctx->graph != graph)
            contextId++;
        if (contextId >= plexContext.nb_inlineass_ctxs)
            break;

        assCtx = &plexContext.inlineass_ctxs[contextId++];
        assCtx->ctx = ctx;
        if (assCtx->width && assCtx->height)
            avfilter_inlineass_set_storage_size(ctx, assCtx->width, assCtx->height);

        for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
            if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT)
                avfilter_inlineass_add_attachment(ctx, ist->st);
        }

        avfilter_inlineass_set_fonts(ctx);

        flush_pending_subtitles(assCtx);
    }
    ff_mutex_unlock(&inlineass_mutex);
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_unlink_subtitles_from_graph(AVFilterGraph* graph)
{
#if CONFIG_INLINEASS_FILTER
    if (!graph)
        return;

    ff_mutex_lock(&inlineass_mutex);
    for (int i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *assCtx = &plexContext.inlineass_ctxs[i];
        if (assCtx->ctx && assCtx->ctx->graph == graph)
            assCtx->ctx = NULL;
    }
    ff_mutex_unlock(&inlineass_mutex);
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_subtitle_stream(void *optctx, const char *opt, const char *arg)
{
#if CONFIG_INLINEASS_FILTER
    InlineAssContext *m = NULL;
    int i, file_idx;
    char *p;
    char *map = av_strdup(arg);

    if (!map)
        return AVERROR(ENOMEM);

    file_idx = strtol(map, &p, 0);
    if (file_idx >= nb_input_files || file_idx < 0) {
        av_log(NULL, AV_LOG_FATAL, "Invalid subtitle input file index: %d.\n", file_idx);
        goto finish;
    }

    for (i = 0; i < input_files[file_idx]->nb_streams; i++) {
        if (check_stream_specifier(input_files[file_idx]->ctx, input_files[file_idx]->ctx->streams[i],
                    *p == ':' ? p + 1 : p) <= 0)
            continue;
        if (input_files[file_idx]->ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            av_log(NULL, AV_LOG_ERROR, "Stream '%s' is not a subtitle stream.\n", arg);
            continue;
        }
        if (GROW_ARRAY(plexContext.inlineass_ctxs, plexContext.nb_inlineass_ctxs) < 0) {
            av_freep(&map);
            return AVERROR(ENOMEM);
        }
        m = &plexContext.inlineass_ctxs[plexContext.nb_inlineass_ctxs - 1];

        m->file_index   = file_idx;
        m->stream_index = i;
        break;
    }

finish:
    if (!m)
        av_log(NULL, AV_LOG_ERROR, "Subtitle stream map '%s' matches no streams.\n", arg);

    av_freep(&map);
#endif
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_process_subtitles(const InputStream *ist, AVCodecContext *dec_ctx,
                           const AVFrame *frame)
{
    int ret = 0;
#if CONFIG_INLINEASS_FILTER
    /* If we're burning subtitles, pass the subtitles of the appropriate
     * stream to the subtitle renderer */
    ff_mutex_lock(&inlineass_mutex);
    for (int i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *ctx = &plexContext.inlineass_ctxs[i];
        if (ist->st->index == ctx->stream_index &&
            ist->file->index == ctx->file_index) {
            ctx->dec_ctx = dec_ctx;
            if (!ctx->ctx) {
                // not bound yet: keep it for later
                AVFrame *copy;
                if (ctx->nb_pending < MAX_PENDING_SUBTITLES &&
                    (copy = av_frame_clone(frame))) {
                    if (av_dynarray_add_nofree(&ctx->pending, &ctx->nb_pending, copy) < 0)
                        av_frame_free(&copy);
                }
                ret = 1;
                break;
            }
            avfilter_inlineass_append_data(ctx->ctx, dec_ctx,
                                           (AVSubtitle *)frame->buf[0]->data);
            ret = 2;
            break;
        }
    }
    ff_mutex_unlock(&inlineass_mutex);
#endif
    return ret;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_link_input_stream(const InputStream *ist)
{
#if CONFIG_INLINEASS_FILTER
    if (ist->st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        return;

    ff_mutex_lock(&inlineass_mutex);
    for (int i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        if (plexContext.inlineass_ctxs[i].ctx)
            avfilter_inlineass_set_storage_size(plexContext.inlineass_ctxs[i].ctx, ist->st->codecpar->width, ist->st->codecpar->height);
        plexContext.inlineass_ctxs[i].width = ist->st->codecpar->width;
        plexContext.inlineass_ctxs[i].height = ist->st->codecpar->height;
    }
    ff_mutex_unlock(&inlineass_mutex);
#endif
}
