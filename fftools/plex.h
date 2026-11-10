/*
 * Copyright (c) 2025 Plex, Inc.
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

#ifndef FFTOOLS_PLEX_H
#define FFTOOLS_PLEX_H

#include <stdatomic.h>

#include "cmdutils.h"
#include "ffmpeg.h"
#include "libavfilter/avfilter.h"

typedef struct
{
    int file_index;
    int stream_index;
    AVFilterContext *ctx;
    int width, height;

    /* subtitles decoded while no inlineass instance is bound (e.g. before
     * the filtergraph is configured); fed to the filter once bound */
    AVFrame **pending;
    int nb_pending;
    AVCodecContext *dec_ctx;
} InlineAssContext;

typedef struct
{
    char* progress_url;
    /* written by the main thread, read by the demuxer threads */
    atomic_int throttle_delay;

    int nb_inlineass_ctxs;
    InlineAssContext *inlineass_ctxs;
} PlexContext;

extern PlexContext plexContext;

typedef enum LogLevel
{
   LOG_LEVEL_ERROR,
   LOG_LEVEL_WARNING,
   LOG_LEVEL_INFO,
   LOG_LEVEL_DEBUG,
   LOG_LEVEL_VERBOSE,
} LogLevel;

char* PMS_IssueHttpRequest(const char* url, const char* verb);
void PMS_Log(LogLevel level, const char* format, ...);

void plex_init(int argc, char **argv, const OptionDef *options);
int av_log_get_level_plex(void);
void av_log_set_level_plex(int);

void plex_report_stream(const AVStream *st);
void plex_report_stream_detail(AVStream *st);

int plex_opt_progress_url(void *optctx, const char *opt, const char *arg);
int plex_opt_loglevel(void *o, const char *opt, const char *arg);

void plex_feedback(const AVFormatContext *ic);

int plex_opt_subtitle_stream(void *optctx, const char *opt, const char *arg);

/**
 * Called by the main thread before the output streams are created.
 */
void plex_prepare_setup_streams_for_input_stream(InputStream* ist);

/**
 * Called by a filtergraph thread once its graph is configured: binds the
 * inlineass instances of that graph to their subtitle stream.
 */
void plex_link_subtitles_to_graph(AVFilterGraph* graph);

/**
 * Called by a filtergraph thread before its graph is freed.
 */
void plex_unlink_subtitles_from_graph(AVFilterGraph* graph);

/**
 * Called by a decoder thread for every decoded subtitle.
 */
int plex_process_subtitles(const InputStream *ist, AVCodecContext *dec_ctx,
                           const AVFrame *frame);

/**
 * Free the pending subtitles; called at exit.
 */
void plex_uninit(void);

void plex_link_input_stream(const InputStream *ist);

/**
 * Report progress to PMS and handle throttling; called by the main thread
 * from print_report().
 */
void plex_report_progress(int64_t pts, int64_t total_size, int64_t run_time);

/**
 * Report the output video dimensions to PMS.
 */
void plex_report_output_size(const OutputStream *ost);

/**
 * Sleep if PMS asked us to throttle; called by the demuxer threads.
 */
void plex_throttle(void);

/* implemented in ffmpeg_dec.c */
int plex_dec_hwaccel_active(const Decoder *dec);

#endif
