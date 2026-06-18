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
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVFILTER_VF_VAPOURSYNTH_H
#define AVFILTER_VF_VAPOURSYNTH_H

#include <VSScript4.h>
#include <pthread.h>

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
    /* Filter context */
    AVFilterContext *ctx;

    /* VapourSynth API and core */
    const VSAPI *vsapi;
    VSCore *vscore;
    const VSSCRIPTAPI *vs_script_api;
    VSScript *vs_script;

    /* Filter nodes */
    VSNode *in_node;      /**< Input node providing frames to VS */
    VSNode *out_node;     /**< Output node with filtered frames */

    /* Input format */
    int in_width;
    int in_height;
    int in_fmt;

    /* Output format (set after VS init) */
    int out_width;
    int out_height;

    /* Frame buffers */
    AVFrame **buffered;           /**< Input frames waiting for VS */
    int num_buffered;             /**< Number of buffered frames */
    int in_frameno;               /**< Frame number of first buffered */

    /* VS frame storage */
    VSFrame **vs_frames;          /**< Completed VS frames */
    int *vs_frame_numbers;        /**< Frame numbers for vs_frames */
    int max_requests;              /**< Max concurrent requests */
    int out_frameno;              /**< Frame number of next output */

    /* PTS tracking */
    int64_t first_pts;
    int64_t next_pts;

    /* Threading */
    pthread_mutex_t lock;
    pthread_cond_t vs_wakeup;     /**< Signaled when VS frames complete */
    pthread_cond_t input_wakeup;   /**< Signaled when input frames arrive */

    /* State */
    int done;                     /**< Shutdown flag */
    int eof;                      /**< EOF flag */
    int failed;                   /**< Error flag */
    int initializing;              /**< During VS init */

    /* Configuration */
    char *script_path;             /**< Path to .vpy script */
    int maxbuffer;                /**< Max buffered frames */
    int nb_threads;               /**< VS thread count (0=auto) */

} VSContext;

#endif /* AVFILTER_VF_VAPOURSYNTH_H */
