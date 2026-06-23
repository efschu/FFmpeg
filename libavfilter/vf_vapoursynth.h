/*
 * VapourSynth filter for FFmpeg
 *
 * Copyright (c) 2024 efschu
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but without ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVFILTER_VF_VAPOURSYNTH_H
#define AVFILTER_VF_VAPOURSYNTH_H

#include <pthread.h>
#include <vapoursynth/VSScript4.h>

#include "avfilter.h"

/**
 * Maximum number of buffered input frames
 */
#define VS_MAX_BUFFER 64

/**
 * Maximum number of concurrent VapourSynth frame requests
 */
#define VS_MAX_REQUESTS 32

/**
 * VapourSynth filter context - manages the bridge between FFmpeg and VapourSynth
 */
typedef struct VSContext {
    /* AVFilter context */
    AVFilterContext *ctx;

    /* VapourSynth API and core */
    const VSAPI *vsapi;
    VSCore *vscore;
    const VSSCRIPTAPI *vs_script_api;
    VSScript *vs_script;

    /* Filter nodes */
    VSNode *in_node;
    VSNode *out_node;

    /* Input format - set when first frame arrives */
    int in_width;
    int in_height;
    int in_fmt;

    /* Output format - set after VS init */
    int out_width;
    int out_height;

    /* Frame buffers */
    AVFrame **buffered;
    int num_buffered;
    int in_frameno;

    /* VS frame storage */
    VSFrame **vs_frames;
    int *vs_frame_numbers;
    int max_requests;
    int out_frameno;

    /* PTS tracking */
    int64_t first_pts;
    int64_t next_pts;

    /* Threading */
    pthread_mutex_t lock;
    pthread_cond_t vs_wakeup;
    pthread_cond_t input_wakeup;

    /* State */
    int done;
    int eof;
    int failed;
    int initializing;
    int initialized;

    /* Configuration */
    char *script_path;
    int maxbuffer;
    int nb_threads;

} VSContext;

#endif /* AVFILTER_VF_VAPOURSYNTH_H */
