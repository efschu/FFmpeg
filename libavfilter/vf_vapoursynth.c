/*
 * VapourSynth video filter for FFmpeg
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <dlfcn.h>
#include <pthread.h>

#include "libavutil/imgutils.h"
#include "libavutil/pixfmt.h"
#include "libavutil/opt.h"
#include "libavutil/avutil.h"
#include "libavutil/log.h"
#include "libavutil/cpu.h"
#include "libavutil/mem.h"

#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"
#include "vf_vapoursynth.h"

/* VapourSynth library names (R73) */
static const char *vsscript_lib_names[] = {
#ifdef _WIN32
    "VSScript.dll",
#elif defined(__APPLE__)
    "libvsscript.dylib",
    "libvapoursynth-script.dylib",
#else
    "libvapoursynth-script.so",
    "libvsscript.so",
#endif
};

#define OFFSET(x) offsetof(VSContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption vs_options[] = {
    { "file",        "VapourSynth script file (.vpy)", OFFSET(script_path),
                      AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "maxbuffer",   "Maximum number of buffered frames", OFFSET(maxbuffer),
                      AV_OPT_TYPE_INT, { .i64 = 16 }, 1, VS_MAX_BUFFER, FLAGS },
    { "maxrequests", "Maximum concurrent VS frame requests", OFFSET(max_requests),
                      AV_OPT_TYPE_INT, { .i64 = 8 }, 1, VS_MAX_REQUESTS, FLAGS },
    { "threads",     "Number of threads for VapourSynth (0=auto)", OFFSET(nb_threads),
                      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 64, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(vapoursynth);

/**
 * Convert FFmpeg pixel format to VapourSynth video format ID.
 * Returns pfNone (0) on failure.
 */
static int get_vs_video_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return pfYUV420P8;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
        return pfYUV422P8;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
        return pfYUV444P8;
    case AV_PIX_FMT_YUV410P:
        return pfYUV410P8;
    case AV_PIX_FMT_YUV411P:
        return pfYUV411P8;
    case AV_PIX_FMT_GRAY8:
        return pfGray8;
    case AV_PIX_FMT_YUV420P9:
        return pfYUV420P9;
    case AV_PIX_FMT_YUV422P9:
        return pfYUV422P9;
    case AV_PIX_FMT_YUV444P9:
        return pfYUV444P9;
    case AV_PIX_FMT_YUV420P10:
        return pfYUV420P10;
    case AV_PIX_FMT_YUV422P10:
        return pfYUV422P10;
    case AV_PIX_FMT_YUV444P10:
        return pfYUV444P10;
    case AV_PIX_FMT_YUV420P12:
        return pfYUV420P12;
    case AV_PIX_FMT_YUV422P12:
        return pfYUV422P12;
    case AV_PIX_FMT_YUV444P12:
        return pfYUV444P12;
    case AV_PIX_FMT_YUV420P14:
        return pfYUV420P14;
    case AV_PIX_FMT_YUV422P14:
        return pfYUV422P14;
    case AV_PIX_FMT_YUV444P14:
        return pfYUV444P14;
    case AV_PIX_FMT_YUV420P16:
        return pfYUV420P16;
    case AV_PIX_FMT_YUV422P16:
        return pfYUV422P16;
    case AV_PIX_FMT_YUV444P16:
        return pfYUV444P16;
    case AV_PIX_FMT_GRAY16:
        return pfGray16;
    default:
        return pfNone;
    }
}

/**
 * Convert VapourSynth video format to FFmpeg pixel format.
 * Returns AV_PIX_FMT_NONE on failure.
 */
static enum AVPixelFormat vs_to_ff_pix_fmt(int vs_format)
{
    switch (vs_format) {
    case pfGray8:   return AV_PIX_FMT_GRAY8;
    case pfGray16:  return AV_PIX_FMT_GRAY16;
    case pfYUV420P8:   return AV_PIX_FMT_YUV420P;
    case pfYUV422P8:   return AV_PIX_FMT_YUV422P;
    case pfYUV444P8:   return AV_PIX_FMT_YUV444P;
    case pfYUV410P8:   return AV_PIX_FMT_YUV410P;
    case pfYUV411P8:   return AV_PIX_FMT_YUV411P;
    case pfYUV420P9:   return AV_PIX_FMT_YUV420P9;
    case pfYUV422P9:   return AV_PIX_FMT_YUV422P9;
    case pfYUV444P9:   return AV_PIX_FMT_YUV444P9;
    case pfYUV420P10:  return AV_PIX_FMT_YUV420P10;
    case pfYUV422P10:  return AV_PIX_FMT_YUV422P10;
    case pfYUV444P10:  return AV_PIX_FMT_YUV444P10;
    case pfYUV420P12:  return AV_PIX_FMT_YUV420P12;
    case pfYUV422P12:  return AV_PIX_FMT_YUV422P12;
    case pfYUV444P12:  return AV_PIX_FMT_YUV444P12;
    case pfYUV420P14:  return AV_PIX_FMT_YUV420P14;
    case pfYUV422P14:  return AV_PIX_FMT_YUV422P14;
    case pfYUV444P14:  return AV_PIX_FMT_YUV444P14;
    case pfYUV420P16:  return AV_PIX_FMT_YUV420P16;
    case pfYUV422P16:  return AV_PIX_FMT_YUV422P16;
    case pfYUV444P16:  return AV_PIX_FMT_YUV444P16;
    default:         return AV_PIX_FMT_NONE;
    }
}

/**
 * Copy frame data from FFmpeg to VapourSynth.
 */
static int copy_frame_to_vs(const VSAPI *vsapi, VSFrame *dst,
                            const AVFrame *src, const VSVideoFormat *vsfmt)
{
    int width = vsapi->getFrameWidth(dst, 0);
    int height = vsapi->getFrameHeight(dst, 0);
    int num_planes = vsfmt->numPlanes;
    int bytes_per_sample = (vsfmt->bitsPerSample + 7) >> 3;

    for (int p = 0; p < num_planes && p < AV_NUM_DATA_POINTERS; p++) {
        uint8_t *dst_ptr = vsapi->getWritePtr(dst, p);
        int dst_stride = vsapi->getStride(dst, p);
        const uint8_t *src_ptr = src->data[p];
        int src_stride = src->linesize[p];

        int plane_w = (p == 0) ? width : (width + vsfmt->subSamplingW) >> vsfmt->subSamplingW;
        int plane_h = (p == 0) ? height : (height + vsfmt->subSamplingH) >> vsfmt->subSamplingH;

        if (src_stride == dst_stride && dst_stride == plane_w * bytes_per_sample) {
            memcpy(dst_ptr, src_ptr, plane_h * dst_stride);
        } else {
            for (int y = 0; y < plane_h; y++) {
                memcpy(dst_ptr + y * dst_stride,
                       src_ptr + y * src_stride,
                       plane_w * bytes_per_sample);
            }
        }
    }
    return 0;
}

/**
 * Copy frame data from VapourSynth to FFmpeg.
 */
static int copy_frame_from_vs(const VSAPI *vsapi, AVFrame *dst,
                              const VSFrame *src, int vs_format,
                              int width, int height)
{
    const VSVideoFormat *vsfmt = vsapi->getVideoFrameFormat(src);

    enum AVPixelFormat out_fmt = vs_to_ff_pix_fmt(vs_format);
    if (out_fmt == AV_PIX_FMT_NONE) {
        return AVERROR(EINVAL);
    }

    if (dst->format != out_fmt || dst->width != width || dst->height != height) {
        av_frame_unref(dst);
        dst->format = out_fmt;
        dst->width = width;
        dst->height = height;
        int ret = av_frame_get_buffer(dst, 0);
        if (ret < 0)
            return ret;
    }

    int num_planes = vsfmt->numPlanes;
    int bytes_per_sample = (vsfmt->bitsPerSample + 7) >> 3;

    for (int p = 0; p < num_planes && p < AV_NUM_DATA_POINTERS; p++) {
        const uint8_t *src_ptr = vsapi->getReadPtr(src, p);
        int src_stride = vsapi->getStride(src, p);
        uint8_t *dst_ptr = dst->data[p];
        int dst_stride = dst->linesize[p];

        int plane_w = (p == 0) ? width : (width + vsfmt->subSamplingW) >> vsfmt->subSamplingW;
        int plane_h = (p == 0) ? height : (height + vsfmt->subSamplingH) >> vsfmt->subSamplingH;

        if (src_stride == dst_stride && dst_stride == plane_w * bytes_per_sample) {
            memcpy(dst_ptr, src_ptr, plane_h * dst_stride);
        } else {
            for (int y = 0; y < plane_h; y++) {
                memcpy(dst_ptr + y * dst_stride,
                       src_ptr + y * src_stride,
                       plane_w * bytes_per_sample);
            }
        }
    }
    return 0;
}

/**
 * VapourSynth input filter callback - provides frames to VS.
 * Called by VapourSynth when it needs an input frame.
 */
static const VSFrame *VS_CC vs_infilter_get_frame(int frameno, int activationReason,
                                                  void *instanceData, void **frameData,
                                                  VSFrameContext *frameCtx, VSCore *core,
                                                  const VSAPI *vsapi)
{
    VSContext *vs = instanceData;
    VSFrame *ret = NULL;

    pthread_mutex_lock(&vs->lock);

    if (vs->done || vs->failed) {
        vsapi->setFilterError("Filter shutdown", frameCtx);
        pthread_mutex_unlock(&vs->lock);
        return NULL;
    }

    /* Wait for the requested frame to be buffered */
    while (1) {
        if (frameno >= vs->in_frameno &&
            frameno < vs->in_frameno + vs->num_buffered) {
            AVFrame *avframe = vs->buffered[frameno - vs->in_frameno];
            int vs_format = get_vs_video_format(avframe->format);
            if (vs_format == pfNone) {
                vsapi->setFilterError("Unsupported pixel format", frameCtx);
                break;
            }
            const VSVideoFormat *vsfmt = vsapi->getVideoFormatDescriptor(vs_format);
            ret = vsapi->newVideoFrame(vsfmt, avframe->width, avframe->height, NULL, core);
            if (ret) {
                copy_frame_to_vs(vsapi, ret, avframe, vsfmt);
            }
            break;
        }

        if (vs->eof) {
            vsapi->setFilterError("End of file", frameCtx);
            break;
        }

        /* Signal that we want more input */
        pthread_cond_broadcast(&vs->input_wakeup);
        /* Wait for more input or shutdown */
        pthread_cond_wait(&vs->vs_wakeup, &vs->lock);
    }

    pthread_mutex_unlock(&vs->lock);
    return ret;
}

/**
 * VapourSynth input filter free callback.
 */
static void VS_CC vs_infilter_free(void *instanceData, VSCore *core,
                                    const VSAPI *vsapi)
{
    VSContext *vs = instanceData;
    pthread_mutex_lock(&vs->lock);
    pthread_cond_signal(&vs->input_wakeup);
    pthread_mutex_unlock(&vs->lock);
}

/**
 * VapourSynth output callback - called when a frame is ready.
 */
static void VS_CC vs_frame_done(void *userData, const VSFrame *f, int n,
                                VSNode *node, const char *errorMsg)
{
    VSContext *vs = userData;
    AVFilterContext *ctx = vs->ctx;

    pthread_mutex_lock(&vs->lock);

    if (errorMsg && !f) {
        av_log(ctx, AV_LOG_ERROR, "VS filter error at frame %d: %s\n", n, errorMsg);
        vs->failed = 1;
        pthread_cond_broadcast(&vs->vs_wakeup);
        pthread_mutex_unlock(&vs->lock);
        return;
    }

    if (f) {
        int idx = n - vs->out_frameno;
        if (idx >= 0 && idx < vs->max_requests) {
            /* Drop the old frame if any */
            if (vs->vs_frames[idx])
                vs->vsapi->freeFrame(vs->vs_frames[idx]);
            vs->vs_frames[idx] = (VSFrame *)f;
            vs->vs_frame_numbers[idx] = n;
        } else {
            /* Out of range - drop it */
            vs->vsapi->freeFrame((VSFrame *)f);
        }
    }

    pthread_cond_broadcast(&vs->vs_wakeup);
    pthread_mutex_unlock(&vs->lock);
}

/**
 * Load VapourSynth library and get APIs.
 */
static int init_vs_lib(VSContext *vs)
{
    void *vsscript_lib = NULL;
    const char *vsscript_path = getenv("VSSCRIPT_PATH");

    /* Try explicit path first */
    if (vsscript_path) {
        vsscript_lib = dlopen(vsscript_path, RTLD_NOW | RTLD_GLOBAL);
    }
    /* Try default names */
    if (!vsscript_lib) {
        for (size_t i = 0; i < FF_ARRAY_ELEMS(vsscript_lib_names); i++) {
            vsscript_lib = dlopen(vsscript_lib_names[i], RTLD_NOW | RTLD_GLOBAL);
            if (vsscript_lib)
                break;
        }
    }

    if (!vsscript_lib) {
        av_log(vs->ctx, AV_LOG_ERROR,
               "Failed to load VapourSynth script library: %s\n",
               dlerror() ? dlerror() : "unknown error");
        return AVERROR(EINVAL);
    }

    /* Get VSScript API */
    int (*getAPIVersion)(void) = dlsym(vsscript_lib, "vsscriptGetAPIVersion");
    if (!getAPIVersion) {
        av_log(vs->ctx, AV_LOG_ERROR, "Failed to find vsscriptGetAPIVersion\n");
        return AVERROR(EINVAL);
    }
    int api_version = getAPIVersion();
    av_log(vs->ctx, AV_LOG_DEBUG, "VSScript API version: %d.%d\n",
           api_version >> 16, api_version & 0xFFFF);

    /* Get VSSCRIPTAPI struct */
    typedef const VSSCRIPTAPI *(*getVSScriptAPI_t)(int version);
    getVSScriptAPI_t getVSScriptAPI = dlsym(vsscript_lib, "getVSScriptAPI");
    if (!getVSScriptAPI) {
        av_log(vs->ctx, AV_LOG_ERROR, "Failed to find getVSScriptAPI\n");
        return AVERROR(EINVAL);
    }
    vs->vs_script_api = getVSScriptAPI(VSSCRIPT_API_VERSION);
    if (!vs->vs_script_api) {
        av_log(vs->ctx, AV_LOG_ERROR,
               "Failed to get VSScript API v%d.%d\n",
               VSSCRIPT_API_VERSION >> 16, VSSCRIPT_API_VERSION & 0xFFFF);
        return AVERROR(EINVAL);
    }

    /* Get VSAPI from VSScript API (no need to dlopen libvapoursynth separately) */
    vs->vsapi = vs->vs_script_api->getVSAPI(VAPOURSYNTH_API_VERSION);
    if (!vs->vsapi) {
        av_log(vs->ctx, AV_LOG_ERROR,
               "Failed to get VSAPI v%d.%d\n",
               VAPOURSYNTH_API_VERSION >> 16, VAPOURSYNTH_API_VERSION & 0xFFFF);
        return AVERROR(EINVAL);
    }

    return 0;
}

/**
 * Load and evaluate VapourSynth script.
 */
static int load_vs_script(VSContext *vs)
{
    AVFilterContext *ctx = vs->ctx;

    /* Create the script */
    vs->vs_script = vs->vs_script_api->createScript(NULL);
    if (!vs->vs_script) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create VapourSynth script\n");
        return AVERROR(ENOMEM);
    }

    /* Get the core */
    vs->vscore = vs->vs_script_api->getCore(vs->vs_script);
    if (!vs->vscore) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get VapourSynth core\n");
        return AVERROR(ENOMEM);
    }

    /* Set thread count */
    if (vs->nb_threads <= 0)
        vs->nb_threads = av_cpu_count();
    if (vs->nb_threads < 1)
        vs->nb_threads = 1;
    vs->vsapi->setThreadCount(vs->nb_threads, vs->vscore);

    /* Create the input filter node */
    int in_vs_format = get_vs_video_format(vs->in_fmt);
    if (in_vs_format == pfNone) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input pixel format\n");
        return AVERROR(EINVAL);
    }
    const VSVideoFormat *in_vsfmt = vs->vsapi->getVideoFormatDescriptor(in_vs_format);

    VSVideoInfo vi_in = {
        .format = *in_vsfmt,
        .width = vs->in_width,
        .height = vs->in_height,
        .numFrames = INT_MAX / 16,
    };

    vs->in_node = vs->vsapi->createVideoFilter2(
        "FFmpegInput", &vi_in,
        vs_infilter_get_frame, vs_infilter_free,
        fmParallel, NULL, 0, vs, vs->vscore);

    if (!vs->in_node) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create VapourSynth input node\n");
        return AVERROR(EINVAL);
    }

    /* Set up script variables (clip, w, h) */
    VSMap *vars = vs->vsapi->createMap();
    vs->vsapi->mapSetNode(vars, "clip", vs->in_node, maReplace);
    vs->vsapi->mapSetInt(vars, "w", vs->in_width, maReplace);
    vs->vsapi->mapSetInt(vars, "h", vs->in_height, maReplace);
    vs->vs_script_api->setVariables(vs->vs_script, vars);
    vs->vsapi->freeMap(vars);

    /* Evaluate the script file */
    if (vs->vs_script_api->evaluateFile(vs->vs_script, vs->script_path)) {
        const char *error = vs->vs_script_api->getError(vs->vs_script);
        av_log(ctx, AV_LOG_ERROR, "Script evaluation failed: %s\n",
               error ? error : "unknown error");
        return AVERROR(EINVAL);
    }

    /* Get the output node */
    vs->out_node = vs->vs_script_api->getOutputNode(vs->vs_script, 0);
    if (!vs->out_node) {
        av_log(ctx, AV_LOG_ERROR, "No output node from VapourSynth script\n");
        return AVERROR(EINVAL);
    }

    /* Get output video info */
    const VSVideoInfo *vi_out = vs->vsapi->getVideoInfo(vs->out_node);
    vs->out_width = vi_out->width;
    vs->out_height = vi_out->height;

    av_log(ctx, AV_LOG_INFO,
           "VapourSynth: %dx%d -> %dx%d, threads=%d\n",
           vs->in_width, vs->in_height,
           vs->out_width, vs->out_height, vs->nb_threads);

    return 0;
}

/**
 * Request frames from VapourSynth asynchronously.
 */
static void request_vs_frames(VSContext *vs)
{
    if (!vs->out_node || vs->eof || vs->failed || vs->done)
        return;

    for (int i = 0; i < vs->max_requests; i++) {
        if (!vs->vs_frames[i]) {
            int frame_num = vs->out_frameno + i;
            vs->vsapi->getFrameAsync(frame_num, vs->out_node,
                                    vs_frame_done, vs);
        }
    }
}

/**
 * Get a ready frame from the buffer.
 * Returns 0 if a frame was retrieved (and *out_frame is set).
 * Returns AVERROR(EAGAIN) if no frame is ready yet.
 */
static int get_ready_frame(VSContext *vs, AVFrame **out_frame)
{
    for (int i = 0; i < vs->max_requests; i++) {
        if (vs->vs_frames[i]) {
            const VSFrame *src = vs->vs_frames[i];
            int frame_num = vs->vs_frame_numbers[i];
            const VSVideoInfo *vi = vs->vsapi->getVideoInfo(vs->out_node);

            AVFrame *out = av_frame_alloc();
            if (!out)
                return AVERROR(ENOMEM);

            int ret = copy_frame_from_vs(vs->vsapi, out, src, vi->format.id,
                                         vs->out_width, vs->out_height);
            vs->vsapi->freeFrame(vs->vs_frames[i]);
            vs->vs_frames[i] = NULL;
            vs->vs_frame_numbers[i] = -1;

            if (ret < 0) {
                av_frame_free(&out);
                return ret;
            }

            *out_frame = out;
            vs->out_frameno = frame_num + 1;
            return 0;
        }
    }
    return AVERROR(EAGAIN);
}

/**
 * Check if all pending frames are done (no more output).
 */
static int pending_frames(VSContext *vs)
{
    for (int i = 0; i < vs->max_requests; i++) {
        if (vs->vs_frames[i])
            return 1;
    }
    return 0;
}

/**
 * Filter activate function.
 */
static int activate(AVFilterContext *ctx)
{
    VSContext *vs = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;

    pthread_mutex_lock(&vs->lock);

    /* First-time initialization (after we know the input format) */
    if (!vs->initialized && !vs->initializing && !vs->failed) {
        AVFrame *first_frame = NULL;

        /* Need a first frame to know the input format */
        if (vs->num_buffered == 0) {
            pthread_mutex_unlock(&vs->lock);
            ret = ff_inlink_consume_frame(inlink, &first_frame);
            if (ret <= 0)
                return ret;
            pthread_mutex_lock(&vs->lock);
        }

        if (vs->num_buffered > 0) {
            first_frame = vs->buffered[0];
        }

        if (first_frame) {
            vs->in_width = first_frame->width;
            vs->in_height = first_frame->height;
            vs->in_fmt = first_frame->format;

            /* Buffer it */
            if (vs->num_buffered == 0) {
                vs->buffered[0] = first_frame;
                vs->num_buffered = 1;
            }

            /* Load VapourSynth */
            vs->initializing = 1;
            pthread_mutex_unlock(&vs->lock);

            ret = init_vs_lib(vs);
            if (ret >= 0)
                ret = load_vs_script(vs);

            pthread_mutex_lock(&vs->lock);
            vs->initializing = 0;

            if (ret < 0) {
                vs->failed = 1;
                pthread_mutex_unlock(&vs->lock);
                av_frame_free(&first_frame);
                return ret;
            }

            vs->initialized = 1;

            /* Notify the system that output format changed */
            outlink->w = vs->out_width;
            outlink->h = vs->out_height;
            outlink->format = vs_to_ff_pix_fmt(
                vs->vsapi->getVideoInfo(vs->out_node)->format.id);

            /* Start requesting frames */
            request_vs_frames(vs);
        }
    }

    /* Try to output ready frames */
    if (vs->initialized) {
        AVFrame *out_frame = NULL;
        pthread_mutex_unlock(&vs->lock);

        ret = get_ready_frame(vs, &out_frame);
        if (ret == 0 && out_frame) {
            /* Set output PTS */
            if (vs->first_pts == AV_NOPTS_VALUE)
                vs->first_pts = out_frame->pts;
            if (out_frame->pts == AV_NOPTS_VALUE)
                out_frame->pts = vs->next_pts;
            vs->next_pts = out_frame->pts + 1;

            return ff_filter_frame(outlink, out_frame);
        }

        pthread_mutex_lock(&vs->lock);

        /* Check for EOF */
        if (vs->eof && vs->num_buffered == 0 && !pending_frames(vs)) {
            pthread_mutex_unlock(&vs->lock);
            return AVERROR_EOF;
        }
    }

    /* Consume input frames into the buffer */
    while (vs->num_buffered < vs->maxbuffer && !vs->eof) {
        pthread_mutex_unlock(&vs->lock);
        AVFrame *frame = NULL;
        ret = ff_inlink_consume_frame(inlink, &frame);

        if (ret <= 0) {
            if (ret < 0)
                return ret;
            break;
        }

        pthread_mutex_lock(&vs->lock);

        /* Check for format change */
        if (vs->initialized) {
            if (frame->width != vs->in_width || frame->height != vs->in_height ||
                frame->format != vs->in_fmt) {
                av_log(ctx, AV_LOG_WARNING, "Format change detected\n");
                av_frame_free(&frame);
                vs->eof = 1;
                break;
            }
        }

        vs->buffered[vs->num_buffered++] = frame;
        pthread_cond_broadcast(&vs->vs_wakeup);
    }

    /* Check for input EOF */
    if (ff_inlink_acknowledge_status(inlink, &ret, NULL) && ret == AVERROR_EOF) {
        vs->eof = 1;
        pthread_cond_broadcast(&vs->vs_wakeup);

        if (!vs->initialized) {
            pthread_mutex_unlock(&vs->lock);
            return AVERROR_EOF;
        }
    }

    /* Request more output frames if needed */
    if (vs->initialized) {
        request_vs_frames(vs);
    }

    /* Request more input if buffer has space */
    if (vs->num_buffered < vs->maxbuffer && !vs->eof) {
        ff_inlink_request_frame(inlink);
    }

    pthread_mutex_unlock(&vs->lock);
    return 0;
}

/**
 * Query supported pixel formats.
 */
static int query_formats(AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV420P9,  AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV420P14, AV_PIX_FMT_YUV420P16,
        AV_PIX_FMT_YUV422P9,  AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV422P16,
        AV_PIX_FMT_YUV444P9,  AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV444P14, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_NONE
    };

    return ff_set_common_formats2(ctx, cfg_in, cfg_out,
                                  ff_make_format_list(pix_fmts));
}

/**
 * Configure input link.
 */
static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    VSContext *vs = ctx->priv;

    vs->in_width = inlink->w;
    vs->in_height = inlink->h;
    vs->in_fmt = inlink->format;

    if (get_vs_video_format(vs->in_fmt) == pfNone) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input format: %s\n",
               av_get_pix_fmt_name(vs->in_fmt));
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_INFO, "Input configured: %dx%d %s\n",
           vs->in_width, vs->in_height, av_get_pix_fmt_name(vs->in_fmt));

    return 0;
}

/**
 * Configure output link.
 */
static int config_output(AVFilterLink *outlink)
{
    /* Output dimensions are set after VapourSynth initialization in activate() */
    return 0;
}

/**
 * Initialize filter.
 */
static av_cold int init(AVFilterContext *ctx)
{
    VSContext *vs = ctx->priv;

    vs->ctx = ctx;
    vs->first_pts = AV_NOPTS_VALUE;
    vs->next_pts = 0;
    vs->in_frameno = 0;
    vs->out_frameno = 0;
    vs->num_buffered = 0;
    vs->initialized = 0;
    vs->initializing = 0;
    vs->eof = 0;
    vs->failed = 0;
    vs->done = 0;

    /* Allocate buffers */
    vs->buffered = av_calloc(vs->maxbuffer, sizeof(AVFrame *));
    vs->vs_frames = av_calloc(vs->max_requests, sizeof(VSFrame *));
    vs->vs_frame_numbers = av_malloc(vs->max_requests * sizeof(int));
    if (!vs->buffered || !vs->vs_frames || !vs->vs_frame_numbers) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate buffers\n");
        return AVERROR(ENOMEM);
    }
    for (int i = 0; i < vs->max_requests; i++)
        vs->vs_frame_numbers[i] = -1;

    /* Initialize threading */
    pthread_mutex_init(&vs->lock, NULL);
    pthread_cond_init(&vs->vs_wakeup, NULL);
    pthread_cond_init(&vs->input_wakeup, NULL);

    /* Validate script path */
    if (!vs->script_path || !vs->script_path[0]) {
        av_log(ctx, AV_LOG_ERROR,
               "No script file specified (use 'file=script.vpy')\n");
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_INFO, "VapourSynth filter initialized: %s\n", vs->script_path);

    return 0;
}

/**
 * Uninitialize filter.
 */
static av_cold void uninit(AVFilterContext *ctx)
{
    VSContext *vs = ctx->priv;
    int i;

    /* Signal shutdown */
    pthread_mutex_lock(&vs->lock);
    vs->done = 1;
    vs->eof = 1;
    pthread_cond_broadcast(&vs->vs_wakeup);
    pthread_cond_broadcast(&vs->input_wakeup);
    pthread_mutex_unlock(&vs->lock);

    /* Free VapourSynth resources */
    if (vs->vsapi) {
        if (vs->in_node) {
            vs->vsapi->freeNode(vs->in_node);
            vs->in_node = NULL;
        }
        if (vs->out_node) {
            vs->vsapi->freeNode(vs->out_node);
            vs->out_node = NULL;
        }
    }

    if (vs->vs_script_api && vs->vs_script) {
        vs->vs_script_api->freeScript(vs->vs_script);
        vs->vs_script = NULL;
    }

    /* Free buffered frames */
    for (i = 0; i < vs->num_buffered; i++) {
        if (vs->buffered[i])
            av_frame_free(&vs->buffered[i]);
    }

    /* Free VS frames */
    if (vs->vs_frames && vs->vsapi) {
        for (i = 0; i < vs->max_requests; i++) {
            if (vs->vs_frames[i])
                vs->vsapi->freeFrame(vs->vs_frames[i]);
        }
    }

    av_freep(&vs->buffered);
    av_freep(&vs->vs_frames);
    av_freep(&vs->vs_frame_numbers);

    /* Destroy threading */
    pthread_cond_destroy(&vs->input_wakeup);
    pthread_cond_destroy(&vs->vs_wakeup);
    pthread_mutex_destroy(&vs->lock);
}

/**
 * Filter definition.
 */
const FFFilter ff_vf_vapoursynth = {
    .p.name        = "vapoursynth",
    .p.description = NULL_IF_CONFIG_SMALL("Run a VapourSynth script as a filter."),
    .p.priv_class  = &vapoursynth_class,
    .p.flags       = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    .query_formats = query_formats,
    .inputs        = ff_video_default_filterpad,
    .outputs       = ff_video_default_filterpad,
    .priv_size     = sizeof(VSContext),
};
