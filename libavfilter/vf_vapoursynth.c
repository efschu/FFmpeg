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
#include "libavutil/thread.h"

#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"
#include "vf_vapoursynth.h"

/* FFmpeg 6.0+ uses FILTER_INOUTPADS, FILTER_INPUTS, FILTER_OUTPUTS macros */
#ifndef FILTER_INPUTS
#define FILTER_INPUTS(array)  .p.inout = (array), .nb_inputs = FF_ARRAY_ELEMS(array)
#endif
#ifndef FILTER_OUTPUTS
#define FILTER_OUTPUTS(array) .p.inout = (array), .nb_outputs = FF_ARRAY_ELEMS(array)
#endif

#define OFFSET(x) offsetof(VSContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

/* Filter options - need to be named vs_options to match AVFILTER_DEFINE_CLASS */
static const AVOption vapoursynth_options[] = {
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
 * Convert VapourSynth video format to FFmpeg pixel format.
 * Uses the colorFamily, sampleType, bitsPerSample, subSamplingW, subSamplingH
 * fields from VSVideoFormat.
 */
static enum AVPixelFormat vs_to_ff_pix_fmt(const VSVideoFormat *vsfmt)
{
    if (vsfmt->colorFamily == cfGray) {
        if (vsfmt->sampleType == stInteger) {
            if (vsfmt->bitsPerSample == 8) return AV_PIX_FMT_GRAY8;
            if (vsfmt->bitsPerSample == 16) return AV_PIX_FMT_GRAY16;
        } else if (vsfmt->sampleType == stFloat) {
            if (vsfmt->bitsPerSample == 16) return AV_PIX_FMT_GRAYF16;
            if (vsfmt->bitsPerSample == 32) return AV_PIX_FMT_GRAYF32;
        }
        return AV_PIX_FMT_NONE;
    }

    if (vsfmt->colorFamily == cfRGB) {
        if (vsfmt->sampleType == stInteger) {
            if (vsfmt->bitsPerSample == 8)  return AV_PIX_FMT_RGB24;
            if (vsfmt->bitsPerSample == 10) return AV_PIX_FMT_RGB48;
        }
        return AV_PIX_FMT_NONE;
    }

    if (vsfmt->colorFamily == cfYUV) {
        if (vsfmt->sampleType == stInteger) {
            if (vsfmt->subSamplingW == 1 && vsfmt->subSamplingH == 1) {
                if (vsfmt->bitsPerSample == 8)  return AV_PIX_FMT_YUV420P;
                if (vsfmt->bitsPerSample == 9)  return AV_PIX_FMT_YUV420P9;
                if (vsfmt->bitsPerSample == 10) return AV_PIX_FMT_YUV420P10;
                if (vsfmt->bitsPerSample == 12) return AV_PIX_FMT_YUV420P12;
                if (vsfmt->bitsPerSample == 14) return AV_PIX_FMT_YUV420P14;
                if (vsfmt->bitsPerSample == 16) return AV_PIX_FMT_YUV420P16;
            } else if (vsfmt->subSamplingW == 1 && vsfmt->subSamplingH == 0) {
                if (vsfmt->bitsPerSample == 8)  return AV_PIX_FMT_YUV422P;
                if (vsfmt->bitsPerSample == 9)  return AV_PIX_FMT_YUV422P9;
                if (vsfmt->bitsPerSample == 10) return AV_PIX_FMT_YUV422P10;
                if (vsfmt->bitsPerSample == 12) return AV_PIX_FMT_YUV422P12;
                if (vsfmt->bitsPerSample == 14) return AV_PIX_FMT_YUV422P14;
                if (vsfmt->bitsPerSample == 16) return AV_PIX_FMT_YUV422P16;
            } else if (vsfmt->subSamplingW == 0 && vsfmt->subSamplingH == 0) {
                if (vsfmt->bitsPerSample == 8)  return AV_PIX_FMT_YUV444P;
                if (vsfmt->bitsPerSample == 9)  return AV_PIX_FMT_YUV444P9;
                if (vsfmt->bitsPerSample == 10) return AV_PIX_FMT_YUV444P10;
                if (vsfmt->bitsPerSample == 12) return AV_PIX_FMT_YUV444P12;
                if (vsfmt->bitsPerSample == 14) return AV_PIX_FMT_YUV444P14;
                if (vsfmt->bitsPerSample == 16) return AV_PIX_FMT_YUV444P16;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    return AV_PIX_FMT_NONE;
}

/**
 * Copy frame data from FFmpeg AVFrame to VapourSynth VSFrame.
 */
/**
 * Get the number of planes for a frame (using its format descriptor).
 */
static int get_frame_plane_count(const VSAPI *vsapi, const VSFrame *f)
{
    const VSVideoFormat *fmt = vsapi->getVideoFrameFormat(f);
    return fmt->numPlanes;
}

static int copy_frame_to_vs(const VSAPI *vsapi, VSFrame *dst,
                            const AVFrame *src)
{
    const VSVideoFormat *vsfmt = vsapi->getVideoFrameFormat(dst);
    int width = vsapi->getFrameWidth(dst, 0);
    int height = vsapi->getFrameHeight(dst, 0);
    int num_planes = get_frame_plane_count(vsapi, dst);
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
 * Copy frame data from VapourSynth VSFrame to FFmpeg AVFrame.
 */
static int copy_frame_from_vs(const VSAPI *vsapi, AVFrame *dst,
                              const VSFrame *src)
{
    const VSVideoFormat *vsfmt = vsapi->getVideoFrameFormat(src);
    int width = vsapi->getFrameWidth(src, 0);
    int height = vsapi->getFrameHeight(src, 0);
    int num_planes = get_frame_plane_count(vsapi, src);
    int bytes_per_sample = (vsfmt->bitsPerSample + 7) >> 3;

    enum AVPixelFormat out_fmt = vs_to_ff_pix_fmt(vsfmt);
    if (out_fmt == AV_PIX_FMT_NONE) {
        return AVERROR(EINVAL);
    }

    if (dst->format != (int)out_fmt || dst->width != width || dst->height != height) {
        av_frame_unref(dst);
        dst->format = out_fmt;
        dst->width = width;
        dst->height = height;
        int ret = av_frame_get_buffer(dst, 0);
        if (ret < 0)
            return ret;
    }

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
static const VSFrame *VS_CC vs_infilter_get_frame(int n, int activationReason,
                                                  void *instanceData, void **frameData,
                                                  VSFrameContext *frameCtx, VSCore *core,
                                                  const VSAPI *vsapi)
{
    VSContext *vs = (VSContext *)instanceData;
    VSFrame *ret = NULL;

    pthread_mutex_lock(&vs->lock);

    if (vs->done || vs->failed) {
        vsapi->setFilterError("Filter shutdown", frameCtx);
        pthread_mutex_unlock(&vs->lock);
        return NULL;
    }

    /* Check if requested frame is available in buffer */
    if (n >= vs->in_frameno && n < vs->in_frameno + vs->num_buffered) {
        AVFrame *avframe = vs->buffered[n - vs->in_frameno];

        /* Convert pixel format to VapourSynth */
        int vs_format = 0;
        if (avframe->format == AV_PIX_FMT_YUV420P) vs_format = pfYUV420P8;
        else if (avframe->format == AV_PIX_FMT_YUV422P) vs_format = pfYUV422P8;
        else if (avframe->format == AV_PIX_FMT_YUV444P) vs_format = pfYUV444P8;
        else if (avframe->format == AV_PIX_FMT_YUV420P10) vs_format = pfYUV420P10;
        else if (avframe->format == AV_PIX_FMT_YUV420P12) vs_format = pfYUV420P12;
        else if (avframe->format == AV_PIX_FMT_YUV422P10) vs_format = pfYUV422P10;
        else if (avframe->format == AV_PIX_FMT_YUV444P10) vs_format = pfYUV444P10;
        else if (avframe->format == AV_PIX_FMT_GRAY8) vs_format = pfGray8;
        else if (avframe->format == AV_PIX_FMT_GRAY16) vs_format = pfGray16;

        if (vs_format == 0) {
            vsapi->setFilterError("Unsupported pixel format", frameCtx);
            pthread_mutex_unlock(&vs->lock);
            return NULL;
        }

        /* Get the format descriptor */
        VSVideoFormat vsfmt;
        if (!vsapi->getVideoFormatByID(&vsfmt, vs_format, core)) {
            vsapi->setFilterError("Cannot get format descriptor", frameCtx);
            pthread_mutex_unlock(&vs->lock);
            return NULL;
        }

        ret = vsapi->newVideoFrame(&vsfmt, avframe->width, avframe->height, NULL, core);
        if (ret) {
            copy_frame_to_vs(vsapi, ret, avframe);
        }
    } else if (vs->eof) {
        vsapi->setFilterError("End of file", frameCtx);
    } else {
        /* Wait for the requested frame */
        while (!vs->done && !vs->failed && !vs->eof) {
            if (n < vs->in_frameno + vs->num_buffered)
                break;
            pthread_cond_broadcast(&vs->input_wakeup);
            pthread_cond_wait(&vs->vs_wakeup, &vs->lock);
        }

        if (n >= vs->in_frameno && n < vs->in_frameno + vs->num_buffered) {
            /* Re-attempt after wakeup */
            AVFrame *avframe = vs->buffered[n - vs->in_frameno];

            int vs_format = 0;
            if (avframe->format == AV_PIX_FMT_YUV420P) vs_format = pfYUV420P8;
            else if (avframe->format == AV_PIX_FMT_YUV420P10) vs_format = pfYUV420P10;

            if (vs_format) {
                VSVideoFormat vsfmt;
                if (vsapi->getVideoFormatByID(&vsfmt, vs_format, core)) {
                    ret = vsapi->newVideoFrame(&vsfmt, avframe->width, avframe->height, NULL, core);
                    if (ret) {
                        copy_frame_to_vs(vsapi, ret, avframe);
                    }
                }
            }
        } else if (vs->eof) {
            vsapi->setFilterError("End of file", frameCtx);
        }
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
    VSContext *vs = (VSContext *)instanceData;
    pthread_mutex_lock(&vs->lock);
    pthread_cond_signal(&vs->input_wakeup);
    pthread_mutex_unlock(&vs->lock);
}

/**
 * VapourSynth frame completion callback.
 */
static void VS_CC vs_frame_done(void *userData, const VSFrame *f, int n,
                                VSNode *node, const char *errorMsg)
{
    VSContext *vs = (VSContext *)userData;
    pthread_mutex_lock(&vs->lock);

    if (errorMsg && !f) {
        vs->failed = 1;
        pthread_cond_broadcast(&vs->vs_wakeup);
        pthread_mutex_unlock(&vs->lock);
        return;
    }

    if (f) {
        int idx = n - vs->out_frameno;
        if (idx >= 0 && idx < vs->max_requests) {
            if (vs->vs_frames[idx])
                vs->vsapi->freeFrame(vs->vs_frames[idx]);
            vs->vs_frames[idx] = (VSFrame *)f;
            vs->vs_frame_numbers[idx] = n;
        } else {
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

    /* Try common library names */
    const char *lib_names[] = {
        "libvapoursynth-script.so",
        "libvsscript.so",
        NULL
    };

    for (int i = 0; lib_names[i]; i++) {
        vsscript_lib = dlopen(lib_names[i], RTLD_NOW | RTLD_GLOBAL);
        if (vsscript_lib) break;
    }

    if (!vsscript_lib) {
        av_log(vs->ctx, AV_LOG_ERROR, "Cannot open VapourSynth script library: %s\n", dlerror());
        return AVERROR(EINVAL);
    }

    /* Get VSScript API */
    int (*getAPIVersion)(void) = dlsym(vsscript_lib, "vsscriptGetAPIVersion");
    if (!getAPIVersion) {
        av_log(vs->ctx, AV_LOG_ERROR, "Cannot find vsscriptGetAPIVersion\n");
        return AVERROR(EINVAL);
    }

    typedef const VSSCRIPTAPI *(*getVSScriptAPI_t)(int version);
    getVSScriptAPI_t getVSScriptAPI = dlsym(vsscript_lib, "getVSScriptAPI");
    if (!getVSScriptAPI) {
        av_log(vs->ctx, AV_LOG_ERROR, "Cannot find getVSScriptAPI\n");
        return AVERROR(EINVAL);
    }

    vs->vs_script_api = getVSScriptAPI(VSSCRIPT_API_VERSION);
    if (!vs->vs_script_api) {
        av_log(vs->ctx, AV_LOG_ERROR, "VSScript API version not supported\n");
        return AVERROR(EINVAL);
    }

    /* Get VSAPI from VSScript API */
    vs->vsapi = vs->vs_script_api->getVSAPI(VAPOURSYNTH_API_VERSION);
    if (!vs->vsapi) {
        av_log(vs->ctx, AV_LOG_ERROR, "VSAPI version not supported\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

/**
 * Load and evaluate the VapourSynth script.
 */
static int load_vs_script(VSContext *vs)
{
    AVFilterContext *ctx = vs->ctx;
    int ret;

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
    int in_vs_format = 0;
    if (vs->in_fmt == AV_PIX_FMT_YUV420P) in_vs_format = pfYUV420P8;
    else if (vs->in_fmt == AV_PIX_FMT_YUV420P10) in_vs_format = pfYUV420P10;
    else if (vs->in_fmt == AV_PIX_FMT_YUV420P12) in_vs_format = pfYUV420P12;
    else if (vs->in_fmt == AV_PIX_FMT_YUV422P) in_vs_format = pfYUV422P8;
    else if (vs->in_fmt == AV_PIX_FMT_YUV444P) in_vs_format = pfYUV444P8;
    else if (vs->in_fmt == AV_PIX_FMT_GRAY8) in_vs_format = pfGray8;
    else if (vs->in_fmt == AV_PIX_FMT_GRAY16) in_vs_format = pfGray16;

    if (in_vs_format == 0) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input pixel format\n");
        return AVERROR(EINVAL);
    }

    VSVideoFormat vi_in_fmt;
    if (!vs->vsapi->getVideoFormatByID(&vi_in_fmt, in_vs_format, vs->vscore)) {
        av_log(ctx, AV_LOG_ERROR, "Cannot get format descriptor for input\n");
        return AVERROR(EINVAL);
    }

    VSVideoInfo vi_in;
    vi_in.format = vi_in_fmt;
    vi_in.fpsNum = 0;
    vi_in.fpsDen = 1;
    vi_in.width = vs->in_width;
    vi_in.height = vs->in_height;
    vi_in.numFrames = INT_MAX;

    vs->in_node = vs->vsapi->createVideoFilter2(
        "FFmpegInput", &vi_in,
        vs_infilter_get_frame, vs_infilter_free,
        fmParallel, NULL, 0, vs, vs->vscore);

    if (!vs->in_node) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create VapourSynth input node\n");
        return AVERROR(EINVAL);
    }

    /* Set up script variables */
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
            vs->vsapi->getFrameAsync(frame_num, vs->out_node, vs_frame_done, vs);
        }
    }
}

/**
 * Get a ready frame from the buffer.
 */
static int get_ready_frame(VSContext *vs, AVFrame **out_frame)
{
    for (int i = 0; i < vs->max_requests; i++) {
        if (vs->vs_frames[i]) {
            const VSFrame *src = vs->vs_frames[i];

            AVFrame *out = av_frame_alloc();
            if (!out)
                return AVERROR(ENOMEM);

            int ret = copy_frame_from_vs(vs->vsapi, out, src);
            int frame_num = vs->vs_frame_numbers[i];
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

static int pending_frames(VSContext *vs)
{
    for (int i = 0; i < vs->max_requests; i++) {
        if (vs->vs_frames[i])
            return 1;
    }
    return 0;
}

/**
 * Filter activation function - called when FFmpeg wants frames.
 */
static int activate(AVFilterContext *ctx)
{
    VSContext *vs = (VSContext *)ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;

    pthread_mutex_lock(&vs->lock);

    /* First-time initialization */
    if (!vs->initialized && !vs->initializing && !vs->failed) {
        AVFrame *first_frame = NULL;

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

            if (vs->num_buffered == 0) {
                vs->buffered[0] = first_frame;
                vs->num_buffered = 1;
            }

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
                return ret;
            }

            vs->initialized = 1;

            /* Notify that output format changed */
            outlink->w = vs->out_width;
            outlink->h = vs->out_height;
            const VSVideoInfo *vi_out = vs->vsapi->getVideoInfo(vs->out_node);
            enum AVPixelFormat pix_fmt = vs_to_ff_pix_fmt(&vi_out->format);
            if (pix_fmt != AV_PIX_FMT_NONE) {
                outlink->format = pix_fmt;
            }

            request_vs_frames(vs);
        }
    }

    /* Try to output ready frames */
    if (vs->initialized) {
        AVFrame *out_frame = NULL;
        pthread_mutex_unlock(&vs->lock);

        ret = get_ready_frame(vs, &out_frame);
        if (ret == 0 && out_frame) {
            if (vs->first_pts == AV_NOPTS_VALUE)
                vs->first_pts = out_frame->pts;
            if (out_frame->pts == AV_NOPTS_VALUE)
                out_frame->pts = vs->next_pts;
            vs->next_pts = out_frame->pts + 1;

            return ff_filter_frame(outlink, out_frame);
        }

        pthread_mutex_lock(&vs->lock);

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

    if (vs->initialized) {
        request_vs_frames(vs);
    }

    if (vs->num_buffered < vs->maxbuffer && !vs->eof) {
        ff_inlink_request_frame(inlink);
    }

    pthread_mutex_unlock(&vs->lock);
    return 0;
}

/**
 * Query supported formats (modern FFmpeg 6.0+).
 */
static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,  AV_PIX_FMT_YUV420P10,  AV_PIX_FMT_YUV420P12,
        AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV422P10,
        AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_GRAY8,     AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_NONE
    };

    return ff_set_common_formats2(ctx, cfg_in, cfg_out,
                                  ff_make_format_list(pix_fmts));
}

/**
 * Initialize filter.
 */
static av_cold int init(AVFilterContext *ctx)
{
    VSContext *vs = (VSContext *)ctx->priv;

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

    if (!vs->script_path || !vs->script_path[0]) {
        av_log(ctx, AV_LOG_ERROR, "No script file specified (use 'file=script.vpy')\n");
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
    VSContext *vs = (VSContext *)ctx->priv;
    int i;

    pthread_mutex_lock(&vs->lock);
    vs->done = 1;
    vs->eof = 1;
    pthread_cond_broadcast(&vs->vs_wakeup);
    pthread_cond_broadcast(&vs->input_wakeup);
    pthread_mutex_unlock(&vs->lock);

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

    for (i = 0; i < vs->num_buffered; i++) {
        if (vs->buffered[i])
            av_frame_free(&vs->buffered[i]);
    }

    if (vs->vs_frames && vs->vsapi) {
        for (i = 0; i < vs->max_requests; i++) {
            if (vs->vs_frames[i])
                vs->vsapi->freeFrame(vs->vs_frames[i]);
        }
    }

    av_freep(&vs->buffered);
    av_freep(&vs->vs_frames);
    av_freep(&vs->vs_frame_numbers);

    pthread_cond_destroy(&vs->input_wakeup);
    pthread_cond_destroy(&vs->vs_wakeup);
    pthread_mutex_destroy(&vs->lock);
}

/**
 * Filter definition - modern FFmpeg 6.0+ style.
 */
const FFFilter ff_vf_vapoursynth = {
    .p.name        = "vapoursynth",
    .p.description = NULL_IF_CONFIG_SMALL("Run a VapourSynth script as a filter."),
    .p.priv_class  = &vapoursynth_class,
    .p.flags       = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    FILTER_INPUTS(ff_video_default_filterpad),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    .formats_state = FF_FILTER_FORMATS_QUERY_FUNC2,
    .query_func2   = query_formats,
    .priv_size     = sizeof(VSContext),
};
