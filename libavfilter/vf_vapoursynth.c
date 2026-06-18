/*
 * VapourSynth video filter for FFmpeg
 *
 * Copyright (c) 2024 efschu
 *
 * Based on mpv's VapourSynth implementation
 * Direct in-process execution with multithreaded support
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

#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libavutil/eval.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavutil/cpu.h>

#include "libavfilter/avfilter.h"
#include "libavfilter/formats.h"
#include "libavfilter/internal.h"
#include "libavfilter/video.h"
#include "avfilter.h"
#include "formats.h"
#include "video.h"
#include "vf_vapoursynth.h"

/* VapourSynth library names */
static const char *vsscript_lib_names[] = {
#ifdef _WIN32
    "VSScript.dll",
#elif defined(__APPLE__)
    "libvsscript.dylib",
    "libvapoursynth-script.dylib",
#else
    "libvsscript.so",
    "libvapoursynth-script.so",
#endif
};

/* VapourSynth filter options */
#define OFFSET(x) offsetof(VSContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption vs_options[] = {
    { "file", "VapourSynth script file (.vpy)", OFFSET(script_path), AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "maxbuffer", "Maximum number of buffered frames", OFFSET(maxbuffer), AV_OPT_TYPE_INT, .min = 1, .max = VS_MAX_BUFFER, .default_val = 16, .flags = FLAGS },
    { "maxrequests", "Maximum concurrent VS frame requests", OFFSET(max_requests), AV_OPT_TYPE_INT, .min = 1, .max = 32, .default_val = 8, .flags = FLAGS },
    { "threads", "Number of threads for VapourSynth (0=auto)", OFFSET(nb_threads), AV_OPT_TYPE_INT, .min = 0, .max = 64, .default_val = 0, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(vapoursynth);

/* Get VapourSynth format info for FFmpeg pixel format */
static int get_vs_format_info(enum AVPixelFormat pix_fmt, VSVideoFormat *vsfmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    if (!desc)
        return AVERROR(EINVAL);

    memset(vsfmt, 0, sizeof(*vsfmt));

    /* Determine color family */
    if (desc->flags & AV_PIX_FMT_FLAG_RGB) {
        vsfmt->colorFamily = cfRGB;
    } else if (desc->nb_components >= 3) {
        vsfmt->colorFamily = cfYUV;
    } else if (desc->nb_components == 1) {
        vsfmt->colorFamily = cfGray;
    } else {
        return AVERROR(EINVAL);
    }

    /* Sample type */
    vsfmt->bitsPerSample = desc->comp[0].depth > 0 ? desc->comp[0].depth : 8;
    vsfmt->sampleType = vsfmt->bitsPerSample > 8 ? stFloat : stInteger;

    /* Sub-sampling for YUV */
    vsfmt->subSamplingW = desc->log2_chroma_w;
    vsfmt->subSamplingH = desc->log2_chroma_h;

    return 0;
}

/* Convert FFmpeg pixel format to VapourSynth pixel format ID */
static int mp_pix_fmt_to_vs(enum AVPixelFormat pix_fmt)
{
    /* Supported YUV formats */
    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return 1; /* cfYUV, 8-bit, 4:2:0 */
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
        return 2; /* cfYUV, 8-bit, 4:2:2 */
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
        return 3; /* cfYUV, 8-bit, 4:4:4 */
    case AV_PIX_FMT_YUV410P:
        return 4; /* cfYUV, 8-bit, 4:1:0 */
    case AV_PIX_FMT_YUV411P:
        return 5; /* cfYUV, 8-bit, 4:1:1 */
    case AV_PIX_FMT_YUV420P9:
        return 101; /* cfYUV, 9-bit, 4:2:0 */
    case AV_PIX_FMT_YUV422P9:
        return 102; /* cfYUV, 9-bit, 4:2:2 */
    case AV_PIX_FMT_YUV444P9:
        return 103; /* cfYUV, 9-bit, 4:4:4 */
    case AV_PIX_FMT_YUV420P10:
        return 104; /* cfYUV, 10-bit, 4:2:0 */
    case AV_PIX_FMT_YUV422P10:
        return 105; /* cfYUV, 10-bit, 4:2:2 */
    case AV_PIX_FMT_YUV444P10:
        return 106; /* cfYUV, 10-bit, 4:4:4 */
    case AV_PIX_FMT_YUV420P12:
        return 107; /* cfYUV, 12-bit, 4:2:0 */
    case AV_PIX_FMT_YUV422P12:
        return 108; /* cfYUV, 12-bit, 4:2:2 */
    case AV_PIX_FMT_YUV444P12:
        return 109; /* cfYUV, 12-bit, 4:4:4 */
    case AV_PIX_FMT_YUV420P14:
        return 110; /* cfYUV, 14-bit, 4:2:0 */
    case AV_PIX_FMT_YUV422P14:
        return 111; /* cfYUV, 14-bit, 4:2:2 */
    case AV_PIX_FMT_YUV444P14:
        return 112; /* cfYUV, 14-bit, 4:4:4 */
    case AV_PIX_FMT_YUV420P16:
        return 113; /* cfYUV, 16-bit, 4:2:0 */
    case AV_PIX_FMT_YUV422P16:
        return 114; /* cfYUV, 16-bit, 4:2:2 */
    case AV_PIX_FMT_YUV444P16:
        return 115; /* cfYUV, 16-bit, 4:4:4 */
    case AV_PIX_FMT_GRAY8:
        return 0; /* cfGray, 8-bit */
    case AV_PIX_FMT_GRAY16:
        return 100; /* cfGray, 16-bit */
    default:
        return -1;
    }
}

/* Query if format is compatible with VapourSynth */
static int is_vs_compatible(enum AVPixelFormat pix_fmt)
{
    return mp_pix_fmt_to_vs(pix_fmt) >= 0;
}

/* Copy frame data from FFmpeg to VapourSynth frame */
static int copy_frame_to_vs(VSContext *vs, VSFrame *dst, const AVFrame *src)
{
    const VSVideoFormat *vsfmt = vs->vsapi->getVideoFrameFormat(dst);
    int width = vs->vsapi->getFrameWidth(dst, 0);
    int height = vs->vsapi->getFrameHeight(dst, 0);
    int num_planes = vsfmt->numPlanes;

    for (int p = 0; p < num_planes && p < src->nb_samples; p++) {
        uint8_t *dst_ptr = vs->vsapi->getWritePtr(dst, p);
        int dst_stride = vs->vsapi->getStride(dst, p);
        const uint8_t *src_ptr = src->data[p];
        int src_stride = src->linesize[p];

        /* Calculate plane dimensions */
        int plane_w = p == 0 ? width : (width + vsfmt->subSamplingW) >> vsfmt->subSamplingW;
        int plane_h = p == 0 ? height : (height + vsfmt->subSamplingH) >> vsfmt->subSamplingH;
        int bytes_per_sample = (vsfmt->bitsPerSample + 7) >> 3;

        /* Handle stride differences */
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

/* Set frame properties for VapourSynth */
static int set_vs_frame_props(VSContext *vs, VSFrame *frame, const AVFrame *avframe)
{
    VSMap *map = vs->vsapi->getFramePropertiesRW(frame);
    if (!map)
        return AVERROR_UNKNOWN;

    /* Set duration */
    if (avframe->pts != AV_NOPTS_VALUE && avframe->pkt_duration > 0) {
        vs->vsapi->mapSetInt(map, "_DurationNum", avframe->pkt_duration, 0);
        vs->vsapi->mapSetInt(map, "_DurationDen", avframe->time_base.den, 0);
    }

    /* Set SAR */
    if (avframe->sample_aspect_ratio.num > 0 && avframe->sample_aspect_ratio.den > 0) {
        vs->vsapi->mapSetInt(map, "_SARNum", avframe->sample_aspect_ratio.num, 0);
        vs->vsapi->mapSetInt(map, "_SARDen", avframe->sample_aspect_ratio.den, 0);
    }

    /* Set color space */
    if (avframe->colorspace != AVCOL_SPC_UNSPECIFIED) {
        vs->vsapi->mapSetInt(map, "_ColorSpace", avframe->colorspace, 0);
    }

    /* Set color range */
    if (avframe->color_range != AVCOL_RANGE_UNSPECIFIED) {
        vs->vsapi->mapSetInt(map, "_ColorRange",
                             avframe->color_range == AVCOL_RANGE_MPEG ? 1 : 0, 0);
    }

    /* Set field info for interlaced content */
    if (avframe->interlaced_frame) {
        int field = avframe->top_field_first ? 2 : 1;
        vs->vsapi->mapSetInt(map, "_FieldBased", field, 0);
    }

    return 0;
}

/* Copy frame data from VapourSynth to FFmpeg frame */
static int copy_frame_from_vs(VSContext *vs, AVFrame *dst, const VSFrame *src)
{
    const VSVideoFormat *vsfmt = vs->vsapi->getVideoFrameFormat(src);
    int width = vs->vsapi->getFrameWidth(src, 0);
    int height = vs->vsapi->getFrameHeight(src, 0);
    int num_planes = vsfmt->numPlanes;

    /* Determine output format */
    enum AVPixelFormat out_fmt = AV_PIX_FMT_NONE;
    if (vsfmt->colorFamily == cfYUV) {
        int bits = vsfmt->bitsPerSample;
        int sub_w = vsfmt->subSamplingW;
        int sub_h = vsfmt->subSamplingH;

        /* Find matching format */
        if (bits <= 8 && sub_w == 1 && sub_h == 1)
            out_fmt = AV_PIX_FMT_YUV420P;
        else if (bits <= 8 && sub_w == 1 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV422P;
        else if (bits <= 8 && sub_w == 0 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV444P;
        else if (bits == 9 && sub_w == 1 && sub_h == 1)
            out_fmt = AV_PIX_FMT_YUV420P9;
        else if (bits == 9 && sub_w == 1 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV422P9;
        else if (bits == 9 && sub_w == 0 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV444P9;
        else if (bits == 10 && sub_w == 1 && sub_h == 1)
            out_fmt = AV_PIX_FMT_YUV420P10;
        else if (bits == 10 && sub_w == 1 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV422P10;
        else if (bits == 10 && sub_w == 0 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV444P10;
        else if (bits == 12 && sub_w == 1 && sub_h == 1)
            out_fmt = AV_PIX_FMT_YUV420P12;
        else if (bits == 12 && sub_w == 1 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV422P12;
        else if (bits == 12 && sub_w == 0 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV444P12;
        else if (bits == 16 && sub_w == 1 && sub_h == 1)
            out_fmt = AV_PIX_FMT_YUV420P16;
        else if (bits == 16 && sub_w == 1 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV422P16;
        else if (bits == 16 && sub_w == 0 && sub_h == 0)
            out_fmt = AV_PIX_FMT_YUV444P16;
    } else if (vsfmt->colorFamily == cfGray) {
        out_fmt = vsfmt->bitsPerSample <= 8 ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_GRAY16;
    } else if (vsfmt->colorFamily == cfRGB) {
        /* RGB formats */
        out_fmt = vsfmt->bitsPerSample == 8 ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_NONE;
    }

    if (out_fmt == AV_PIX_FMT_NONE) {
        av_log(vs->ctx, AV_LOG_ERROR, "Unsupported VS output format\n");
        return AVERROR(EINVAL);
    }

    /* Ensure output buffer is allocated with correct format */
    if (dst->format != out_fmt || dst->width != width || dst->height != height) {
        av_frame_unref(dst);
        dst->format = out_fmt;
        dst->width = width;
        dst->height = height;
        int ret = av_frame_get_buffer(dst, 0);
        if (ret < 0)
            return ret;
    }

    /* Copy frame data */
    for (int p = 0; p < num_planes && p < dst->nb_samples; p++) {
        const uint8_t *src_ptr = vs->vsapi->getReadPtr(src, p);
        int src_stride = vs->vsapi->getStride(src, p);
        uint8_t *dst_ptr = dst->data[p];
        int dst_stride = dst->linesize[p];

        /* Calculate plane dimensions */
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(out_fmt);
        int plane_w = p == 0 ? width : (width + desc->log2_chroma_w) >> desc->log2_chroma_w;
        int plane_h = p == 0 ? height : (height + desc->log2_chroma_h) >> desc->log2_chroma_h;
        int bytes_per_sample = (vsfmt->bitsPerSample + 7) >> 3;

        /* Handle stride differences */
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

    /* Get frame properties */
    const VSMap *props = vs->vsapi->getFramePropertiesRO(src);
    if (props) {
        int err;

        /* Get duration */
        int64_t num = vs->vsapi->mapGetInt(props, "_DurationNum", 0, &err);
        if (!err) {
            int64_t den = vs->vsapi->mapGetInt(props, "_DurationDen", 0, &err);
            if (!err && den > 0) {
                dst->pkt_duration = num;
                dst->time_base.num = 1;
                dst->time_base.den = den;
            }
        }

        /* Get SAR */
        int64_t sar_num = vs->vsapi->mapGetInt(props, "_SARNum", 0, &err);
        if (!err) {
            int64_t sar_den = vs->vsapi->mapGetInt(props, "_SARDen", 0, &err);
            if (!err) {
                dst->sample_aspect_ratio.num = sar_num;
                dst->sample_aspect_ratio.den = sar_den;
            }
        }

        /* Get colorspace */
        int64_t cs = vs->vsapi->mapGetInt(props, "_ColorSpace", 0, &err);
        if (!err)
            dst->colorspace = cs;

        /* Get color range */
        int64_t cr = vs->vsapi->mapGetInt(props, "_ColorRange", 0, &err);
        if (!err)
            dst->color_range = cr ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
    }

    return 0;
}

/* VapourSynth input filter callback - provides frames to VS */
const VSFrame *VS_CC vs_infilter_get_frame(int frameno, int activationReason,
                                           void *instanceData, void **frameData,
                                           VSFrameContext *frameCtx, VSCore *core,
                                           const VSAPI *vsapi)
{
    VSContext *vs = instanceData;
    AVFilterContext *ctx = vs->ctx;
    VSFrame *ret = NULL;

    pthread_mutex_lock(&vs->lock);

    if (vs->done || vs->failed) {
        vsapi->setFilterError("Filter shutdown", frameCtx);
        pthread_mutex_unlock(&vs->lock);
        return NULL;
    }

    if (vs->initializing) {
        av_log(ctx, AV_LOG_WARNING, "Frame requested during initialization!\n");
        pthread_mutex_unlock(&vs->lock);
        return NULL;
    }

    /* Wait for frame to be available */
    while (1) {
        /* Check if we have the frame in buffer */
        if (frameno >= vs->in_frameno && frameno < vs->in_frameno + vs->num_buffered) {
            AVFrame *avframe = vs->buffered[frameno - vs->in_frameno];

            /* Create VS frame */
            VSVideoFormat vsfmt;
            get_vs_format_info(avframe->format, &vsfmt);
            ret = vsapi->newVideoFrame(&vsfmt, avframe->width, avframe->height, NULL, core);

            if (ret) {
                copy_frame_to_vs(vs, ret, avframe);
                set_vs_frame_props(vs, ret, avframe);
            }
            break;
        }

        /* Check for EOF */
        if (vs->eof) {
            vsapi->setFilterError("EOF", frameCtx);
            break;
        }

        /* Signal we need more frames */
        pthread_cond_broadcast(&vs->input_wakeup);

        /* Wait for more frames or shutdown */
        pthread_cond_wait(&vs->vs_wakeup, &vs->lock);
    }

    pthread_mutex_unlock(&vs->lock);
    return ret;
}

void VS_CC vs_infilter_free(void *instanceData, VSCore *core, const VSAPI *vsapi)
{
    VSContext *vs = instanceData;
    pthread_mutex_lock(&vs->lock);
    pthread_cond_signal(&vs->input_wakeup);
    pthread_mutex_unlock(&vs->lock);
}

/* VapourSynth output callback - receives filtered frames */
void VS_CC vs_frame_done(void *userData, const VSFrame *f, int n,
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
        /* Store VS frame for later conversion */
        /* We keep it alive until ff_vs_map_frame is called */
        int idx = n - vs->out_frameno;
        if (idx >= 0 && idx < vs->max_requests) {
            /* Store the VS frame - it will be freed after conversion */
            vs->vs_frames[idx] = f;
            vs->vs_frame_numbers[idx] = n;
        } else {
            vs->vsapi->freeFrame(f);
        }
    }

    pthread_cond_broadcast(&vs->vs_wakeup);
    pthread_mutex_unlock(&vs->lock);
}

/* Initialize VapourSynth library and core */
static int init_vs_lib(VSContext *vs)
{
    void *vsscript_lib = NULL;
    const char *vsscript_path = getenv("VSSCRIPT_PATH");

    /* Try to load VSScript library */
    if (vsscript_path) {
        vsscript_lib = dlopen(vsscript_path, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!vsscript_lib) {
        for (size_t i = 0; i < FF_ARRAY_ELEMS(vsscript_lib_names); i++) {
            vsscript_lib = dlopen(vsscript_lib_names[i], RTLD_NOW | RTLD_GLOBAL);
            if (vsscript_lib)
                break;
        }
    }

    if (!vsscript_lib) {
        av_log(vs->ctx, AV_LOG_ERROR, "Failed to load VSScript library: %s\n",
               dlerror() ? dlerror() : "unknown");
        return AVERROR(EINVAL);
    }

    /* Get API functions */
    typedef const VSSCRIPTAPI *(VCAPI *getVSScriptAPI_func)(int);
    getVSScriptAPI_func getVSScriptAPI = (getVSScriptAPI_func)dlsym(vsscript_lib, "getVSScriptAPI");
    if (!getVSScriptAPI) {
        av_log(vs->ctx, AV_LOG_ERROR, "Failed to find getVSScriptAPI\n");
        return AVERROR(EINVAL);
    }

    vs->vs_script_api = getVSScriptAPI(VSSCRIPT_API_VERSION);
    if (!vs->vs_script_api) {
        av_log(vs->ctx, AV_LOG_ERROR, "Failed to get VSScript API v%d\n", VSSCRIPT_API_VERSION);
        return AVERROR(EINVAL);
    }

    vs->vsapi = vs->vs_script_api->getVSAPI(VAPOURSYNTH_API_VERSION);
    if (!vs->vsapi) {
        av_log(vs->ctx, AV_LOG_ERROR, "Failed to get VSAPI v%d\n", VAPOURSYNTH_API_VERSION);
        return AVERROR(EINVAL);
    }

    return 0;
}

/* Load VapourSynth script */
static int load_vs_script(VSContext *vs)
{
    AVFilterContext *ctx = vs->ctx;

    /* Create script */
    vs->vs_script = vs->vs_script_api->createScript(NULL);
    if (!vs->vs_script) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create script\n");
        return AVERROR(ENOMEM);
    }

    /* Get core */
    vs->vscore = vs->vs_script_api->getCore(vs->vs_script);
    if (!vs->vscore) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get core\n");
        return AVERROR(ENOMEM);
    }

    /* Set thread count */
    if (vs->nb_threads <= 0)
        vs->nb_threads = av_cpu_count();
    vs->vsapi->setThreadCount(vs->nb_threads, vs->vscore);

    /* Create input node */
    VSVideoInfo vi_in = {
        .width = vs->in_width,
        .height = vs->in_height,
        .numFrames = INT_MAX / 16,
    };
    get_vs_format_info(vs->in_fmt, &vi_in.format);

    vs->in_node = vs->vsapi->createVideoFilter2(
        "FFmpegInput", &vi_in,
        vs_infilter_get_frame, vs_infilter_free,
        fmParallel, NULL, 0, vs, vs->vscore);

    if (!vs->in_node) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create input node\n");
        return AVERROR(EINVAL);
    }

    /* Set up script variables */
    VSMap *vars = vs->vsapi->createMap();
    vs->vsapi->mapSetNode(vars, "clip", vs->in_node, 0);
    vs->vsapi->mapSetInt(vars, "w", vs->in_width, 0);
    vs->vsapi->mapSetInt(vars, "h", vs->in_height, 0);
    vs->vs_script_api->setVariables(vs->vs_script, vars);
    vs->vsapi->freeMap(vars);

    /* Evaluate script */
    if (vs->vs_script_api->evaluateFile(vs->vs_script, vs->script_path)) {
        const char *error = vs->vs_script_api->getError(vs->vs_script);
        av_log(ctx, AV_LOG_ERROR, "Script evaluation failed: %s\n", error ? error : "unknown");
        return AVERROR(EINVAL);
    }

    /* Get output node */
    vs->out_node = vs->vs_script_api->getOutputNode(vs->vs_script, 0);
    if (!vs->out_node) {
        av_log(ctx, AV_LOG_ERROR, "No output node from script\n");
        return AVERROR(EINVAL);
    }

    /* Get output info */
    const VSVideoInfo *vi_out = vs->vsapi->getVideoInfo(vs->out_node);
    vs->out_width = vi_out->width;
    vs->out_height = vi_out->height;

    av_log(ctx, AV_LOG_INFO, "VS initialized: %dx%d -> %dx%d, threads=%d\n",
           vs->in_width, vs->in_height, vs->out_width, vs->out_height, vs->nb_threads);

    return 0;
}

/* Request frames from VapourSynth asynchronously */
static void request_vs_frames(VSContext *vs)
{
    if (!vs->out_node || vs->eof || vs->failed)
        return;

    for (int i = 0; i < vs->max_requests; i++) {
        if (!vs->vs_frames[i]) {
            int frame_num = vs->out_frameno + i;
            vs->vsapi->getFrameAsync(frame_num, vs->out_node, vs_frame_done, vs);
        }
    }
}

/* Check if we have a completed frame ready */
static int get_ready_frame(VSContext *vs, AVFrame **out_frame)
{
    for (int i = 0; i < vs->max_requests; i++) {
        if (vs->vs_frames[i]) {
            int frame_num = vs->vs_frame_numbers[i];

            /* Allocate output frame */
            AVFrame *out = av_frame_alloc();
            if (!out)
                return AVERROR(ENOMEM);

            /* Copy from VS frame */
            int ret = copy_frame_from_vs(vs, out, vs->vs_frames[i]);
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

/* Filter activation function */
static int activate(AVFilterContext *ctx)
{
    VSContext *vs = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;

    pthread_mutex_lock(&vs->lock);

    /* First-time initialization */
    if (!vs->in_node && !vs->initializing) {
        AVFrame *first_frame = NULL;

        /* Check if we have a frame to init with */
        if (vs->num_buffered > 0) {
            first_frame = vs->buffered[0];
        } else {
            /* Request input */
            pthread_mutex_unlock(&vs->lock);
            ret = ff_inlink_consume_frame(inlink, &first_frame);
            if (ret <= 0)
                return ret;

            pthread_mutex_lock(&vs->lock);
        }

        if (first_frame) {
            vs->in_width = first_frame->width;
            vs->in_height = first_frame->height;
            vs->in_fmt = first_frame->format;

            /* Move to buffer */
            if (vs->num_buffered == 0) {
                vs->buffered[0] = first_frame;
                vs->num_buffered = 1;
            }

            /* Initialize VapourSynth */
            vs->initializing = 1;
            pthread_mutex_unlock(&vs->lock);

            ret = init_vs_lib(vs);
            if (ret >= 0) {
                ret = load_vs_script(vs);
            }

            pthread_mutex_lock(&vs->lock);
            vs->initializing = 0;

            if (ret < 0) {
                pthread_mutex_unlock(&vs->lock);
                return ret;
            }

            /* Request initial frames */
            request_vs_frames(vs);
        }
    }

    /* Check for ready output frame */
    if (vs->out_node) {
        AVFrame *out_frame = NULL;
        pthread_mutex_unlock(&vs->lock);

        ret = get_ready_frame(vs, &out_frame);
        if (ret == 0 && out_frame) {
            /* Set output PTS */
            if (vs->first_pts == AV_NOPTS_VALUE)
                vs->first_pts = out_frame->pts;
            out_frame->pts = vs->next_pts;
            if (out_frame->pkt_duration > 0)
                vs->next_pts += out_frame->pkt_duration;

            return ff_filter_frame(outlink, out_frame);
        }

        pthread_mutex_lock(&vs->lock);

        /* Check for EOF */
        if (vs->eof && vs->num_buffered == 0) {
            /* Wait for all pending frames */
            int pending = 0;
            for (int i = 0; i < vs->max_requests; i++)
                pending += vs->vs_frames[i] != NULL;

            if (pending == 0) {
                pthread_mutex_unlock(&vs->lock);
                return ff_filter_frame(outlink, EOF_FRAME);
            }
        }
    }

    /* Consume input frames */
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

        if (frame->width != vs->in_width || frame->height != vs->in_height ||
            frame->format != vs->in_fmt) {
            /* Format change - would need reinit */
            av_log(ctx, AV_LOG_WARNING, "Format change detected, draining...\n");
            vs->eof = 1;
            av_frame_free(&frame);
            break;
        }

        vs->buffered[vs->num_buffered++] = frame;
        pthread_cond_broadcast(&vs->vs_wakeup);
    }

    /* Check for EOF on input */
    if (ff_inlink_acknowledge_status(inlink, &ret, NULL) && ret == AVERROR_EOF) {
        vs->eof = 1;
        pthread_cond_broadcast(&vs->vs_wakeup);

        if (!vs->out_node) {
            pthread_mutex_unlock(&vs->lock);
            return ff_filter_frame(outlink, EOF_FRAME);
        }
    }

    /* Request more frames */
    request_vs_frames(vs);

    /* Signal we need input */
    if (vs->num_buffered < vs->maxbuffer && !vs->eof) {
        ff_inlink_request_frame(inlink);
    }

    pthread_mutex_unlock(&vs->lock);
    return 0;
}

/* Query supported formats */
static int query_formats(AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV420P14, AV_PIX_FMT_YUV420P16,
        AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV422P16,
        AV_PIX_FMT_YUV444P9, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV444P14, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_NONE
    };

    return ff_set_common_formats2(ctx, cfg_in, cfg_out,
                                  ff_make_format_list(pix_fmts));
}

/* Configure input link */
static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    VSContext *vs = ctx->priv;

    vs->in_width = inlink->w;
    vs->in_height = inlink->h;
    vs->in_fmt = inlink->format;

    if (!is_vs_compatible(vs->in_fmt)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input format: %s\n",
               av_get_pix_fmt_name(vs->in_fmt));
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_INFO, "Input configured: %dx%d %s\n",
           vs->in_width, vs->in_height, av_get_pix_fmt_name(vs->in_fmt));

    return 0;
}

/* Configure output link */
static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VSContext *vs = ctx->priv;

    /* Output dimensions will be set after VS init */
    outlink->w = vs->out_width;
    outlink->h = vs->out_height;

    return 0;
}

/* Initialize filter */
static av_cold int init(AVFilterContext *ctx)
{
    VSContext *vs = ctx->priv;

    vs->ctx = ctx;
    vs->maxbuffer = 16;
    vs->max_requests = 8;
    vs->nb_threads = 0;
    vs->first_pts = AV_NOPTS_VALUE;
    vs->next_pts = 0;
    vs->in_frameno = 0;
    vs->out_frameno = 0;
    vs->num_buffered = 0;

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
        av_log(ctx, AV_LOG_ERROR, "No script file specified (use 'file=script.vpy')\n");
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_INFO, "VapourSynth filter initialized: %s\n", vs->script_path);

    return 0;
}

/* Uninitialize filter */
static av_cold void uninit(AVFilterContext *ctx)
{
    VSContext *vs = ctx->priv;
    int i;

    pthread_mutex_lock(&vs->lock);
    vs->done = 1;
    vs->eof = 1;
    pthread_cond_broadcast(&vs->vs_wakeup);
    pthread_cond_broadcast(&vs->input_wakeup);
    pthread_mutex_unlock(&vs->lock);

    /* Free VS resources */
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
    for (i = 0; i < vs->max_requests; i++) {
        if (vs->vs_frames[i])
            vs->vsapi->freeFrame(vs->vs_frames[i]);
    }

    av_freep(&vs->buffered);
    av_freep(&vs->vs_frames);
    av_freep(&vs->vs_frame_numbers);

    /* Destroy threading */
    pthread_cond_destroy(&vs->input_wakeup);
    pthread_cond_destroy(&vs->vs_wakeup);
    pthread_mutex_destroy(&vs->lock);
}

/* Input pad */
static const AVFilterPad inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = NULL,
        .config_props = config_input,
    },
};

/* Output pad */
static const AVFilterPad outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

const AVFilter ff_vf_vapoursynth = {
    .p.name = "vapoursynth",
    .p.description = "VapourSynth video filter for in-process filter execution",
    .p.priv_class = &vapoursynth_class,
    .init = init,
    .uninit = uninit,
    .activate = activate,
    .query_formats = query_formats,
    .p.flags = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .inputs = inputs,
    .outputs = outputs,
    .priv_size = sizeof(VSContext),
};
